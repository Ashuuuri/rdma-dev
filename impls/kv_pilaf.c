#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include "kv.h"

// ── CRC64 (poly 0xad93d23594c935a9) ──────────────────────────────────────────

static uint64_t crc64_update(uint64_t crc, const void *data, size_t len)
{
    static const uint64_t poly = 0xad93d23594c935a9ULL;
    const uint8_t *p = data;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint64_t)p[i] << 56;
        for (int j = 0; j < 8; j++)
            crc = (crc & 0x8000000000000000ULL) ? (crc << 1) ^ poly : (crc << 1);
    }
    return crc;
}

// Compute CRC64 over key + value_a + value_b (excluding the checksum field).
// Returns non-zero (0 reserved for empty/in-progress).
static uint64_t slot_checksum(const struct kv_slot *s)
{
    uint64_t crc = 0xFFFFFFFFFFFFFFFFULL;
    crc = crc64_update(crc, &s->key,    sizeof(s->key));
    crc = crc64_update(crc, s->value_a, KV_VALUE_A);
    crc = crc64_update(crc, s->value_b, KV_VALUE_B);
    crc ^= 0xFFFFFFFFFFFFFFFFULL;
    return crc ? crc : 1ULL;
}

static int slot_valid(const struct kv_slot *s)
{
    if (!s->checksum) return 0;
    return s->checksum == slot_checksum(s);
}

// Copy logical value (value_a ++ value_b) into a flat buffer.
static void slot_get_value(const struct kv_slot *s, void *out)
{
    memcpy(out,                        s->value_a, KV_VALUE_A);
    memcpy((uint8_t *)out + KV_VALUE_A, s->value_b, KV_VALUE_B);
}

// Split a flat 96-byte buffer into value_a and value_b in the slot.
static void slot_set_value(struct kv_slot *s, const void *val)
{
    memcpy(s->value_a, val,                              KV_VALUE_A);
    memcpy(s->value_b, (const uint8_t *)val + KV_VALUE_A, KV_VALUE_B);
}

// ── FNV-1a (bucket index only) ────────────────────────────────────────────────

#define FNV_OFFSET 2166136261U
#define FNV_PRIME  16777619U

static uint32_t fnv1a(const void *data, size_t len, uint32_t seed)
{
    uint32_t h = seed;
    const uint8_t *p = data;
    for (size_t i = 0; i < len; i++) { h ^= p[i]; h *= FNV_PRIME; }
    return h;
}

static uint32_t hash1(uint64_t key) { return fnv1a(&key, 8, FNV_OFFSET)               % KV_NUM_BUCKETS; }
static uint32_t hash2(uint64_t key) { return fnv1a(&key, 8, FNV_OFFSET ^ 0xdeadbeefU) % KV_NUM_BUCKETS; }

// ── server-side cuckoo table ──────────────────────────────────────────────────

static inline struct kv_slot *get_slot(struct kv_slot *table, int which, uint32_t idx)
{
    return &table[which * KV_NUM_BUCKETS + idx];
}

// Write protocol:
//   1. zero checksum        → readers see "in-progress"
//   2. fence
//   3. write key + value
//   4. fence
//   5. write checksum       → readers see "complete"
static void write_slot(struct kv_slot *dst, uint64_t key, const void *val)
{
    dst->checksum = 0;
    __sync_synchronize();
    dst->key = key;
    slot_set_value(dst, val);
    __sync_synchronize();
    dst->checksum = slot_checksum(dst);
}

#define MAX_EVICT 16

