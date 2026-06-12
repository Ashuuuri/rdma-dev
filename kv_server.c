#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "kv.h"

static const struct kv_ops *select_impl(const char *name)
{
    if (strcmp(name, "pilaf") == 0) return &kv_pilaf_ops;
    if (strcmp(name, "herd")  == 0) return &kv_herd_ops;
    fprintf(stderr, "unknown impl: %s (choices: pilaf, herd)\n", name);
    return NULL;
}

int main(int argc, char *argv[])
{
    const char *impl_name = (argc >= 2) ? argv[1] : "pilaf";
    const struct kv_ops *ops = select_impl(impl_name);
    if (!ops) return 1;

    size_t srv_buf = ops->server_buf_size ? ops->server_buf_size : KV_SERVER_BUF_SIZE;
    struct connection *conn = rdma_init(srv_buf);
    if (!conn) return 1;

    struct kv_slot *table = (struct kv_slot *)conn->buf;
    ops->server_init(table);

    struct qp_info base_local;
    rdma_fill_local_info(conn, &base_local);
    base_local.rkey = conn->mr->rkey;
    base_local.addr = (uint64_t)table;

    int num_clients = ops->num_clients > 0 ? ops->num_clients : 1;

    printf("kv_server: impl=%s  table=%zu B  port=%d  clients=%d\n",
           ops->name, ops->table_size, KV_PORT, num_clients);

    int lsock = rdma_listen(KV_PORT);
    if (lsock < 0) { perror("rdma_listen"); return 1; }

    for (int cid = 0; cid < num_clients; cid++) {
        struct qp_info local  = base_local;
        struct qp_info remote;
        local.client_id = cid;

        if (ops->pre_exchange && ops->pre_exchange(conn, &local, cid, 1)) {
            close(lsock); return 1;
        }

        int syncfd = rdma_accept(lsock, &local, &remote);
        if (syncfd < 0) {
            fprintf(stderr, "rdma_accept failed for cid %d\n", cid);
            close(lsock); return 1;
        }

        if (!ops->skip_rc_qp && rdma_modify_qp(conn, &remote)) {
            close(lsock); return 1;
        }
        if (rdma_barrier(syncfd)) { close(lsock); return 1; }

        if (ops->post_exchange &&
            ops->post_exchange(conn, &remote, syncfd, cid, 1)) {
            close(lsock); return 1;
        }
        close(syncfd);
    }
    close(lsock);

    printf("kv_server: ready\n");

    if (ops->server_run) {
        ops->server_run(conn);
    } else {
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
    }

    return 0;
}
