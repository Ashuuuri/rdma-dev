#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <infiniband/verbs.h>

#define MSG       "Hello RDMA!"
#define MSG_SIZE  64
#define PORT      12345
#define IB_PORT   1
#define GID_INDEX 1

struct connection {
    struct ibv_context      *ctx;
    struct ibv_pd           *pd;
    struct ibv_mr           *mr;
    struct ibv_cq           *cq;
    struct ibv_comp_channel *channel;
    struct ibv_qp           *qp;
    char                    *buf;
};

struct qp_info {
    uint32_t      qpn;
    union ibv_gid gid;
    uint32_t      rkey;
    uint64_t      addr;
};

static int send_all(int fd, const void *buf, size_t len)
{
    while (len > 0) {
        ssize_t n = send(fd, buf, len, 0);
        if (n <= 0) return -1;
        buf = (const char *)buf + n;
        len -= n;
    }
    return 0;
}

static int recv_all(int fd, void *buf, size_t len)
{
    while (len > 0) {
        ssize_t n = recv(fd, buf, len, 0);
        if (n <= 0) return -1;
        buf = (char *)buf + n;
        len -= n;
    }
    return 0;
}

static struct connection *init_connection(void)
{
    struct ibv_device **dev_list;
    struct ibv_qp_init_attr qp_attr;
    struct connection *conn;

    conn = calloc(1, sizeof(*conn));
    if (!conn) {
        fprintf(stderr, "failed to allocate connection\n");
        return NULL;
    }

    dev_list = ibv_get_device_list(NULL);
    if (!dev_list) {
        fprintf(stderr, "failed to get device list\n");
        goto err_conn;
    }
    if (!dev_list[0]) {
        fprintf(stderr, "no RDMA device found\n");
        ibv_free_device_list(dev_list);
        goto err_conn;
    }

    conn->ctx = ibv_open_device(dev_list[0]);
    ibv_free_device_list(dev_list);
    if (!conn->ctx) {
        fprintf(stderr, "failed to open device\n");
        goto err_conn;
    }

    conn->pd = ibv_alloc_pd(conn->ctx);
    if (!conn->pd) {
        fprintf(stderr, "failed to allocate pd\n");
        goto err_ctx;
    }

    conn->buf = calloc(1, MSG_SIZE);
    if (!conn->buf) {
        fprintf(stderr, "failed to allocate buffer\n");
        goto err_pd;
    }

    conn->mr = ibv_reg_mr(conn->pd, conn->buf, MSG_SIZE,
                          IBV_ACCESS_LOCAL_WRITE  |
                          IBV_ACCESS_REMOTE_WRITE |
                          IBV_ACCESS_REMOTE_READ);
    if (!conn->mr) {
        fprintf(stderr, "failed to register mr\n");
        goto err_buf;
    }

    conn->channel = ibv_create_comp_channel(conn->ctx);
    if (!conn->channel) {
        fprintf(stderr, "failed to create comp channel\n");
        goto err_mr;
    }

    conn->cq = ibv_create_cq(conn->ctx, 16, NULL, conn->channel, 0);
    if (!conn->cq) {
        fprintf(stderr, "failed to create cq\n");
        goto err_channel;
    }

    memset(&qp_attr, 0, sizeof(qp_attr));
    qp_attr.send_cq          = conn->cq;
    qp_attr.recv_cq          = conn->cq;
    qp_attr.cap.max_send_wr  = 16;
    qp_attr.cap.max_recv_wr  = 16;
    qp_attr.cap.max_send_sge = 1;
    qp_attr.cap.max_recv_sge = 1;
    qp_attr.qp_type          = IBV_QPT_RC;

    conn->qp = ibv_create_qp(conn->pd, &qp_attr);
    if (!conn->qp) {
        fprintf(stderr, "failed to create qp\n");
        goto err_cq;
    }

    return conn;

err_cq:      ibv_destroy_cq(conn->cq);
err_channel: ibv_destroy_comp_channel(conn->channel);
err_mr:      ibv_dereg_mr(conn->mr);
err_buf:     free(conn->buf);
err_pd:      ibv_dealloc_pd(conn->pd);
err_ctx:     ibv_close_device(conn->ctx);
err_conn:    free(conn);
    return NULL;
}

static int modify_qp_to_rts(struct ibv_qp *qp, uint32_t remote_qpn,
                              union ibv_gid remote_gid)
{
    struct ibv_qp_attr attr;
    int flags;

    memset(&attr, 0, sizeof(attr));
    attr.qp_state        = IBV_QPS_INIT;
    attr.pkey_index      = 0;
    attr.port_num        = IB_PORT;
    attr.qp_access_flags = IBV_ACCESS_LOCAL_WRITE  |
                           IBV_ACCESS_REMOTE_WRITE |
                           IBV_ACCESS_REMOTE_READ;
    flags = IBV_QP_STATE | IBV_QP_PKEY_INDEX |
            IBV_QP_PORT  | IBV_QP_ACCESS_FLAGS;
    if (ibv_modify_qp(qp, &attr, flags)) {
        fprintf(stderr, "failed to modify QP to INIT\n");
        return -1;
    }

