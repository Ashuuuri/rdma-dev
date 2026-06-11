#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <arpa/inet.h>
#include "rdma.h"

// ── utilities ─────────────────────────────────────────────────────────────────

int send_all(int fd, const void *buf, size_t len)
{
    while (len > 0) {
        ssize_t n = send(fd, buf, len, 0);
        if (n <= 0) return -1;
        buf = (const char *)buf + n;
        len -= n;
    }
    return 0;
}

int recv_all(int fd, void *buf, size_t len)
{
    while (len > 0) {
        ssize_t n = recv(fd, buf, len, 0);
        if (n <= 0) return -1;
        buf = (char *)buf + n;
        len -= n;
    }
    return 0;
}

long now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000000L + ts.tv_nsec;
}

// ── setup ─────────────────────────────────────────────────────────────────────

struct connection *rdma_init(size_t buf_size)
{
    struct ibv_device      **dev_list;
    struct ibv_qp_init_attr  qp_attr;
    struct ibv_port_attr     port_attr;
    struct connection       *conn;

    conn = calloc(1, sizeof(*conn));
    if (!conn) {
        fprintf(stderr, "rdma_init: failed to allocate connection\n");
        return NULL;
    }

    dev_list = ibv_get_device_list(NULL);
    if (!dev_list) {
        fprintf(stderr, "rdma_init: failed to get device list\n");
        goto err_conn;
    }
    if (!dev_list[0]) {
        fprintf(stderr, "rdma_init: no RDMA device found\n");
        ibv_free_device_list(dev_list);
        goto err_conn;
    }

    conn->ctx = ibv_open_device(dev_list[0]);
    ibv_free_device_list(dev_list);
    if (!conn->ctx) {
        fprintf(stderr, "rdma_init: failed to open device\n");
        goto err_conn;
    }

    conn->pd = ibv_alloc_pd(conn->ctx);
    if (!conn->pd) {
        fprintf(stderr, "rdma_init: failed to allocate pd\n");
        goto err_ctx;
    }

    conn->buf = calloc(1, buf_size);
    if (!conn->buf) {
        fprintf(stderr, "rdma_init: failed to allocate buffer\n");
        goto err_pd;
    }

    conn->mr = ibv_reg_mr(conn->pd, conn->buf, buf_size,
                          IBV_ACCESS_LOCAL_WRITE   |
                          IBV_ACCESS_REMOTE_WRITE  |
                          IBV_ACCESS_REMOTE_READ   |
                          IBV_ACCESS_REMOTE_ATOMIC);
    if (!conn->mr) {
        fprintf(stderr, "rdma_init: failed to register mr\n");
        goto err_buf;
    }

    conn->channel = ibv_create_comp_channel(conn->ctx);
    if (!conn->channel) {
        fprintf(stderr, "rdma_init: failed to create comp channel\n");
        goto err_mr;
    }

    conn->cq = ibv_create_cq(conn->ctx, MAX_WR, NULL, conn->channel, 0);
    if (!conn->cq) {
        fprintf(stderr, "rdma_init: failed to create cq\n");
        goto err_channel;
    }

    memset(&qp_attr, 0, sizeof(qp_attr));
    qp_attr.send_cq             = conn->cq;
    qp_attr.recv_cq             = conn->cq;
    qp_attr.cap.max_send_wr     = MAX_WR;
    qp_attr.cap.max_recv_wr     = MAX_WR;
    qp_attr.cap.max_send_sge    = 1;
    qp_attr.cap.max_recv_sge    = 1;
    qp_attr.cap.max_inline_data = INLINE_HINT;
    qp_attr.qp_type             = IBV_QPT_RC;

    conn->qp = ibv_create_qp(conn->pd, &qp_attr);
    if (!conn->qp) {
        fprintf(stderr, "rdma_init: failed to create qp\n");
        goto err_cq;
    }
    conn->max_inline = qp_attr.cap.max_inline_data;

    if (ibv_query_port(conn->ctx, IB_PORT, &port_attr)) {
        fprintf(stderr, "rdma_init: failed to query port\n");
        goto err_qp;
    }
    conn->lid        = port_attr.lid;
    conn->link_layer = port_attr.link_layer;
    conn->active_mtu = port_attr.active_mtu;

    return conn;

err_qp:      ibv_destroy_qp(conn->qp);
err_cq:      ibv_destroy_cq(conn->cq);
err_channel: ibv_destroy_comp_channel(conn->channel);
err_mr:      ibv_dereg_mr(conn->mr);
err_buf:     free(conn->buf);
err_pd:      ibv_dealloc_pd(conn->pd);
err_ctx:     ibv_close_device(conn->ctx);
err_conn:    free(conn);
    return NULL;
}

