#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <infiniband/verbs.h>
#include "rdma.h"

#define MSG            "Hello RDMA!"
#define MSG_SIZE       64
#define BENCH_MAX_SIZE (1 << 20)
#define DEMO_PORT      12345

#define BENCH_LAT 0
#define BENCH_BW  1

static const int SWEEP_SIZES[] = {
    64, 256, 1024, 4096, 16384, 65536, 262144, 1048576
};
#define N_SWEEP_SIZES ((int)(sizeof(SWEEP_SIZES) / sizeof(SWEEP_SIZES[0])))

struct bench_opts {
    int   mode;
    int   iters;
    int   warmup;
    int   size;
    int   sweep;
    int   depth;
    int   use_inline;
    char *output;
};

struct stats {
    double avg, min, p50, p99, max;
};

static int cmp_long(const void *a, const void *b)
{
    long x = *(const long *)a, y = *(const long *)b;
    return (x > y) - (x < y);
}

static struct stats compute_stats(long *s, int n)
{
    qsort(s, n, sizeof(long), cmp_long);
    double sum = 0;
    for (int i = 0; i < n; i++) sum += s[i];
    return (struct stats){
        .avg = sum / n / 1000.0,
        .min = s[0] / 1000.0,
        .p50 = s[n / 2] / 1000.0,
        .p99 = s[(int)(n * 0.99)] / 1000.0,
        .max = s[n - 1] / 1000.0,
    };
}

// ── functional ops ───────────────────────────────────────────────────────────

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
        if (rdma_poll_cq(conn->cq)) return -1;
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
        if (rdma_poll_cq(conn->cq)) return -1;
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
        if (rdma_wait_event(conn)) return -1;
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
        if (rdma_poll_cq(conn->cq)) return -1;
        printf("[write initiator] wrote: %s\n", conn->buf);
    }
    return 0;
}

static int op_read(struct connection *conn, struct qp_info *remote, int is_server)
{
    if (is_server) {
        strncpy(conn->buf, MSG, MSG_SIZE);
        printf("server: data ready, waiting for remote read...\n");
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
        if (rdma_poll_cq(conn->cq)) return -1;
        printf("[read] %s\n", conn->buf);
    }
    return 0;
}

static int op_atomic_cas(struct connection *conn, struct qp_info *remote, int is_server)
{
    uint64_t *val = (uint64_t *)conn->buf;

    if (is_server) {
        *val = 42ULL;
        printf("server: value=42, waiting for CAS...\n");
        sleep(2);
        printf("server: value after CAS = %lu (expected 99)\n", *val);
    } else {
        struct ibv_sge sge = {
            .addr   = (uint64_t)conn->buf,
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
                .remote_addr = remote->addr,
                .rkey        = remote->rkey,
                .compare_add = 42ULL,
                .swap        = 99ULL,
            },
        };
        struct ibv_send_wr *bad;
        if (ibv_post_send(conn->qp, &swr, &bad)) {
            fprintf(stderr, "post_send (CAS) failed\n");
            return -1;
        }
        if (rdma_poll_cq(conn->cq)) return -1;
        printf("[atomic-cas] original = %lu (expected 42), swapped to 99\n", *val);
    }
    return 0;
}

static int op_atomic_faa(struct connection *conn, struct qp_info *remote, int is_server)
{
    uint64_t *val = (uint64_t *)conn->buf;

    if (is_server) {
        *val = 0ULL;
        printf("server: counter=0, waiting for FAA x10...\n");
        sleep(3);
        printf("server: counter after FAA = %lu (expected 10)\n", *val);
    } else {
        struct ibv_sge sge = {
            .addr   = (uint64_t)conn->buf,
            .length = 8,
            .lkey   = conn->mr->lkey,
        };
        for (int i = 0; i < 10; i++) {
            struct ibv_send_wr swr = {
                .wr_id      = i,
                .sg_list    = &sge,
                .num_sge    = 1,
                .opcode     = IBV_WR_ATOMIC_FETCH_AND_ADD,
                .send_flags = IBV_SEND_SIGNALED,
                .wr.atomic  = {
                    .remote_addr = remote->addr,
                    .rkey        = remote->rkey,
                    .compare_add = 1ULL,
                },
            };
            struct ibv_send_wr *bad;
            if (ibv_post_send(conn->qp, &swr, &bad)) {
                fprintf(stderr, "post_send (FAA) failed at iter %d\n", i);
                return -1;
            }
            if (rdma_poll_cq(conn->cq)) return -1;
            printf("[atomic-faa] iter %2d: prev_val = %lu\n", i + 1, *val);
        }
        printf("[atomic-faa] done; server counter should be 10\n");
    }
    return 0;
}

