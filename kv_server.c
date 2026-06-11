#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "kv.h"

static const struct kv_ops *select_impl(const char *name)
{
    if (strcmp(name, "pilaf") == 0) return &kv_pilaf_ops;
    fprintf(stderr, "unknown impl: %s (choices: pilaf)\n", name);
    return NULL;
}

int main(int argc, char *argv[])
{
    const char *impl_name = (argc >= 2) ? argv[1] : "pilaf";
    const struct kv_ops *ops = select_impl(impl_name);
    if (!ops) return 1;

    struct connection *conn = rdma_init(KV_SERVER_BUF_SIZE);
    if (!conn) return 1;

    // Table occupies the start of conn->buf; expose its address via qp_info.
    struct kv_slot *table = (struct kv_slot *)conn->buf;
    ops->server_init(table);

    struct qp_info local, remote;
    rdma_fill_local_info(conn, &local);
    // Advertise only the table region to clients (not the msg buffers at the end)
    local.rkey = conn->mr->rkey;
    local.addr = (uint64_t)table;

    printf("kv_server: impl=%s  table=%zu KB  port=%d\n",
           ops->name, ops->table_size / 1024, KV_PORT);

    int syncfd = rdma_exchange_server(&local, &remote, KV_PORT);
    if (syncfd < 0) { fprintf(stderr, "exchange failed\n"); return 1; }

    if (rdma_modify_qp(conn, &remote)) return 1;
    if (rdma_barrier(syncfd)) return 1;

    printf("kv_server: ready\n");

    // Event loop: handle PUT requests via two-sided SEND/RECV.
    // Client GET requests arrive as one-sided RDMA READs — no server CPU needed.
    struct kv_msg *req  = (struct kv_msg *)(conn->buf + KV_SERVER_RECV_OFF);
    struct kv_msg *resp = (struct kv_msg *)(conn->buf + KV_SERVER_SEND_OFF);

    for (;;) {
        if (rdma_post_recv(conn, req, sizeof(*req))) break;
        if (rdma_poll_cq(conn->cq)) break;

        if (req->type == KV_MSG_PUT) {
            int r = ops->server_put(table, req->key, req->value);
            resp->type = (r == 0) ? KV_MSG_OK : KV_MSG_ERR;
            if (rdma_post_send(conn, resp, sizeof(*resp))) break;
            if (rdma_poll_cq(conn->cq)) break;
        } else {
            fprintf(stderr, "kv_server: unknown msg type %u\n", req->type);
            break;
        }
    }

    close(syncfd);
    return 0;
}