    memset(&attr, 0, sizeof(attr));
    attr.qp_state               = IBV_QPS_RTR;
    attr.path_mtu               = IBV_MTU_1024;
    attr.dest_qp_num            = remote_qpn;
    attr.rq_psn                 = 0;
    attr.max_dest_rd_atomic     = 1;
    attr.min_rnr_timer          = 12;
    attr.ah_attr.is_global      = 1;
    attr.ah_attr.grh.dgid       = remote_gid;
    attr.ah_attr.grh.hop_limit  = 1;
    attr.ah_attr.grh.sgid_index = GID_INDEX;
    attr.ah_attr.port_num       = IB_PORT;
    flags = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
            IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
            IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;
    if (ibv_modify_qp(qp, &attr, flags)) {
        fprintf(stderr, "failed to modify QP to RTR\n");
        return -1;
    }

    memset(&attr, 0, sizeof(attr));
    attr.qp_state      = IBV_QPS_RTS;
    attr.timeout       = 14;
    attr.retry_cnt     = 7;
    attr.rnr_retry     = 7;
    attr.sq_psn        = 0;
    attr.max_rd_atomic = 1;
    flags = IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
            IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC;
    if (ibv_modify_qp(qp, &attr, flags)) {
        fprintf(stderr, "failed to modify QP to RTS\n");
        return -1;
    }

    return 0;
}

static void exchange_info_server(struct qp_info *local, struct qp_info *remote)
{
    int sockfd, connfd;
    struct sockaddr_in addr;
    int opt = 1;

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(PORT);

    bind(sockfd, (struct sockaddr *)&addr, sizeof(addr));
    listen(sockfd, 1);
    connfd = accept(sockfd, NULL, NULL);

    send_all(connfd, local, sizeof(*local));
    recv_all(connfd, remote, sizeof(*remote));

    close(connfd);
    close(sockfd);
}

static void exchange_info_client(struct qp_info *local, struct qp_info *remote,
                                  const char *server_ip)
{
    int sockfd;
    struct sockaddr_in addr;

    sockfd = socket(AF_INET, SOCK_STREAM, 0);

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(PORT);
    inet_pton(AF_INET, server_ip, &addr.sin_addr);

    connect(sockfd, (struct sockaddr *)&addr, sizeof(addr));

    recv_all(sockfd, remote, sizeof(*remote));
    send_all(sockfd, local, sizeof(*local));

    close(sockfd);
}

static int poll_cq(struct ibv_cq *cq)
{
    struct ibv_wc wc;
    int ret;
    while ((ret = ibv_poll_cq(cq, 1, &wc)) == 0);
    if (ret < 0) {
        fprintf(stderr, "poll_cq error: %d\n", ret);
        return -1;
    }
    if (wc.status != IBV_WC_SUCCESS) {
        fprintf(stderr, "wc error: %s\n", ibv_wc_status_str(wc.status));
        return -1;
    }
    return 0;
}

static int wait_cq_event(struct connection *conn)
{
    struct ibv_cq *ev_cq;
    void *ev_ctx;
    if (ibv_get_cq_event(conn->channel, &ev_cq, &ev_ctx)) {
        fprintf(stderr, "get_cq_event failed\n");
        return -1;
    }
    ibv_ack_cq_events(ev_cq, 1);
    return poll_cq(conn->cq);
}

static int op_send_recv(struct connection *conn, struct qp_info *remote, int is_server)
{
    struct ibv_sge sge = {
        .addr   = (uint64_t)conn->buf,
        .length = MSG_SIZE,
        .lkey   = conn->mr->lkey,
    };

    if (is_server) {
        struct ibv_recv_wr rwr = { .wr_id = 0, .sg_list = &sge, .num_sge = 1 };
        struct ibv_recv_wr *bad;
        if (ibv_post_recv(conn->qp, &rwr, &bad)) {
            fprintf(stderr, "post_recv failed\n");
            return -1;
        }
        if (poll_cq(conn->cq)) return -1;
        printf("[recv] %s\n", conn->buf);
    } else {
        strncpy(conn->buf, MSG, MSG_SIZE);
        struct ibv_send_wr swr = {
            .wr_id      = 0,
            .sg_list    = &sge,
            .num_sge    = 1,
            .opcode     = IBV_WR_SEND,
            .send_flags = IBV_SEND_SIGNALED,
        };
        struct ibv_send_wr *bad;
        if (ibv_post_send(conn->qp, &swr, &bad)) {
            fprintf(stderr, "post_send failed\n");
            return -1;
        }
        if (poll_cq(conn->cq)) return -1;
        printf("[send] %s\n", conn->buf);
    }
    return 0;
}

