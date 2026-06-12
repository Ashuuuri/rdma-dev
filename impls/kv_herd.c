#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "kv.h"

// ── server-side open-addressing hash table ────────────────────────────────────

#define HERD_TABLE_BITS 17
#define HERD_TABLE_SIZE (1 << HERD_TABLE_BITS)  // 131072 entries
#define HERD_PROBE_LIMIT 32

struct herd_entry {
    uint8_t  occupied;
    uint8_t  _pad[7];
    uint64_t key;
    uint8_t  value[KV_VALUE_SIZE];
};  // 112 bytes

static uint32_t herd_hash(uint64_t key)
{
    return (uint32_t)((key * 11400714819323198485ULL) >> (64 - HERD_TABLE_BITS));
}

static int herd_lookup(struct herd_entry *table, uint64_t key, void *val_out)
{
    uint32_t idx = herd_hash(key);
    for (int i = 0; i < HERD_PROBE_LIMIT; i++) {
        uint32_t pos = (idx + (uint32_t)i) & (HERD_TABLE_SIZE - 1);
        if (!table[pos].occupied) return 1;
        if (table[pos].key == key) {
            if (val_out) memcpy(val_out, table[pos].value, KV_VALUE_SIZE);
            return 0;
        }
    }
    return 1;
}

static int herd_insert(struct herd_entry *table, uint64_t key, const void *val)
{
    uint32_t idx = herd_hash(key);
    for (int i = 0; i < HERD_PROBE_LIMIT; i++) {
        uint32_t pos = (idx + (uint32_t)i) & (HERD_TABLE_SIZE - 1);
        if (!table[pos].occupied || table[pos].key == key) {
            table[pos].occupied = 1;
            table[pos].key      = key;
            memcpy(table[pos].value, val, KV_VALUE_SIZE);
            return 0;
        }
    }
    return -1;
}

// ── per-client state (server-side) ────────────────────────────────────────────

static struct ibv_qp *g_uc_qps[HERD_MAX_CLIENTS];
static struct ibv_ah *g_ahs[HERD_MAX_CLIENTS];
static uint32_t       g_client_ud_qpns[HERD_MAX_CLIENTS];
static int            g_num_clients;

// ── hooks ─────────────────────────────────────────────────────────────────────

static int herd_pre_exchange(struct connection *conn, struct qp_info *local,
                             int cid, int is_server)
{
    if (cid == 0) {
        // UD QP created once: server sends (nslots=0), client receives (nslots=64)
        int nslots = is_server ? 0 : 64;
        if (rdma_init_ud_qp(conn, sizeof(struct herd_resp), nslots)) {
            fprintf(stderr, "herd: ud qp init failed\n"); return -1;
        }
    }

    if (is_server) {
        g_uc_qps[cid] = rdma_alloc_uc_qp(conn);
        if (!g_uc_qps[cid]) {
            fprintf(stderr, "herd: uc qp alloc failed for cid %d\n", cid); return -1;
        }
        local->uc_qpn = g_uc_qps[cid]->qp_num;
    } else {
        if (rdma_init_uc_qp(conn)) {
            fprintf(stderr, "herd: uc qp init failed\n"); return -1;
        }
        local->uc_qpn = conn->uc_qp->qp_num;
    }
    local->ud_qpn = conn->ud_qp->qp_num;
    return 0;
}

static int herd_post_exchange(struct connection *conn, struct qp_info *remote,
                              int syncfd, int cid, int is_server)
{
    (void)syncfd;

    if (is_server) {
        if (rdma_connect_uc_qp_to(conn, g_uc_qps[cid], remote)) {
            fprintf(stderr, "herd: uc connect failed for cid %d\n", cid); return -1;
        }
        g_ahs[cid] = rdma_alloc_ah(conn, remote);
        if (!g_ahs[cid]) {
            fprintf(stderr, "herd: ah alloc failed for cid %d\n", cid); return -1;
        }
        g_client_ud_qpns[cid] = remote->ud_qpn;
        g_num_clients = cid + 1;
    } else {
        if (rdma_connect_uc_qp(conn, remote)) {
            fprintf(stderr, "herd: uc connect failed\n"); return -1;
        }
        // create AH toward server's UD QP (unused for client, but harmless)
        conn->peer_ah = rdma_alloc_ah(conn, remote);
    }
    return 0;
}

// ── server side ───────────────────────────────────────────────────────────────

static void herd_server_init(struct kv_slot *raw)
{
    memset(raw, 0, HERD_SERVER_REQ_SIZE);
}

static int herd_server_put(struct kv_slot *table, uint64_t key, const void *val)
{
    (void)table; (void)key; (void)val;
    return -1;  // not used; server_run handles everything
}

