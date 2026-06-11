#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "kv.h"

#define DEFAULT_ITERS     10000
#define DEFAULT_KEY_RANGE 256

static const struct kv_ops *select_impl(const char *name)
{
    if (strcmp(name, "pilaf") == 0) return &kv_pilaf_ops;
    fprintf(stderr, "unknown impl: %s (choices: pilaf)\n", name);
    return NULL;
}

static int cmp_long(const void *a, const void *b)
{
    long x = *(const long *)a, y = *(const long *)b;
    return (x > y) - (x < y);
}

static void print_stats(const char *label, long *samples, int n)
{
    qsort(samples, n, sizeof(long), cmp_long);
    double sum = 0;
    for (int i = 0; i < n; i++) sum += samples[i];
    printf("%-8s  avg=%6.1f  min=%6.1f  p50=%6.1f  p99=%6.1f  max=%6.1f  (us)\n",
           label,
           sum / n / 1000.0,
           samples[0] / 1000.0,
           samples[n / 2] / 1000.0,
           samples[(int)(n * 0.99)] / 1000.0,
           samples[n - 1] / 1000.0);
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "usage: %s <server_ip> [options]\n"
        "  --impl  pilaf        implementation (default: pilaf)\n"
        "  --op    get|put|mixed  operation (default: get)\n"
        "  --iters N            iterations (default: %d)\n"
        "  --key-range N        number of distinct keys (default: %d)\n",
        prog, DEFAULT_ITERS, DEFAULT_KEY_RANGE);
}

int main(int argc, char *argv[])
{
    if (argc < 2) { usage(argv[0]); return 1; }

    const char *server_ip  = argv[1];
    const char *impl_name  = "pilaf";
    const char *op         = "get";
    int         iters      = DEFAULT_ITERS;
    int         key_range  = DEFAULT_KEY_RANGE;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--impl")      == 0 && i+1 < argc) impl_name = argv[++i];
        else if (strcmp(argv[i], "--op")   == 0 && i+1 < argc) op        = argv[++i];
        else if (strcmp(argv[i], "--iters") == 0 && i+1 < argc) iters    = atoi(argv[++i]);
        else if (strcmp(argv[i], "--key-range") == 0 && i+1 < argc) key_range = atoi(argv[++i]);
        else { fprintf(stderr, "unknown arg: %s\n", argv[i]); return 1; }
    }

    const struct kv_ops *ops = select_impl(impl_name);
    if (!ops) return 1;

    struct connection *conn = rdma_init(KV_CLIENT_BUF_SIZE);
    if (!conn) return 1;

    struct qp_info local, remote;
    rdma_fill_local_info(conn, &local);

    int syncfd = rdma_exchange_client(&local, &remote, server_ip, KV_PORT);
    if (syncfd < 0) { fprintf(stderr, "exchange failed\n"); return 1; }

    if (rdma_modify_qp(conn, &remote)) return 1;
    if (rdma_barrier(syncfd)) return 1;

    printf("kv_bench: impl=%s  op=%s  iters=%d  key_range=%d\n",
           impl_name, op, iters, key_range);

    // Seed the server with some keys so GET has something to find
    if (strcmp(op, "get") == 0 || strcmp(op, "mixed") == 0) {
        uint8_t val[KV_VALUE_SIZE];
        memset(val, 0xab, sizeof(val));
        printf("pre-loading %d keys...\n", key_range);
        for (int k = 0; k < key_range; k++) {
            if (ops->put(conn, &remote, (uint64_t)k, val)) {
                fprintf(stderr, "pre-load PUT failed at key %d\n", k);
                return 1;
            }
        }
        printf("pre-load done\n");
    }

    long *samples = malloc(iters * sizeof(long));
    if (!samples) { perror("malloc"); return 1; }

    uint8_t val[KV_VALUE_SIZE];
    memset(val, 0, sizeof(val));

    int errors = 0;

    for (int i = 0; i < iters; i++) {
        uint64_t key = (uint64_t)(i % key_range);
        long t0 = now_ns();
        int r;

        if (strcmp(op, "get") == 0) {
            r = ops->get(conn, &remote, key, val);
        } else if (strcmp(op, "put") == 0) {
            val[0] = (uint8_t)i;
            r = ops->put(conn, &remote, key, val);
        } else {
            // mixed: even iters → GET, odd iters → PUT
            if (i % 2 == 0)
                r = ops->get(conn, &remote, key, val);
            else {
                val[0] = (uint8_t)i;
                r = ops->put(conn, &remote, key, val);
            }
        }

        long t1 = now_ns();
        samples[i] = t1 - t0;
        if (r < 0) errors++;
    }

    print_stats(op, samples, iters);
    if (errors)
        fprintf(stderr, "kv_bench: %d errors\n", errors);

    free(samples);
    close(syncfd);
    return errors ? 1 : 0;
}
