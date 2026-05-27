#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <infiniband/verbs.h>

#define MSG "Hello RDMA!"
#define MSG_SIZE 64
#define PORT 12345
#define IB_PORT 1
#define GID_INDEX 1

struct connection {
    struct ibv_context *ctx;      // device context
    struct ibv_pd      *pd;       // protection domain
    struct ibv_mr      *mr;       // memory region
    struct ibv_cq      *cq;       // completion queue
    struct ibv_qp      *qp;       // queue pair
    char               *buf;      // data buffer
};

struct connection *init_connection(void)
{
    struct connection *conn;
    struct ibv_device **dev_list;
    struct ibv_device *dev;
    struct ibv_qp_init_attr qp_init_attr;

    conn = calloc(1, sizeof(*conn));
    if (!conn) {
        fprintf(stderr, "failed to allocate connection\n");
        return NULL;
    }

    // get device list
    dev_list = ibv_get_device_list(NULL);
    if (!dev_list) {
        fprintf(stderr, "failed to get device list\n");
        return NULL;
    }

    // use first device
    dev = dev_list[0];
    if (!dev) {
        fprintf(stderr, "no RDMA device found\n");
        return NULL;
    }

    // open device
    conn->ctx = ibv_open_device(dev);
    if (!conn->ctx) {
        fprintf(stderr, "failed to open device\n");
        return NULL;
    }

    ibv_free_device_list(dev_list);

    // allocate protection domain
    conn->pd = ibv_alloc_pd(conn->ctx);
    if (!conn->pd) {
        fprintf(stderr, "failed to allocate pd\n");
        return NULL;
    }

    // allocate and register buffer
    conn->buf = calloc(1, MSG_SIZE);
    if (!conn->buf) {
        fprintf(stderr, "failed to allocate buffer\n");
        return NULL;
    }

    conn->mr = ibv_reg_mr(conn->pd, conn->buf, MSG_SIZE,
                          IBV_ACCESS_LOCAL_WRITE |
                          IBV_ACCESS_REMOTE_WRITE);
    if (!conn->mr) {
        fprintf(stderr, "failed to register mr\n");
        return NULL;
    }

    // create completion queue
    conn->cq = ibv_create_cq(conn->ctx, 16, NULL, NULL, 0);
    if (!conn->cq) {
        fprintf(stderr, "failed to create cq\n");
        return NULL;
    }

    // create queue pair
    memset(&qp_init_attr, 0, sizeof(qp_init_attr));
    qp_init_attr.send_cq = conn->cq;
    qp_init_attr.recv_cq = conn->cq;
    qp_init_attr.cap.max_send_wr  = 16;
    qp_init_attr.cap.max_recv_wr  = 16;
    qp_init_attr.cap.max_send_sge = 1;
    qp_init_attr.cap.max_recv_sge = 1;
    qp_init_attr.qp_type = IBV_QPT_RC;

    conn->qp = ibv_create_qp(conn->pd, &qp_init_attr);
    if (!conn->qp) {
        fprintf(stderr, "failed to create qp\n");
        return NULL;
    }

    return conn;
}

