#pragma once

#include <stdint.h>
#include <stddef.h>
#include <infiniband/verbs.h>

#define IB_PORT     1
#define GID_INDEX   1
#define MAX_WR      4096
#define INLINE_HINT 256
#define UD_GRH_SIZE 40   // GRH prepended to every UD recv payload

struct connection {
    struct ibv_context      *ctx;
    struct ibv_pd           *pd;
    struct ibv_mr           *mr;
    struct ibv_cq           *cq;
    struct ibv_comp_channel *channel;
    struct ibv_qp           *qp;      // RC QP
    char                    *buf;
    uint8_t                  link_layer;
    uint16_t                 lid;
    uint8_t                  active_mtu;
    uint32_t                 max_inline;
    // UC + UD (HERD transport)
    struct ibv_qp           *uc_qp;
    struct ibv_qp           *ud_qp;
    struct ibv_cq           *ud_cq;
    struct ibv_mr           *ud_mr;
    char                    *ud_buf;       // recv slots: nslots × (GRH + payload)
    size_t                   ud_slot_size; // GRH + payload per slot
    struct ibv_ah           *peer_ah;
};

struct qp_info {
    uint32_t      qpn;
    uint16_t      lid;
    union ibv_gid gid;
    uint32_t      rkey;
    uint64_t      addr;
    uint32_t      uc_qpn;     // 0 if not used
    uint32_t      ud_qpn;     // 0 if not used
    int           client_id;  // assigned by server per connection
};

// Connection setup
struct connection *rdma_init(size_t buf_size);
void rdma_fill_local_info(struct connection *conn, struct qp_info *info);
// Multi-client server helpers: listen once, accept N times
int  rdma_listen(int port);
int  rdma_accept(int lsock, struct qp_info *local, struct qp_info *remote);
// Single-client convenience wrapper (listen + accept + close listen)
int  rdma_exchange_server(struct qp_info *local, struct qp_info *remote, int port);
int  rdma_exchange_client(struct qp_info *local, struct qp_info *remote,
                          const char *ip, int port);
int  rdma_modify_qp(struct connection *conn, struct qp_info *remote);
int  rdma_barrier(int syncfd);

// UC QP — single-client wrappers (store in conn->uc_qp / conn->peer_ah)
int rdma_init_uc_qp(struct connection *conn);
int rdma_connect_uc_qp(struct connection *conn, struct qp_info *remote);
int rdma_uc_write(struct connection *conn, uint32_t rkey, uint64_t raddr,
                  const void *local, size_t len);

// UC QP — raw variants for multi-client (caller manages the QP/AH)
struct ibv_qp *rdma_alloc_uc_qp(struct connection *conn);
int  rdma_connect_uc_qp_to(struct connection *conn, struct ibv_qp *uc_qp,
                            struct qp_info *remote);
int  rdma_uc_write_via(struct connection *conn, struct ibv_qp *uc_qp,
                       uint32_t rkey, uint64_t raddr, const void *local, size_t len);

// UD QP — single-client wrappers
int rdma_init_ud_qp(struct connection *conn, size_t payload_size, int nslots);
int rdma_create_ah(struct connection *conn, struct qp_info *remote);
int rdma_ud_send(struct connection *conn, uint32_t remote_qpn,
                 const void *buf, size_t len);
int rdma_ud_recv(struct connection *conn, void *out);

// UD QP — raw variants
struct ibv_ah *rdma_alloc_ah(struct connection *conn, struct qp_info *remote);
// Post one UD SEND without polling; set signaled=1 only for the last WR in a batch.
int  rdma_ud_post_send(struct connection *conn, struct ibv_ah *ah, uint32_t remote_qpn,
                       const void *buf, size_t len, int signaled);
// Post + poll (single send, backward compat).
int  rdma_ud_send_via(struct connection *conn, struct ibv_ah *ah, uint32_t remote_qpn,
                      const void *buf, size_t len);

// Completion
int rdma_poll_cq(struct ibv_cq *cq);
int rdma_wait_event(struct connection *conn);

// One-sided RC primitives
int rdma_post_read(struct connection *conn, uint32_t rkey, uint64_t raddr,
                   void *local, size_t len);
int rdma_read (struct connection *conn, uint32_t rkey, uint64_t raddr,
               void *local, size_t len);
int rdma_write(struct connection *conn, uint32_t rkey, uint64_t raddr,
               const void *local, size_t len);
int rdma_cas  (struct connection *conn, uint32_t rkey, uint64_t raddr,
               uint64_t cmp, uint64_t swap, uint64_t *orig);

// Two-sided RC primitives
int rdma_post_recv(struct connection *conn, void *buf, size_t len);
int rdma_post_send(struct connection *conn, const void *buf, size_t len);

// Utilities
int  send_all(int fd, const void *buf, size_t len);
int  recv_all(int fd, void *buf, size_t len);
long now_ns(void);