// ── bench ─────────────────────────────────────────────────────────────────────

static void bench_sync(int fd)
{
    char b = 0;
    send_all(fd, &b, 1);
    recv_all(fd, &b, 1);
}

static int bench_lat(struct connection *conn, struct qp_info *remote,
                     int is_server, const char *op,
                     struct bench_opts *opts, int size, int syncfd, FILE *csv)
{
    int total     = opts->warmup + opts->iters;
    long *samples = NULL;
    int do_inline = opts->use_inline && (uint32_t)size <= conn->max_inline;

    struct ibv_sge sge = {
        .addr   = (uint64_t)conn->buf,
        .length = (uint32_t)size,
        .lkey   = conn->mr->lkey,
    };

    if (strcmp(op, "send-recv") == 0) {
        if (is_server) {
            struct ibv_recv_wr rwr = { .wr_id = 0, .sg_list = &sge, .num_sge = 1 };
            struct ibv_recv_wr *bad_rwr;
            ibv_post_recv(conn->qp, &rwr, &bad_rwr);
            bench_sync(syncfd);

            for (int i = 0; i < total; i++) {
                struct ibv_wc wc;
                while (ibv_poll_cq(conn->cq, 1, &wc) == 0);
                if (i < total - 1)
                    ibv_post_recv(conn->qp, &rwr, &bad_rwr);
                struct ibv_send_wr swr = {
                    .wr_id      = 0,
                    .sg_list    = &sge,
                    .num_sge    = 1,
                    .opcode     = IBV_WR_SEND,
                    .send_flags = IBV_SEND_SIGNALED | (do_inline ? IBV_SEND_INLINE : 0),
                };
                struct ibv_send_wr *bad_swr;
                ibv_post_send(conn->qp, &swr, &bad_swr);
                while (ibv_poll_cq(conn->cq, 1, &wc) == 0);
            }
            bench_sync(syncfd);
        } else {
            samples = malloc(opts->iters * sizeof(long));
            bench_sync(syncfd);

            for (int i = 0; i < total; i++) {
                long t0 = now_ns();
                struct ibv_send_wr swr = {
                    .wr_id      = 0,
                    .sg_list    = &sge,
                    .num_sge    = 1,
                    .opcode     = IBV_WR_SEND,
                    .send_flags = IBV_SEND_SIGNALED | (do_inline ? IBV_SEND_INLINE : 0),
                };
                struct ibv_send_wr *bad_swr;
                ibv_post_send(conn->qp, &swr, &bad_swr);
                struct ibv_wc wc;
                while (ibv_poll_cq(conn->cq, 1, &wc) == 0);
                struct ibv_recv_wr rwr = { .wr_id = 0, .sg_list = &sge, .num_sge = 1 };
                struct ibv_recv_wr *bad_rwr;
                ibv_post_recv(conn->qp, &rwr, &bad_rwr);
                while (ibv_poll_cq(conn->cq, 1, &wc) == 0);
                long t1 = now_ns();
                if (i >= opts->warmup)
                    samples[i - opts->warmup] = (t1 - t0) / 2;
            }
            bench_sync(syncfd);

            struct stats st = compute_stats(samples, opts->iters);
            printf("  %8d  %8.2f  %8.2f  %8.2f  %8.2f  %8.2f\n",
                   size, st.avg, st.min, st.p50, st.p99, st.max);
            if (csv)
                fprintf(csv, "%s,lat,%d,%d,%.2f,%.2f,%.2f,%.2f,%.2f,\n",
                        op, size, opts->iters,
                        st.avg, st.min, st.p50, st.p99, st.max);
            free(samples);
        }
    } else {
        bench_sync(syncfd);
        if (!is_server) {
            samples = malloc(opts->iters * sizeof(long));
            int opcode = (strcmp(op, "write") == 0)
                         ? IBV_WR_RDMA_WRITE : IBV_WR_RDMA_READ;
            int inline_flag = (opcode == IBV_WR_RDMA_WRITE && do_inline)
                              ? IBV_SEND_INLINE : 0;

            for (int i = 0; i < total; i++) {
                long t0 = now_ns();
                struct ibv_send_wr swr = {
                    .wr_id               = 0,
                    .sg_list             = &sge,
                    .num_sge             = 1,
                    .opcode              = opcode,
                    .send_flags          = IBV_SEND_SIGNALED | inline_flag,
                    .wr.rdma.remote_addr = remote->addr,
                    .wr.rdma.rkey        = remote->rkey,
                };
                struct ibv_send_wr *bad;
                ibv_post_send(conn->qp, &swr, &bad);
                struct ibv_wc wc;
                while (ibv_poll_cq(conn->cq, 1, &wc) == 0);
                long t1 = now_ns();
                if (i >= opts->warmup)
                    samples[i - opts->warmup] = t1 - t0;
            }

            struct stats st = compute_stats(samples, opts->iters);
            printf("  %8d  %8.2f  %8.2f  %8.2f  %8.2f  %8.2f\n",
                   size, st.avg, st.min, st.p50, st.p99, st.max);
            if (csv)
                fprintf(csv, "%s,lat,%d,%d,%.2f,%.2f,%.2f,%.2f,%.2f,\n",
                        op, size, opts->iters,
                        st.avg, st.min, st.p50, st.p99, st.max);
            free(samples);
        }
        bench_sync(syncfd);
    }