int modify_qp_to_rts(struct ibv_qp *qp, uint32_t remote_qpn,
                     uint16_t remote_lid, union ibv_gid remote_gid)
{
    struct ibv_qp_attr attr;
    int flags;

    // RESET -> INIT
    memset(&attr, 0, sizeof(attr));
    attr.qp_state        = IBV_QPS_INIT;
    attr.pkey_index      = 0;
    attr.port_num        = IB_PORT;
    attr.qp_access_flags = IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_LOCAL_WRITE;

    flags = IBV_QP_STATE | IBV_QP_PKEY_INDEX |
            IBV_QP_PORT  | IBV_QP_ACCESS_FLAGS;

    if (ibv_modify_qp(qp, &attr, flags)) {
        fprintf(stderr, "failed to modify QP to INIT\n");
        return -1;
    }

    // INIT -> RTR (Ready to Receive)
    memset(&attr, 0, sizeof(attr));
    attr.qp_state              = IBV_QPS_RTR;
    attr.path_mtu              = IBV_MTU_1024;
    attr.dest_qp_num           = remote_qpn;
    attr.rq_psn                = 0;
    attr.max_dest_rd_atomic    = 1;
    attr.min_rnr_timer         = 12;
    attr.ah_attr.is_global     = 1;
    attr.ah_attr.grh.dgid      = remote_gid;
    attr.ah_attr.grh.hop_limit = 1;
    attr.ah_attr.grh.sgid_index = GID_INDEX;
    attr.ah_attr.port_num      = IB_PORT;

    flags = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
            IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
            IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;

    if (ibv_modify_qp(qp, &attr, flags)) {
        fprintf(stderr, "failed to modify QP to RTR\n");
        return -1;
    }

    // RTR -> RTS (Ready to Send)
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

struct qp_info {
    uint32_t qpn;       // QP number
    union ibv_gid gid;  // GID (like IP address for RDMA)
};

void exchange_info_server(struct qp_info *local, struct qp_info *remote)
{
    int sockfd, connfd;
    struct sockaddr_in addr;

    sockfd = socket(AF_INET, SOCK_STREAM, 0);

    int opt = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(PORT);

    bind(sockfd, (struct sockaddr *)&addr, sizeof(addr));
    listen(sockfd, 1);

    connfd = accept(sockfd, NULL, NULL);

    // send local info to client
    send(connfd, local, sizeof(*local), 0);

    // receive remote info from client
    recv(connfd, remote, sizeof(*remote), 0);

    close(connfd);
    close(sockfd);
}

void exchange_info_client(struct qp_info *local, struct qp_info *remote,
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

    // receive remote info from server
    recv(sockfd, remote, sizeof(*remote), 0);

    // send local info to server
    send(sockfd, local, sizeof(*local), 0);

    close(sockfd);
}

int main(int argc, char *argv[])
{
    struct connection *conn;
    struct qp_info local_info, remote_info;
    union ibv_gid gid;
    int is_server = 0;

    if (argc < 2) {
        fprintf(stderr, "usage: %s <server|client> [server_ip]\n", argv[0]);
        return 1;
    }

    is_server = (strcmp(argv[1], "server") == 0);

    // init connection
    conn = init_connection();
    if (!conn) {
        fprintf(stderr, "failed to init connection\n");
        return 1;
    }

    // get local GID
    ibv_query_gid(conn->ctx, IB_PORT, GID_INDEX, &gid);

    // fill local qp info
    local_info.qpn = conn->qp->qp_num;
    local_info.gid = gid;

    // exchange info
    if (is_server) {
        exchange_info_server(&local_info, &remote_info);
    } else {
        if (argc < 3) {
            fprintf(stderr, "client needs server IP\n");
            return 1;
        }
        exchange_info_client(&local_info, &remote_info, argv[2]);
    }

    // bring QP to RTS
    modify_qp_to_rts(conn->qp, remote_info.qpn, 0, remote_info.gid);

    if (is_server) {
        // post receive WR
        struct ibv_sge sge = {
            .addr   = (uint64_t)conn->buf,
            .length = MSG_SIZE,
            .lkey   = conn->mr->lkey,
        };
        struct ibv_recv_wr wr = {
            .wr_id   = 0,
            .sg_list = &sge,
            .num_sge = 1,
        };
        struct ibv_recv_wr *bad_wr;
        r(conn->qp, &wr, &bad_wr);

        // poll CQ
        struct ibv_wc wc;
        while (ibv_poll_cq(conn->cq, 1, &wc) == 0);

        if (wc.status == IBV_WC_SUCCESS)
            printf("server received: %s\n", conn->buf);
        else
            fprintf(stderr, "recv failed: %d\n", wc.status);

    } else {
        // fill buffer
        strncpy(conn->buf, MSG, MSG_SIZE);

        // post send WR
        struct ibv_sge sge = {
            .addr   = (uint64_t)conn->buf,
            .length = MSG_SIZE,
            .lkey   = conn->mr->lkey,
        };
        struct ibv_send_wr wr = {
            .wr_id      = 0,
            .sg_list    = &sge,
            .num_sge    = 1,
            .opcode     = IBV_WR_SEND,
            .send_flags = IBV_SEND_SIGNALED,
        };
        struct ibv_send_wr *bad_wr;
        ibv_post_send(conn->qp, &wr, &bad_wr);

        // poll CQ
        struct ibv_wc wc;
        while (ibv_poll_cq(conn->cq, 1, &wc) == 0);

        if (wc.status == IBV_WC_SUCCESS)
            printf("client sent: %s\n", conn->buf);
        else
            fprintf(stderr, "send failed: %d\n", wc.status);
    }

    return 0;
}