void rdma_fill_local_info(struct connection *conn, struct qp_info *info)
{
    union ibv_gid gid;
    memset(&gid, 0, sizeof(gid));
    if (conn->link_layer != IBV_LINK_LAYER_INFINIBAND)
        ibv_query_gid(conn->ctx, IB_PORT, GID_INDEX, &gid);

    info->qpn  = conn->qp->qp_num;
    info->lid  = conn->lid;
    info->gid  = gid;
    info->rkey = conn->mr->rkey;
    info->addr = (uint64_t)conn->buf;
}

// returns connected TCP fd used for later synchronization
int rdma_exchange_server(struct qp_info *local, struct qp_info *remote, int port)
{
    int sockfd, connfd;
    struct sockaddr_in addr;
    int opt = 1;

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons((uint16_t)port);

    bind(sockfd, (struct sockaddr *)&addr, sizeof(addr));
    listen(sockfd, 1);
    connfd = accept(sockfd, NULL, NULL);
    close(sockfd);

    send_all(connfd, local,  sizeof(*local));
    recv_all(connfd, remote, sizeof(*remote));

    return connfd;
}

int rdma_exchange_client(struct qp_info *local, struct qp_info *remote,
                         const char *ip, int port)
{
    int sockfd;
    struct sockaddr_in addr;

    sockfd = socket(AF_INET, SOCK_STREAM, 0);

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)port);
    inet_pton(AF_INET, ip, &addr.sin_addr);

    connect(sockfd, (struct sockaddr *)&addr, sizeof(addr));

    recv_all(sockfd, remote, sizeof(*remote));
    send_all(sockfd, local,  sizeof(*local));

    return sockfd;
}

int rdma_modify_qp(struct connection *conn, struct qp_info *remote)
{
    struct ibv_qp_attr attr;
    int flags;

    memset(&attr, 0, sizeof(attr));
    attr.qp_state        = IBV_QPS_INIT;
    attr.pkey_index      = 0;
    attr.port_num        = IB_PORT;
    attr.qp_access_flags = IBV_ACCESS_LOCAL_WRITE   |
                           IBV_ACCESS_REMOTE_WRITE  |
                           IBV_ACCESS_REMOTE_READ   |
                           IBV_ACCESS_REMOTE_ATOMIC;
    flags = IBV_QP_STATE | IBV_QP_PKEY_INDEX |
            IBV_QP_PORT  | IBV_QP_ACCESS_FLAGS;
    if (ibv_modify_qp(conn->qp, &attr, flags)) {
        fprintf(stderr, "rdma_modify_qp: failed to move to INIT\n");
        return -1;
    }

    memset(&attr, 0, sizeof(attr));
    attr.qp_state           = IBV_QPS_RTR;
    attr.path_mtu           = conn->active_mtu;
    attr.dest_qp_num        = remote->qpn;
    attr.rq_psn             = 0;
    attr.max_dest_rd_atomic = 16;
    attr.min_rnr_timer      = 12;
    attr.ah_attr.port_num   = IB_PORT;

    if (conn->link_layer == IBV_LINK_LAYER_INFINIBAND) {
        attr.ah_attr.is_global = 0;
        attr.ah_attr.dlid      = remote->lid;
    } else {
        attr.ah_attr.is_global      = 1;
        attr.ah_attr.grh.dgid       = remote->gid;
        attr.ah_attr.grh.sgid_index = GID_INDEX;
        attr.ah_attr.grh.hop_limit  = 1;
    }

    flags = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
            IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
            IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;
    if (ibv_modify_qp(conn->qp, &attr, flags)) {
        fprintf(stderr, "rdma_modify_qp: failed to move to RTR\n");
        return -1;
    }

    memset(&attr, 0, sizeof(attr));
    attr.qp_state      = IBV_QPS_RTS;
    attr.timeout       = 14;
    attr.retry_cnt     = 7;
    attr.rnr_retry     = 7;
    attr.sq_psn        = 0;
    attr.max_rd_atomic = 16;
    flags = IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
            IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC;
    if (ibv_modify_qp(conn->qp, &attr, flags)) {
        fprintf(stderr, "rdma_modify_qp: failed to move to RTS\n");
        return -1;
    }

    return 0;
}

int rdma_barrier(int syncfd)
{
    char b = 0;
    if (send(syncfd, &b, 1, 0) != 1) return -1;
    if (recv(syncfd, &b, 1, 0) != 1) return -1;
    return 0;
}

// ── completion ────────────────────────────────────────────────────────────────

int rdma_poll_cq(struct ibv_cq *cq)
{
    struct ibv_wc wc;
    int ret;
    while ((ret = ibv_poll_cq(cq, 1, &wc)) == 0);
    if (ret < 0) {
        fprintf(stderr, "rdma_poll_cq: error %d\n", ret);
        return -1;
    }
    if (wc.status != IBV_WC_SUCCESS) {
        fprintf(stderr, "rdma_poll_cq: wc error: %s\n", ibv_wc_status_str(wc.status));
        return -1;
    }
    return 0;
}