// Alternating eviction: even depth kicks from t1 (s1), odd depth kicks from t2 (s2).
static int cuckoo_insert(struct kv_slot *table, uint64_t key, const void *val, int depth)
{
    if (depth > MAX_EVICT)
        return -1;

    uint32_t i1 = hash1(key);
    uint32_t i2 = hash2(key);
    struct kv_slot *s1 = get_slot(table, 0, i1);
    struct kv_slot *s2 = get_slot(table, 1, i2);

    if (s1->checksum && s1->key == key) { write_slot(s1, key, val); return 0; }
    if (s2->checksum && s2->key == key) { write_slot(s2, key, val); return 0; }

    if (!s1->checksum) { write_slot(s1, key, val); return 0; }
    if (!s2->checksum) { write_slot(s2, key, val); return 0; }

    struct kv_slot *evict_slot = (depth % 2 == 0) ? s1 : s2;
    uint64_t evict_key = evict_slot->key;
    uint8_t  evict_val[KV_VALUE_SIZE];
    slot_get_value(evict_slot, evict_val);
    write_slot(evict_slot, key, val);
    return cuckoo_insert(table, evict_key, evict_val, depth + 1);
}

static void pilaf_server_init(struct kv_slot *table)
{
    memset(table, 0, KV_TABLE_SIZE);
}

static int pilaf_server_put(struct kv_slot *table, uint64_t key, const void *val)
{
    if (cuckoo_insert(table, key, val, 0)) {
        fprintf(stderr, "pilaf: table full (eviction depth > %d)\n", MAX_EVICT);
        return -1;
    }
    return 0;
}

// ── client-side GET: two parallel RDMA READs ─────────────────────────────────

static int pilaf_get(struct connection *conn, struct qp_info *remote,
                     uint64_t key, void *val_out)
{
    struct kv_slot *local_a = (struct kv_slot *)(conn->buf + KV_CLIENT_READ_A_OFF);
    struct kv_slot *local_b = (struct kv_slot *)(conn->buf + KV_CLIENT_READ_B_OFF);

    uint32_t i1 = hash1(key);
    uint32_t i2 = hash2(key);
    uint64_t addr1 = remote->addr + (uint64_t)i1 * sizeof(struct kv_slot);
    uint64_t addr2 = remote->addr + (uint64_t)(KV_NUM_BUCKETS + i2) * sizeof(struct kv_slot);

    for (int retry = 0; retry < 16; retry++) {
        if (rdma_post_read(conn, remote->rkey, addr1, local_a, sizeof(*local_a))) return -1;
        if (rdma_post_read(conn, remote->rkey, addr2, local_b, sizeof(*local_b))) return -1;
        if (rdma_poll_cq(conn->cq)) return -1;
        if (rdma_poll_cq(conn->cq)) return -1;

        if (local_a->checksum) {
            if (!slot_valid(local_a)) continue;
            if (local_a->key == key) { slot_get_value(local_a, val_out); return 0; }
        }
        if (local_b->checksum) {
            if (!slot_valid(local_b)) continue;
            if (local_b->key == key) { slot_get_value(local_b, val_out); return 0; }
        }

        return 1;
    }

    return -1;
}

// ── client-side PUT: two-sided SEND/RECV ─────────────────────────────────────

static int pilaf_put(struct connection *conn, struct qp_info *remote,
                     uint64_t key, const void *val)
{
    struct kv_msg *req  = (struct kv_msg *)(conn->buf + KV_CLIENT_SEND_OFF);
    struct kv_msg *resp = (struct kv_msg *)(conn->buf + KV_CLIENT_RECV_OFF);

    req->type = KV_MSG_PUT;
    req->key  = key;
    memcpy(req->value, val, KV_VALUE_SIZE);

    if (rdma_post_recv(conn, resp, sizeof(*resp))) return -1;
    if (rdma_post_send(conn, req,  sizeof(*req)))  return -1;
    if (rdma_poll_cq(conn->cq)) return -1;
    if (rdma_poll_cq(conn->cq)) return -1;

    return (resp->type == KV_MSG_OK) ? 0 : -1;
}

// ── exported ops table ────────────────────────────────────────────────────────

const struct kv_ops kv_pilaf_ops = {
    .name        = "pilaf",
    .table_size  = KV_TABLE_SIZE,
    .server_init = pilaf_server_init,
    .server_put  = pilaf_server_put,
    .get         = pilaf_get,
    .put         = pilaf_put,
};