    return 0;
}

static int bench_bw(struct connection *conn, struct qp_info *remote,
                    int is_server, const char *op,
                    struct bench_opts *opts, int size, int syncfd, FILE *csv)
{
    int depth     = opts->depth;
    int iters     = opts->iters;
    int initial   = (depth < iters) ? depth : iters;
    int do_inline = opts->use_inline && (uint32_t)size <= conn->max_inline;

    struct ibv_sge sge = {
        .addr   = (uint64_t)conn->buf,
        .length = (uint32_t)size,
        .lkey   = conn->mr->lkey,
    };

    if (strcmp(op, "send-recv") == 0) {
        if (is_server) {
            for (int i = 0; i < initial; i++) {
                struct ibv_recv_wr rwr = { .wr_id = 0, .sg_list = &sge, .num_sge = 1 };
                struct ibv_recv_wr *bad;
                ibv_post_recv(conn->qp, &rwr, &bad);
            }
            bench_sync(syncfd);

            int completed = 0, posted = initial;
            while (completed < iters) {
                struct ibv_wc wc;
                if (ibv_poll_cq(conn->cq, 1, &wc) > 0) {
                    completed++;
                    if (posted < iters) {
                        struct ibv_recv_wr rwr = { .wr_id = 0, .sg_list = &sge, .num_sge = 1 };
                        struct ibv_recv_wr *bad;
                        ibv_post_recv(conn->qp, &rwr, &bad);
                        posted++;
                    }
                }
            }
            bench_sync(syncfd);
        } else {
            bench_sync(syncfd);

            long t0 = now_ns();
            for (int i = 0; i < initial; i++) {
                struct ibv_send_wr swr = {
                    .wr_id      = 0,
                    .sg_list    = &sge,
                    .num_sge    = 1,
                    .opcode     = IBV_WR_SEND,
                    .send_flags = IBV_SEND_SIGNALED | (do_inline ? IBV_SEND_INLINE : 0),
                };
                struct ibv_send_wr *bad;
                ibv_post_send(conn->qp, &swr, &bad);
            }

            int completed = 0, posted = initial;
            while (completed < iters) {
                struct ibv_wc wc;
                if (ibv_poll_cq(conn->cq, 1, &wc) > 0) {
                    completed++;
                    if (posted < iters) {
                        struct ibv_send_wr swr = {
                            .wr_id      = 0,
                            .sg_list    = &sge,
                            .num_sge    = 1,
                            .opcode     = IBV_WR_SEND,
                            .send_flags = IBV_SEND_SIGNALED | (do_inline ? IBV_SEND_INLINE : 0),
                        };
                        struct ibv_send_wr *bad;
                        ibv_post_send(conn->qp, &swr, &bad);
                        posted++;
                    }
                }
            }
            long t1 = now_ns();
            bench_sync(syncfd);

            double gbps = (double)iters * size / (t1 - t0);
            printf("  %8d  %10.2f  %10.1f\n", size, gbps, gbps * 8);
            if (csv)
                fprintf(csv, "%s,bw,%d,%d,,,,,,%.4f\n", op, size, iters, gbps);
        }
    } else {
        bench_sync(syncfd);
        if (!is_server) {
            int opcode = (strcmp(op, "write") == 0)
                         ? IBV_WR_RDMA_WRITE : IBV_WR_RDMA_READ;
            int inline_flag = (opcode == IBV_WR_RDMA_WRITE && do_inline)
                              ? IBV_SEND_INLINE : 0;

            long t0 = now_ns();
            for (int i = 0; i < initial; i++) {
                struct ibv_send_wr swr = {
                    .wr_id               = 0,
                    .sg_list             = &sge,
                    .num_sge             = 1,
                    .opcode              = opcode,
                    .send_flags          = IBV_SEND_SIGNALED | inline_flag,
                    .wr.rdma.remote_addr = remote->addr,
                    .wr.rdma.rkey        = remote->rkey,
                };
                struct ibv_send_wr *bad;
                ibv_post_send(conn->qp, &swr, &bad);
            }

            int completed = 0, posted = initial;
            while (completed < iters) {
                struct ibv_wc wc;
                if (ibv_poll_cq(conn->cq, 1, &wc) > 0) {
                    completed++;
                    if (posted < iters) {
                        struct ibv_send_wr swr = {
                            .wr_id               = 0,
                            .sg_list             = &sge,
                            .num_sge             = 1,
                            .opcode              = opcode,
                            .send_flags          = IBV_SEND_SIGNALED | inline_flag,
                            .wr.rdma.remote_addr = remote->addr,
                            .wr.rdma.rkey        = remote->rkey,
                        };
                        struct ibv_send_wr *bad;
                        ibv_post_send(conn->qp, &swr, &bad);
                        posted++;
                    }
                }
            }
            long t1 = now_ns();

            double gbps = (double)iters * size / (t1 - t0);
            printf("  %8d  %10.2f  %10.1f\n", size, gbps, gbps * 8);
            if (csv)
                fprintf(csv, "%s,bw,%d,%d,,,,,,%.4f\n", op, size, iters, gbps);
        }
        bench_sync(syncfd);
    }

