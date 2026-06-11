CC     = gcc
CFLAGS = -Wall -O2
LIBS   = -libverbs

RDMA_OBJS = rdma.o
IMPL_OBJS = impls/kv_pilaf.o

all: rdma_demo kv_server kv_bench

rdma_demo: rdma_demo.o $(RDMA_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

kv_server: kv_server.o $(IMPL_OBJS) $(RDMA_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

kv_bench: kv_bench.o $(IMPL_OBJS) $(RDMA_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

impls/%.o: impls/%.c
	$(CC) $(CFLAGS) -I. -c -o $@ $<

clean:
	rm -f *.o impls/*.o rdma_demo kv_server kv_bench
