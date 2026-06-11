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

#define KV_MSG_PUT  1
#define KV_MSG_OK   2
#define KV_MSG_ERR  3

struct kv_msg {
    uint8_t  type;
    uint8_t  _pad[7];
    uint64_t key;
    uint8_t  value[KV_VALUE_SIZE];  // contiguous 96 bytes; server splits on insert
};

struct kv_ops {
    const char *name;
    size_t      table_size;
    void (*server_init)(struct kv_slot *table);
    int  (*server_put) (struct kv_slot *table, uint64_t key, const void *val);
    // GET: two RDMA READs + CRC64 verify; no server CPU.
    // Returns 0 found, 1 not-found, -1 error.
    int  (*get)(struct connection *conn, struct qp_info *remote,
                uint64_t key, void *val_out);
    // PUT: SEND/RECV to server; server does cuckoo insertion.
    // Returns 0 OK, -1 error.
    int  (*put)(struct connection *conn, struct qp_info *remote,
                uint64_t key, const void *val);
};

extern const struct kv_ops kv_pilaf_ops;

// Client buffer layout inside conn->buf:
//   [0,   112)  slot_a  — RDMA READ target for t1 candidate
//   [112, 224)  slot_b  — RDMA READ target for t2 candidate
//   [224, 336)  send    — PUT request  (kv_msg = 112 bytes)
//   [336, 448)  recv    — PUT reply    (kv_msg = 112 bytes)
#define KV_CLIENT_READ_A_OFF  0
#define KV_CLIENT_READ_B_OFF  sizeof(struct kv_slot)
#define KV_CLIENT_SEND_OFF    (2 * sizeof(struct kv_slot))
#define KV_CLIENT_RECV_OFF    (2 * sizeof(struct kv_slot) + sizeof(struct kv_msg))
#define KV_CLIENT_BUF_SIZE    (2 * sizeof(struct kv_slot) + 2 * sizeof(struct kv_msg))

// Server buffer layout inside conn->buf:
//   [0, table_size)    — KV table (RDMA-readable by client)
//   [table_size, +112) — RECV buffer for PUT requests
//   [table_size+112,+112) — SEND buffer for PUT replies
#define KV_TABLE_SIZE       (2 * KV_NUM_BUCKETS * sizeof(struct kv_slot))
#define KV_SERVER_RECV_OFF  KV_TABLE_SIZE
#define KV_SERVER_SEND_OFF  (KV_TABLE_SIZE + sizeof(struct kv_msg))
#define KV_SERVER_BUF_SIZE  (KV_TABLE_SIZE + 2 * sizeof(struct kv_msg))
