CC = gcc
CFLAGS = -Wall -O2
LIBS = -libverbs

rdma_hello: rdma_hello.c
	$(CC) $(CFLAGS) -o rdma_hello rdma_hello.c $(LIBS)

rdma_write: rdma_write.c
	$(CC) $(CFLAGS) -o rdma_write rdma_write.c $(LIBS)

clean:
	rm -f rdma_hello
	rm -f rdma_hello rdma_write