    return 0;
}

static void parse_bench_opts(int argc, char *argv[], struct bench_opts *opts)
{
    opts->mode       = -1;
    opts->iters      = 0;
    opts->warmup     = 100;
    opts->size       = 64;
    opts->sweep      = 0;
    opts->depth      = 128;
    opts->use_inline = 0;
    opts->output     = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--mode") == 0 && i+1 < argc) {
            opts->mode = (strcmp(argv[++i], "lat") == 0) ? BENCH_LAT : BENCH_BW;
        } else if (strcmp(argv[i], "--iters") == 0 && i+1 < argc) {
            opts->iters = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--warmup") == 0 && i+1 < argc) {
            opts->warmup = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--size") == 0 && i+1 < argc) {
            opts->size = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--sweep") == 0) {
            opts->sweep = 1;
        } else if (strcmp(argv[i], "--depth") == 0 && i+1 < argc) {
            opts->depth = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--inline") == 0) {
            opts->use_inline = 1;
        } else if (strcmp(argv[i], "--output") == 0 && i+1 < argc) {
            opts->output = argv[++i];
        }
    }

    if (opts->iters == 0)
        opts->iters = (opts->mode == BENCH_LAT) ? 10000 : 1000;
}

static int run_bench(struct connection *conn, struct qp_info *remote,
                     int is_server, const char *op,
                     struct bench_opts *opts, int syncfd)
{
    if (strcmp(op, "atomic-cas") == 0 || strcmp(op, "atomic-faa") == 0) {
        fprintf(stderr, "atomic ops are not supported in bench mode\n");
        return -1;
    }

    FILE *csv = NULL;
    if (!is_server && opts->output) {
        csv = fopen(opts->output, "w");
        if (!csv) {
            fprintf(stderr, "failed to open output file: %s\n", opts->output);
            return -1;
        }
        fprintf(csv, "op,mode,size,iters,avg_us,min_us,p50_us,p99_us,max_us,gbps\n");
    }

    const int *sizes;
    int n_sizes;
    int single_size = opts->size;

    if (opts->sweep) {
        sizes   = SWEEP_SIZES;
        n_sizes = N_SWEEP_SIZES;
    } else {
        sizes   = &single_size;
        n_sizes = 1;
    }

    if (!is_server) {
        if (opts->mode == BENCH_LAT) {
            printf("op=%-10s  mode=lat  iters=%d  warmup=%d  inline=%s\n",
                   op, opts->iters, opts->warmup,
                   opts->use_inline ? "yes" : "no");
            printf("  %8s  %8s  %8s  %8s  %8s  %8s\n",
                   "size(B)", "avg(us)", "min(us)", "p50(us)", "p99(us)", "max(us)");
        } else {
            printf("op=%-10s  mode=bw  iters=%d  depth=%d  inline=%s\n",
                   op, opts->iters, opts->depth,
                   opts->use_inline ? "yes" : "no");
            printf("  %8s  %10s  %10s\n", "size(B)", "GB/s", "Gb/s");
        }
    }