static int op_write(struct connection *conn, struct qp_info *remote, int is_server)
{
    struct ibv_sge sge = {
        .addr   = (uint64_t)conn->buf,
        .length = MSG_SIZE,
        .lkey   = conn->mr->lkey,
    };

    if (is_server) {
        struct ibv_recv_wr rwr = { .wr_id = 0, .sg_list = &sge, .num_sge = 1 };
        struct ibv_recv_wr *bad;
        if (ibv_post_recv(conn->qp, &rwr, &bad)) {
            fprintf(stderr, "post_recv failed\n");
            return -1;
        }
        if (ibv_req_notify_cq(conn->cq, 0)) {
            fprintf(stderr, "req_notify_cq failed\n");
            return -1;
        }
        printf("server: waiting for RDMA WRITE...\n");
        if (wait_cq_event(conn)) return -1;
        printf("[write target] received: %s\n", conn->buf);
    } else {
        strncpy(conn->buf, MSG, MSG_SIZE);
        struct ibv_send_wr swr = {
            .wr_id               = 0,
            .sg_list             = &sge,
            .num_sge             = 1,
            .opcode              = IBV_WR_RDMA_WRITE_WITH_IMM,
            .imm_data            = htonl(1),
            .send_flags          = IBV_SEND_SIGNALED,
            .wr.rdma.remote_addr = remote->addr,
            .wr.rdma.rkey        = remote->rkey,
        };
        struct ibv_send_wr *bad;
        if (ibv_post_send(conn->qp, &swr, &bad)) {
            fprintf(stderr, "post_send failed\n");
            return -1;
        }
        if (poll_cq(conn->cq)) return -1;
        printf("[write initiator] wrote: %s\n", conn->buf);
    }
    return 0;
}

static int op_read(struct connection *conn, struct qp_info *remote, int is_server)
{
    if (is_server) {
        strncpy(conn->buf, MSG, MSG_SIZE);
        printf("server: data ready, waiting for remote read...\n");
        // RDMA READ is one-sided — server CPU not involved in the transfer.
        // sleep keeps the buffer valid until client finishes.
        sleep(2);
        printf("server: done\n");
    } else {
        struct ibv_sge sge = {
            .addr   = (uint64_t)conn->buf,
            .length = MSG_SIZE,
            .lkey   = conn->mr->lkey,
        };
        struct ibv_send_wr swr = {
            .wr_id               = 0,
            .sg_list             = &sge,
            .num_sge             = 1,
            .opcode              = IBV_WR_RDMA_READ,
            .send_flags          = IBV_SEND_SIGNALED,
            .wr.rdma.remote_addr = remote->addr,
            .wr.rdma.rkey        = remote->rkey,
        };
        struct ibv_send_wr *bad;
        if (ibv_post_send(conn->qp, &swr, &bad)) {
            fprintf(stderr, "post_send failed\n");
            return -1;
        }
        if (poll_cq(conn->cq)) return -1;
        printf("[read] %s\n", conn->buf);
    }
    return 0;
}

int main(int argc, char *argv[])
{
    const char *op, *role;
    int is_server;
    struct connection *conn;
    struct qp_info local_info, remote_info;
    union ibv_gid gid;

    if (argc < 3) {
        fprintf(stderr, "usage: %s <send-recv|write|read> <server|client> [server_ip]\n",
                argv[0]);
        return 1;
    }

    op   = argv[1];
    role = argv[2];

    if (strcmp(op, "send-recv") != 0 &&
        strcmp(op, "write")     != 0 &&
        strcmp(op, "read")      != 0) {
        fprintf(stderr, "unknown op: %s\n", op);
        return 1;
    }

    if (strcmp(role, "server") == 0) {
        is_server = 1;
    } else if (strcmp(role, "client") == 0) {
        if (argc < 4) {
            fprintf(stderr, "client requires server_ip\n");
            return 1;
        }
        is_server = 0;
    } else {
        fprintf(stderr, "unknown role: %s\n", role);
        return 1;
    }

    conn = init_connection();
    if (!conn) return 1;

    if (ibv_query_gid(conn->ctx, IB_PORT, GID_INDEX, &gid)) {
        fprintf(stderr, "failed to query gid\n");
        return 1;
    }

    local_info.qpn  = conn->qp->qp_num;
    local_info.gid  = gid;
    local_info.rkey = conn->mr->rkey;
    local_info.addr = (uint64_t)conn->buf;

    if (is_server)
        exchange_info_server(&local_info, &remote_info);
    else
        exchange_info_client(&local_info, &remote_info, argv[3]);

    if (modify_qp_to_rts(conn->qp, remote_info.qpn, remote_info.gid)) {
        fprintf(stderr, "failed to bring QP to RTS\n");
        return 1;
    }

    if (strcmp(op, "send-recv") == 0) return op_send_recv(conn, &remote_info, is_server);
    if (strcmp(op, "write")     == 0) return op_write    (conn, &remote_info, is_server);
    if (strcmp(op, "read")      == 0) return op_read     (conn, &remote_info, is_server);

    return 0;
}