// Server event loop — three-phase batch:
//   1. Scan all slots, snapshot pending requests, free slots immediately.
//   2. Process all (hash table ops).
//   3. Post all UD SENDs; only the last one is signaled → one CQ poll per batch.
static void herd_server_run(struct connection *conn)
{
    struct herd_entry *kv = calloc(HERD_TABLE_SIZE, sizeof(struct herd_entry));
    if (!kv) { perror("herd: calloc kv table"); return; }

    volatile struct herd_req *reqs =
        (volatile struct herd_req *)(conn->buf + HERD_SERVER_REQ_OFF);
    struct herd_resp *resps =
        (struct herd_resp *)(conn->buf + HERD_SERVER_RESP_OFF);

    int num = g_num_clients;

    struct pending {
        int      cid;
        uint8_t  op;
        uint64_t key;
        uint8_t  val[KV_VALUE_SIZE];
    } batch[HERD_MAX_CLIENTS];

    for (;;) {
        // Phase 1: scan — snapshot every pending slot and free it
        int bsz = 0;
        for (int cid = 0; cid < num; cid++) {
            volatile struct herd_req *req = &reqs[cid];
            if (req->key == 0) continue;
            __sync_synchronize();

            batch[bsz].cid = cid;
            batch[bsz].op  = req->opcode;
            batch[bsz].key = req->key - 1;
            if (req->opcode == HERD_OP_PUT)
                memcpy(batch[bsz].val, (const void *)req->value, KV_VALUE_SIZE);

            req->key = 0;   // free slot; client sees this before we send response
            __sync_synchronize();
            bsz++;
        }
        if (bsz == 0) continue;

        // Phase 2: process — hash table ops into registered resp buffers
        for (int i = 0; i < bsz; i++) {
            int r;
            if (batch[i].op == HERD_OP_GET) {
                r = herd_lookup(kv, batch[i].key, resps[i].value);
            } else if (batch[i].op == HERD_OP_PUT) {
                r = herd_insert(kv, batch[i].key, batch[i].val);
            } else {
                fprintf(stderr, "herd server: unknown opcode %u\n", batch[i].op);
                r = -1;
            }
            resps[i].status = (r == 0) ? KV_MSG_OK : KV_MSG_ERR;
        }

        // Phase 3: burst-send — unsignaled for all but the last, one CQ poll total
        for (int i = 0; i < bsz; i++) {
            int signaled = (i == bsz - 1);
            if (rdma_ud_post_send(conn, g_ahs[batch[i].cid],
                                  g_client_ud_qpns[batch[i].cid],
                                  &resps[i], sizeof(resps[i]), signaled)) {
                fprintf(stderr, "herd server: ud post_send failed\n");
            }
        }
        if (rdma_poll_cq(conn->ud_cq)) {
            fprintf(stderr, "herd server: ud cq poll failed\n");
        }
    }

    free(kv);
}

// ── client side ───────────────────────────────────────────────────────────────

static int herd_rpc(struct connection *conn, struct qp_info *remote,
                    uint8_t opcode, uint64_t key, const void *val_in, void *val_out)
{
    int cid = remote->client_id;
    struct herd_req  *req  = (struct herd_req  *)(conn->buf + HERD_CLIENT_REQ_OFF);
    struct herd_resp  resp;

    req->opcode = opcode;
    req->key    = key + 1;  // +1 so key=0 is safe; server subtracts 1
    if (val_in)
        memcpy(req->value, val_in, KV_VALUE_SIZE);
    else
        memset(req->value, 0, KV_VALUE_SIZE);

    // UC WRITE into server's slot for this client_id
    uint64_t req_addr = remote->addr + HERD_SERVER_REQ_OFF +
                        (uint64_t)cid * sizeof(struct herd_req);
    if (rdma_uc_write(conn, remote->rkey, req_addr, req, sizeof(*req))) return -1;
    if (rdma_ud_recv(conn, &resp)) return -1;

    if (resp.status != KV_MSG_OK) return (opcode == HERD_OP_GET) ? 1 : -1;

    if (opcode == HERD_OP_GET && val_out)
        memcpy(val_out, resp.value, KV_VALUE_SIZE);

    return 0;
}

static int herd_get(struct connection *conn, struct qp_info *remote,
                    uint64_t key, void *val_out)
{
    return herd_rpc(conn, remote, HERD_OP_GET, key, NULL, val_out);
}

static int herd_put(struct connection *conn, struct qp_info *remote,
                    uint64_t key, const void *val)
{
    return herd_rpc(conn, remote, HERD_OP_PUT, key, val, NULL);
}

// ── exported ops ──────────────────────────────────────────────────────────────

const struct kv_ops kv_herd_ops = {
    .name            = "herd",
    .table_size      = sizeof(struct herd_req),
    .server_buf_size = HERD_SERVER_BUF_SIZE,
    .client_buf_size = 0,   // default KV_CLIENT_BUF_SIZE is sufficient
    .num_clients     = 2,   // wait for 2 clients; request region sized for HERD_MAX_CLIENTS
    .skip_rc_qp      = 1,   // HERD uses UC WRITE + UD SEND, not RC QP
    .server_init     = herd_server_init,
    .server_put      = herd_server_put,
    .server_run      = herd_server_run,
    .get             = herd_get,
    .put             = herd_put,
    .pre_exchange    = herd_pre_exchange,
    .post_exchange   = herd_post_exchange,
};