    for (int i = 0; i < n_sizes; i++) {
        if (opts->mode == BENCH_LAT)
            bench_lat(conn, remote, is_server, op, opts, sizes[i], syncfd, csv);
        else
            bench_bw(conn, remote, is_server, op, opts, sizes[i], syncfd, csv);
    }

    if (csv) fclose(csv);
    return 0;
}

// ── main ──────────────────────────────────────────────────────────────────────

int main(int argc, char *argv[])
{
    const char *op, *role, *server_ip = NULL;
    int is_server, is_bench;
    struct connection *conn;
    struct qp_info local_info, remote_info;
    int syncfd;

    if (argc < 3) {
        fprintf(stderr,
            "usage:\n"
            "  %s <send-recv|write|read|atomic-cas|atomic-faa> <server|client> [ip]\n"
            "  %s bench <send-recv|write|read> <server|client> [ip] [options]\n"
            "options:\n"
            "  --mode lat|bw   --iters N  --warmup N\n"
            "  --size N        --sweep    --depth N\n"
            "  --inline        --output file.csv\n",
            argv[0], argv[0]);
        return 1;
    }

    is_bench = (strcmp(argv[1], "bench") == 0);

    if (is_bench) {
        if (argc < 5) {
            fprintf(stderr, "bench requires op and role\n");
            return 1;
        }
        op   = argv[2];
        role = argv[3];
    } else {
        op   = argv[1];
        role = argv[2];
    }

    if (strcmp(op, "send-recv")  != 0 &&
        strcmp(op, "write")      != 0 &&
        strcmp(op, "read")       != 0 &&
        strcmp(op, "atomic-cas") != 0 &&
        strcmp(op, "atomic-faa") != 0) {
        fprintf(stderr, "unknown op: %s\n", op);
        return 1;
    }

    if (strcmp(role, "server") == 0) {
        is_server = 1;
    } else if (strcmp(role, "client") == 0) {
        int ip_idx = is_bench ? 4 : 3;
        if (argc <= ip_idx) {
            fprintf(stderr, "client requires server_ip\n");
            return 1;
        }
        server_ip = argv[ip_idx];
        is_server = 0;
    } else {
        fprintf(stderr, "unknown role: %s\n", role);
        return 1;
    }

    conn = rdma_init(is_bench ? BENCH_MAX_SIZE : MSG_SIZE);
    if (!conn) return 1;

    printf("transport: %s",
           conn->link_layer == IBV_LINK_LAYER_INFINIBAND ? "InfiniBand" : "RoCE");
    if (conn->link_layer == IBV_LINK_LAYER_INFINIBAND)
        printf(" (LID %u)", conn->lid);
    printf(", max_inline=%u\n", conn->max_inline);

    rdma_fill_local_info(conn, &local_info);

    if (is_server)
        syncfd = rdma_exchange_server(&local_info, &remote_info, DEMO_PORT);
    else
        syncfd = rdma_exchange_client(&local_info, &remote_info, server_ip, DEMO_PORT);

    if (rdma_modify_qp(conn, &remote_info)) {
        fprintf(stderr, "failed to bring QP to RTS\n");
        return 1;
    }

    int ret = 0;
    if (is_bench) {
        struct bench_opts opts;
        parse_bench_opts(argc, argv, &opts);
        if (opts.mode < 0) {
            fprintf(stderr, "bench requires --mode lat|bw\n");
            return 1;
        }
        ret = run_bench(conn, &remote_info, is_server, op, &opts, syncfd);
    } else {
        close(syncfd);
        if      (strcmp(op, "send-recv")  == 0) ret = op_send_recv (conn, &remote_info, is_server);
        else if (strcmp(op, "write")      == 0) ret = op_write     (conn, &remote_info, is_server);
        else if (strcmp(op, "read")       == 0) ret = op_read      (conn, &remote_info, is_server);
        else if (strcmp(op, "atomic-cas") == 0) ret = op_atomic_cas(conn, &remote_info, is_server);
        else if (strcmp(op, "atomic-faa") == 0) ret = op_atomic_faa(conn, &remote_info, is_server);
    }

    if (is_bench) close(syncfd);
    return ret;
}
