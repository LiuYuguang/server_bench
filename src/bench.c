#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>

#include "server_bench.h"

#define HOST "127.0.0.1"
#define PORT 8888

inline int set_http_header(char *data,int size)
{
	char tmp[512] = {0};
	int len=0;
	len = sprintf(tmp, 
		"POST / HTTP/1.1\r\n"
        "Content-Type: application/xml\r\n"
        "Connection: close\r\n"
		"Host: %s:%d\r\n"
        "Content-Length: %d\r\n"
		"\r\n",HOST,PORT,size);
	memmove(data+len,data,size);
	memmove(data,tmp,len);
	
	return len+size;
}

int sendData(int fd,void*arg){
    int n = *(int*)arg;
    char sendData[4096] = {0};
	int sendLen = 0;
	sendLen = sprintf(sendData,"<?xml version=\"1.0\" encoding=\"utf-8\"?><apple>%d</apple>",n);
	sendLen = set_http_header(sendData,sendLen);

    sendLen = send(fd,sendData,sendLen,0);
    return sendLen;
}

int recvData(int fd,void*arg){
    char recvData[4096] = {0};
	int recvLen = 0;

    recvLen =  recv(fd,recvData,sizeof(recvData),0);
    return recvLen;
}

int main(){
    requests_t* r = create_request(4,3,10,100);

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    inet_pton(AF_INET,HOST,&addr.sin_addr.s_addr);

    request_set_host_port(r,(struct sockaddr *)&addr,sizeof(addr));
    
    int data[1000];
    int i;
    for(i=0;i<1000;i++){
        data[i] = i;
        request_add_trans(r,&data[i],sendData,recvData);
    }
    
    request_loop(r);

    request_destroy(r);
    return 0;
}