#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <sys/timerfd.h>
#include <sys/epoll.h>
#include <pthread.h>
#include <errno.h>
#include <sys/time.h>

#include "queue.h"

#define HOST "127.0.0.1"
#define PORT 8888
#define MAX_COUNT 1000

queue_t root;//等待发送数据队列
int task_count = 0;
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t cond = PTHREAD_COND_INITIALIZER;

uint64_t total_count = 0;
uint64_t total_second = 0;

int epfd=-1;

typedef struct{
	int fd;
	uint64_t request_time;
	uint64_t response_time;
	int (*callback)(void* arg);
	queue_t node;
}TRANS;

int init_sock(){
	int socketfd = socket(AF_INET,SOCK_STREAM,0);
	if(socketfd == -1){
		return -1;
	}

	int flag = 1;
    if(setsockopt(socketfd,SOL_SOCKET,SO_REUSEADDR,&flag,sizeof(flag)) < 0){
        close(socketfd);
        return -1;
    }

	struct sockaddr_in dest;
	dest.sin_family = AF_INET;
	dest.sin_port = htons(PORT);
	inet_pton(AF_INET,HOST,&dest.sin_addr.s_addr);
	
	if(connect(socketfd,(struct sockaddr*)&dest,sizeof(dest)) == -1){
		close(socketfd);
		return -1;
	}
	
	return socketfd;
}

void* thread_run(void*arg){
	struct epoll_event events[MAX_COUNT+1];
	int nready,i;
	TRANS *trans;
	for(;;){
		nready = epoll_wait(epfd,events,MAX_COUNT+1,-1);
		if(nready == 0){
			continue;
		}

		if(nready == -1){
			continue;
		}

		for(i=0;i<nready;i++){
			trans = (TRANS*)events[i].data.ptr;
			trans->callback(trans);
		}
	}
	pthread_exit(NULL);
}

int set_http_header(char *data,int size)
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

int recvData_cb(void *arg);
int sendData_cb(void *arg){
	// printf("start send\n");
	TRANS *trans = (TRANS*)arg;

	trans->fd = init_sock();
	if(trans->fd == -1){
		printf("connect fail,errno%d,%s\n",errno,strerror(errno));
		return -1;
	}
	// printf("connect finish\n");
	//拼接数据
	char sendData[4096] = {0};
	int sendLen = 0;
	sendLen = sprintf(sendData,"<?xml version=\"1.0\" encoding=\"utf-8\"?><apple></apple>");
	sendLen = set_http_header(sendData,sendLen);
	// printf("sendData[%d][%s]\n",sendLen,sendData);
	//设置发送请求时间
	struct timeval tv;
	gettimeofday(&tv,NULL);
	trans->request_time = tv.tv_sec*1000000 + tv.tv_usec;

	//发送请求
	if(send(trans->fd,sendData,sendLen,0) == -1){
		printf("send fail,errno%d,%s\n",errno,strerror(errno));
		close(trans->fd);
		trans->fd = -1;
	}
	// printf("send finish\n");
	//设置响应callback
	trans->callback = recvData_cb;

	//发送成功，由子线程接收响应数据
	struct epoll_event ev;
	ev.data.ptr = trans;
	ev.events = EPOLLIN;
	epoll_ctl(epfd,EPOLL_CTL_ADD,trans->fd,&ev);
	return 0;
}

int recvData_cb(void *arg){
	// printf("start recv\n");
	TRANS *trans=(TRANS*)arg;

	//接收数据
	char recvData[4096] = {0};
	int recvLen = 0;
	recvLen = recv(trans->fd,recvData,sizeof(recvData),0);
	if(recvLen < 0){
		printf("recv fail,errno%d,%s\n",errno,strerror(errno));
	}
	//设置响应时间
	struct timeval tv;
	gettimeofday(&tv,NULL);
	trans->response_time = tv.tv_sec*1000000 + tv.tv_usec;

	//计算时差
	total_count++;
	total_second+=(trans->response_time-trans->request_time);

	//移除epoll
	epoll_ctl(epfd,EPOLL_CTL_DEL,trans->fd,NULL);

	//关闭fd
	close(trans->fd);
	trans->fd = -1;

	//重新加入任务队列, 并唤醒主线程
	pthread_mutex_lock(&mutex);
	queue_insert_tail(&root,&trans->node);
	task_count++;
	pthread_cond_signal(&cond);
	pthread_mutex_unlock(&mutex);

	return 0;
}

int per_count_cb(void *arg){
	TRANS *trans = (TRANS*)arg;
	uint64_t count;
	read(trans->fd,&count,sizeof(uint64_t));

	static uint64_t old_total_count = 0;
	if(total_count > 0){
		printf("task %d, request %lu r/s, total_count %lu r, total_second %lf s, average cost %lf s/r\n",
			task_count,
			total_count-old_total_count,
			total_count,
			total_second/1000000.0,
			(total_second/1000000.0)/total_count
		);
	}else{
		printf("task %d, request %lu r/s, total_count %lu r, total_second %lf s, average cost %lf s/r\n",
			task_count,
			total_count-old_total_count,
			total_count,
			total_second/1000000.0,
			0.0
		);
	}
	
	old_total_count = total_count;
	return 0;
}

int init_timer(){
	TRANS *trans;
	trans = malloc(sizeof(TRANS));
	trans->fd = timerfd_create(CLOCK_REALTIME, TFD_NONBLOCK);
	struct timespec startTime, intervalTime;
    startTime.tv_sec = 1;    
    startTime.tv_nsec = 0;                                
    intervalTime.tv_sec = 1;                             
    intervalTime.tv_nsec = 0;
	struct itimerspec newValue;
    newValue.it_value = startTime;                        //首次超时时间
    newValue.it_interval = intervalTime;                  //首次超时后，每隔it_interval超时一次
	
	timerfd_settime(trans->fd, 0, &newValue, NULL);
	trans->callback = per_count_cb;

	struct epoll_event ev;
	ev.data.ptr = trans;
	ev.events = EPOLLIN;
	epoll_ctl(epfd,EPOLL_CTL_ADD,trans->fd,&ev);
	return 0;
}

int main(){
	//创建epfd
    epfd = epoll_create(1);
    if(epfd == -1){
		printf("epoll_create fail, errno%d,%s\n",errno,strerror(errno));
		exit(1);
	}

	//创建等待发送数据队列root
	queue_init(&root);

	//创建所有待发送任务，并加入到队列尾
	TRANS *trans;
	queue_t *node;
	int i;
	for(i=0;i<MAX_COUNT;i++){
		trans = malloc(sizeof(TRANS));
		trans->fd = -1;
		queue_init(&trans->node);
		queue_insert_tail(&root,&trans->node);
		task_count++;
	}

	init_timer();

	//创建子线程接收数据
	pthread_t tid;
	pthread_create(&tid,NULL,thread_run,NULL);

	for(;;){
		pthread_mutex_lock(&mutex);
		while(queue_empty(&root)){
			//判断任务队列是否为空
			pthread_cond_wait(&cond,&mutex);
		}
		//获取任务队列头
		node = queue_head(&root);
		trans = queue_data(node,TRANS,node);
		//并移除
		queue_remove(&trans->node);
		task_count--;
		pthread_mutex_unlock(&mutex);

		
		//发送请求
		if(sendData_cb(trans) == -1){
			//发送失败, 放回任务队列尾
			pthread_mutex_lock(&mutex);
			queue_insert_tail(&root,&trans->node);
			task_count++;
			pthread_mutex_unlock(&mutex);
			continue;
		}
	}
}