#pragma once

#include <stdint.h>
#include <stddef.h>
#include "rdma.h"

#define KV_PORT         12346
#define KV_NUM_BUCKETS  512

#define KV_VALUE_A      48
#define KV_VALUE_B      48
#define KV_VALUE_SIZE   (KV_VALUE_A + KV_VALUE_B)  // 96 bytes

// 112 bytes; checksum first so writer zeroes it before touching key/value
struct kv_slot {
    uint64_t checksum;              // CRC64; 0 = empty/in-progress
    uint64_t key;
    uint8_t  value_a[KV_VALUE_A];
    uint8_t  value_b[KV_VALUE_B];
};

// ── Pilaf two-sided message ───────────────────────────────────────────────────

#define KV_MSG_PUT  1
#define KV_MSG_OK   2
#define KV_MSG_ERR  3

struct kv_msg {
    uint8_t  type;
    uint8_t  _pad[7];
    uint64_t key;
    uint8_t  value[KV_VALUE_SIZE];
};  // 112 bytes

// ── HERD UC/UD messages ───────────────────────────────────────────────────────

#define HERD_OP_EMPTY  0
#define HERD_OP_GET    1
#define HERD_OP_PUT    2

// Client UC-WRITEs this into server's request slot.
// key is placed last so left-to-right DMA ordering guarantees the entire
// struct (opcode + value) is visible before server sees key != 0.
struct herd_req {
    uint8_t  opcode;        // HERD_OP_*
    uint8_t  _pad[7];
    uint8_t  value[KV_VALUE_SIZE];
    uint64_t key;           // last field; 0 = slot is free
};  // 112 bytes

// Server UD-SENDs this back to client
struct herd_resp {
    uint8_t  status;        // KV_MSG_OK or KV_MSG_ERR
    uint8_t  _pad[7];
    uint8_t  value[KV_VALUE_SIZE];
};  // 104 bytes

// ── ops table ────────────────────────────────────────────────────────────────

struct kv_ops {
    const char *name;
    size_t      table_size;
    size_t      server_buf_size;  // 0 → KV_SERVER_BUF_SIZE
    size_t      client_buf_size;  // 0 → KV_CLIENT_BUF_SIZE
    int         num_clients;      // 0/1 = single client; N = wait for N clients
    int         skip_rc_qp;       // 1 = don't bring RC QP to RTS (HERD uses UC/UD only)

    void (*server_init)(struct kv_slot *table);
    int  (*server_put) (struct kv_slot *table, uint64_t key, const void *val);
    // If non-NULL, kv_server calls this instead of the default SEND/RECV loop.
    void (*server_run) (struct connection *conn);

    int  (*get)(struct connection *conn, struct qp_info *remote,
                uint64_t key, void *val_out);
    int  (*put)(struct connection *conn, struct qp_info *remote,
                uint64_t key, const void *val);

    // Called once per client during setup.
    // cid = client index (0..num_clients-1); is_server = 1 on server, 0 on client.
    int  (*pre_exchange) (struct connection *conn, struct qp_info *local,
                          int cid, int is_server);
    int  (*post_exchange)(struct connection *conn, struct qp_info *remote, int syncfd,
                          int cid, int is_server);
};

extern const struct kv_ops kv_pilaf_ops;
extern const struct kv_ops kv_herd_ops;

// ── Pilaf buffer layout ───────────────────────────────────────────────────────

#define KV_CLIENT_READ_A_OFF  0
#define KV_CLIENT_READ_B_OFF  sizeof(struct kv_slot)
#define KV_CLIENT_SEND_OFF    (2 * sizeof(struct kv_slot))
#define KV_CLIENT_RECV_OFF    (2 * sizeof(struct kv_slot) + sizeof(struct kv_msg))
#define KV_CLIENT_BUF_SIZE    (2 * sizeof(struct kv_slot) + 2 * sizeof(struct kv_msg))

#define KV_TABLE_SIZE         (2 * KV_NUM_BUCKETS * sizeof(struct kv_slot))
#define KV_SERVER_RECV_OFF    KV_TABLE_SIZE
#define KV_SERVER_SEND_OFF    (KV_TABLE_SIZE + sizeof(struct kv_msg))
#define KV_SERVER_BUF_SIZE    (KV_TABLE_SIZE + 2 * sizeof(struct kv_msg))

// ── HERD buffer layout ────────────────────────────────────────────────────────

// One slot per client; sized at compile time for HERD_MAX_CLIENTS
#define HERD_MAX_CLIENTS      64
#define HERD_SERVER_REQ_SIZE  (HERD_MAX_CLIENTS * sizeof(struct herd_req))

// Server: [MAX_CLIENTS req slots | MAX_CLIENTS resp bufs]
// Resp bufs are written before batch UD SENDs; one per concurrent slot.
#define HERD_SERVER_REQ_OFF   0
#define HERD_SERVER_RESP_OFF  HERD_SERVER_REQ_SIZE
#define HERD_SERVER_RESP_SIZE (HERD_MAX_CLIENTS * sizeof(struct herd_resp))
#define HERD_SERVER_BUF_SIZE  (HERD_SERVER_REQ_SIZE + HERD_SERVER_RESP_SIZE)

// Client: [req preparation buf]  (fits within KV_CLIENT_BUF_SIZE)
#define HERD_CLIENT_REQ_OFF   0
