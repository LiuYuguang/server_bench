#ifndef _SERVER_BENCH_H_
#define _SERVER_BENCH_H_

#include <sys/socket.h>

typedef struct requests_s requests_t;

requests_t* create_request(int threads, int duration, int connections,int total_connections);

int request_add_trans(requests_t* r,struct sockaddr * addr,socklen_t len,
    void *arg,int (*sendData)(int fd,void*arg),int (*recvData)(int fd,void*arg));

int request_loop(requests_t* r);

void request_destroy(requests_t* r);

#endif