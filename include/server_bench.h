#ifndef _SERVER_BENCH_H_
#define _SERVER_BENCH_H_

#include <sys/socket.h>

typedef struct requests_s requests_t;

requests_t* create_request(uint8_t threads, int32_t duration, uint32_t connections,uint64_t total_connections);

int request_set_host_port(requests_t* r,struct sockaddr * addr,socklen_t len);

typedef int (*callback)(int fd,void*arg);

int request_add_trans(requests_t* r,void *arg,callback sendData,callback recvData);

int request_loop(requests_t* r);

void request_destroy(requests_t* r);

#endif