int rdma_wait_event(struct connection *conn)
{
    struct ibv_cq *ev_cq;
    void *ev_ctx;
    if (ibv_get_cq_event(conn->channel, &ev_cq, &ev_ctx)) {
        fprintf(stderr, "rdma_wait_event: get_cq_event failed\n");
        return -1;
    }
    ibv_ack_cq_events(ev_cq, 1);
    return rdma_poll_cq(conn->cq);
}

// ── one-sided primitives ──────────────────────────────────────────────────────

int rdma_post_read(struct connection *conn, uint32_t rkey, uint64_t raddr,
                   void *local, size_t len)
{
    struct ibv_sge sge = {
        .addr   = (uint64_t)local,
        .length = (uint32_t)len,
        .lkey   = conn->mr->lkey,
    };
    struct ibv_send_wr swr = {
        .wr_id               = 0,
        .sg_list             = &sge,
        .num_sge             = 1,
        .opcode              = IBV_WR_RDMA_READ,
        .send_flags          = IBV_SEND_SIGNALED,
        .wr.rdma.remote_addr = raddr,
        .wr.rdma.rkey        = rkey,
    };
    struct ibv_send_wr *bad;
    return ibv_post_send(conn->qp, &swr, &bad) ? -1 : 0;
}

int rdma_read(struct connection *conn, uint32_t rkey, uint64_t raddr,
              void *local, size_t len)
{
    if (rdma_post_read(conn, rkey, raddr, local, len)) return -1;
    return rdma_poll_cq(conn->cq);
}

int rdma_write(struct connection *conn, uint32_t rkey, uint64_t raddr,
               const void *local, size_t len)
{
    struct ibv_sge sge = {
        .addr   = (uint64_t)local,
        .length = (uint32_t)len,
        .lkey   = conn->mr->lkey,
    };
    struct ibv_send_wr swr = {
        .wr_id               = 0,
        .sg_list             = &sge,
        .num_sge             = 1,
        .opcode              = IBV_WR_RDMA_WRITE,
        .send_flags          = IBV_SEND_SIGNALED,
        .wr.rdma.remote_addr = raddr,
        .wr.rdma.rkey        = rkey,
    };
    struct ibv_send_wr *bad;
    if (ibv_post_send(conn->qp, &swr, &bad)) return -1;
    return rdma_poll_cq(conn->cq);
}

int rdma_cas(struct connection *conn, uint32_t rkey, uint64_t raddr,
             uint64_t cmp, uint64_t swap, uint64_t *orig)
{
    struct ibv_sge sge = {
        .addr   = (uint64_t)orig,
        .length = 8,
        .lkey   = conn->mr->lkey,
    };
    struct ibv_send_wr swr = {
        .wr_id      = 0,
        .sg_list    = &sge,
        .num_sge    = 1,
        .opcode     = IBV_WR_ATOMIC_CMP_AND_SWP,
        .send_flags = IBV_SEND_SIGNALED,
        .wr.atomic  = {
            .remote_addr = raddr,
            .rkey        = rkey,
            .compare_add = cmp,
            .swap        = swap,
        },
    };
    struct ibv_send_wr *bad;
    if (ibv_post_send(conn->qp, &swr, &bad)) return -1;
    return rdma_poll_cq(conn->cq);
}

// ── two-sided primitives ──────────────────────────────────────────────────────

int rdma_post_recv(struct connection *conn, void *buf, size_t len)
{
    struct ibv_sge sge = {
        .addr   = (uint64_t)buf,
        .length = (uint32_t)len,
        .lkey   = conn->mr->lkey,
    };
    struct ibv_recv_wr rwr = {
        .wr_id   = 0,
        .sg_list = &sge,
        .num_sge = 1,
    };
    struct ibv_recv_wr *bad;
    return ibv_post_recv(conn->qp, &rwr, &bad) ? -1 : 0;
}

int rdma_post_send(struct connection *conn, const void *buf, size_t len)
{
    struct ibv_sge sge = {
        .addr   = (uint64_t)buf,
        .length = (uint32_t)len,
        .lkey   = conn->mr->lkey,
    };
    int flags = IBV_SEND_SIGNALED;
    if (len <= conn->max_inline)
        flags |= IBV_SEND_INLINE;
    struct ibv_send_wr swr = {
        .wr_id      = 0,
        .sg_list    = &sge,
        .num_sge    = 1,
        .opcode     = IBV_WR_SEND,
        .send_flags = flags,
    };
    struct ibv_send_wr *bad;
    return ibv_post_send(conn->qp, &swr, &bad) ? -1 : 0;
}
