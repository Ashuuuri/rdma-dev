#pragma once

#include <stdint.h>
#include <stddef.h>
#include <infiniband/verbs.h>

#define IB_PORT     1
#define GID_INDEX   1
#define MAX_WR      4096
#define INLINE_HINT 256

struct connection {
    struct ibv_context      *ctx;
    struct ibv_pd           *pd;
    struct ibv_mr           *mr;
    struct ibv_cq           *cq;
    struct ibv_comp_channel *channel;
    struct ibv_qp           *qp;
    char                    *buf;
    uint8_t                  link_layer;
    uint16_t                 lid;
    uint8_t                  active_mtu;
    uint32_t                 max_inline;
};

struct qp_info {
    uint32_t      qpn;
    uint16_t      lid;
    union ibv_gid gid;
    uint32_t      rkey;
    uint64_t      addr;
};

// Connection setup
struct connection *rdma_init(size_t buf_size);
void rdma_fill_local_info(struct connection *conn, struct qp_info *info);
int  rdma_exchange_server(struct qp_info *local, struct qp_info *remote, int port);
int  rdma_exchange_client(struct qp_info *local, struct qp_info *remote,
                          const char *ip, int port);
int  rdma_modify_qp(struct connection *conn, struct qp_info *remote);

// Completion
int rdma_poll_cq(struct ibv_cq *cq);
int rdma_wait_event(struct connection *conn);

// One-sided primitives — local buf must lie within conn->mr
int rdma_post_read(struct connection *conn, uint32_t rkey, uint64_t raddr,
                   void *local, size_t len);   // post only, no poll
int rdma_read (struct connection *conn, uint32_t rkey, uint64_t raddr,
               void *local, size_t len);       // post + poll
int rdma_write(struct connection *conn, uint32_t rkey, uint64_t raddr,
               const void *local, size_t len);
int rdma_cas  (struct connection *conn, uint32_t rkey, uint64_t raddr,
               uint64_t cmp, uint64_t swap, uint64_t *orig);

// Two-sided primitives — buf must lie within conn->mr
int rdma_post_recv(struct connection *conn, void *buf, size_t len);
int rdma_post_send(struct connection *conn, const void *buf, size_t len);

// Barrier: exchange a single byte over the TCP sync fd so both sides know
// the QP is in RTS before any RDMA operation is posted.
int  rdma_barrier(int syncfd);

// Utilities
int  send_all(int fd, const void *buf, size_t len);
int  recv_all(int fd, void *buf, size_t len);
long now_ns(void);
