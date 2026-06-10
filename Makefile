CC     = gcc
CFLAGS = -Wall -O2
LIBS   = -libverbs

rdma_demo: rdma_demo.c
	$(CC) $(CFLAGS) -o rdma_demo rdma_demo.c $(LIBS)

clean:
	rm -f rdma_demo
