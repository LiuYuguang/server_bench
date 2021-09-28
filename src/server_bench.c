#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <pthread.h>
#include <errno.h>
#include <string.h>
#include <sys/timerfd.h>
#include <sys/time.h>
#include "server_bench.h"
#include "queue.h"


struct requests_s{
	int alive;                    // request是否alive
	int threads;                  // 线程数,Number of threads to use
	int duration;                 // 持续时间,Duration of test
	int connections;              // 连接数上限,Connections to keep open
	int connections_alive;        // 活跃的任务数
	int total_connections;        // 总连接数
	int done_connections;         // 已完成连接数
	int64_t done_connections_time;// 已完成连接数耗时
	int epfd;                     // epoll
	int timerfd;                  // 定时器
	pthread_mutex_t mutex;        // 锁
	pthread_cond_t cond;          // 条件等待
	pthread_barrier_t barrier;    // 栅栏
	queue_t root;                 // 任务队列根
};

typedef struct{
	int fd;                               // fd
	struct sockaddr * addr;               // 地址
	socklen_t len;                        // 地址长度
	void *arg;                            // 
	uint64_t request_time;                // 请求时间
	uint64_t response_time;               // 响应时间
	int (*sendData)(int fd, void*arg);    // callback
	int (*recvData)(int fd, void*arg);    // callback
	queue_t node;                         // 任务队列结点
}COMM;

requests_t* create_request(int threads, int duration, int connections,int total_connections){
	requests_t *r = malloc(sizeof(requests_t));
	r->alive = 0;
	r->threads = threads;
	r->duration = duration;
	r->connections = connections;
	r->connections_alive = 0;
	r->total_connections = total_connections;
	r->done_connections = 0;
	r->epfd = -1;
	pthread_mutex_init(&r->mutex,NULL);
	pthread_cond_init(&r->cond,NULL);
	queue_init(&r->root);
	return r;
}

int request_add_trans(requests_t* r,struct sockaddr * addr,socklen_t len,void *arg,int (*sendData)(int fd,void*arg),int (*recvData)(int fd,void*arg)){
	COMM *comm = malloc(sizeof(COMM));
	comm->fd = -1;
	comm->addr = addr;
	comm->len = len;
	comm->arg = arg;
	comm->sendData = sendData;
	comm->recvData = recvData;
	queue_insert_tail(&r->root,&comm->node);
	return 0;
}

void* recv_handler(void*arg){
	requests_t* r = (requests_t*)arg;
	COMM *comm;
	int nready,i;

	struct epoll_event *events = malloc(sizeof(struct epoll_event) * r->connections);
	for(;r->alive==1 || r->connections_alive>0;){
		nready = epoll_wait(r->epfd,events,r->connections,100);
		if(nready == 0){
			continue;
		}

		if(nready == -1){
			continue;
		}

		for(i=0;i<nready;i++){
			comm = (COMM*)events[i].data.ptr;
			comm->recvData(comm->fd,comm->arg);
			struct timeval tv;
			gettimeofday(&tv,NULL);
			comm->response_time = tv.tv_sec*1000000 + tv.tv_usec;
			if(comm->fd == r->timerfd)
				continue;
			epoll_ctl(r->epfd,EPOLL_CTL_DEL,comm->fd,NULL);
			close(comm->fd);
			comm->fd = -1;
			pthread_mutex_lock(&r->mutex);
			r->done_connections++;
			r->done_connections_time += (comm->response_time-comm->request_time);
			queue_insert_tail(&r->root,&comm->node);
			r->connections_alive--;
			pthread_cond_signal(&r->cond);
			pthread_mutex_unlock(&r->mutex);
		}
	}
	free(events);
	pthread_cond_broadcast(&r->cond);
	pthread_exit(NULL);
}

int connect_sock(struct sockaddr* addr,socklen_t len){
	int socketfd = socket(AF_INET,SOCK_STREAM,0);
	if(socketfd == -1){
		return -1;
	}

	int flag = 1;
    if(setsockopt(socketfd,SOL_SOCKET,SO_REUSEADDR,&flag,sizeof(flag)) < 0){
        close(socketfd);
        return -1;
    }
	
	if(connect(socketfd,addr,len) == -1){
		fprintf(stderr,"connect fail %d,%s\n",errno,strerror(errno));
		close(socketfd);
		return -1;
	}
	
	return socketfd;
}

void* send_handler(void*arg){
	requests_t* r = (requests_t*)arg;
	COMM *comm;
	queue_t *node;

	pthread_barrier_wait(&r->barrier);

	for(;r->done_connections < r->total_connections;){
		pthread_mutex_lock(&r->mutex);
		while(queue_empty(&r->root) || r->connections_alive >= r->connections){
			//判断任务队列是否为空
			pthread_cond_wait(&r->cond,&r->mutex);
		}
		if(r->done_connections >= r->total_connections){
			pthread_mutex_unlock(&r->mutex);
			break;
		}
		//获取任务队列头
		node = queue_head(&r->root);
		comm = queue_data(node,COMM,node);
		//并移除
		queue_remove(&comm->node);
		r->connections_alive++;
		// r->done_connections++;
		// printf("alive %d, total_connections %d, done %d\n",r->connections_alive,r->total_connections,r->done_connections);
		pthread_mutex_unlock(&r->mutex);

		//connect
		comm->fd = connect_sock(comm->addr,comm->len);
		if(comm->fd == -1){
			pthread_mutex_lock(&r->mutex);
			queue_insert_tail(&r->root,&comm->node);
			r->connections_alive--;
			// r->done_connections--;
			pthread_mutex_unlock(&r->mutex);
			//sleep
			continue;
		}

		struct timeval tv;
		gettimeofday(&tv,NULL);
		comm->request_time = tv.tv_sec*1000000 + tv.tv_usec;

		//发送失败
		if(comm->sendData(comm->fd,comm->arg) == -1){
			close(comm->fd);
			pthread_mutex_lock(&r->mutex);
			queue_insert_tail(&r->root,&comm->node);
			r->connections_alive--;
			// r->done_connections--;
			pthread_mutex_unlock(&r->mutex);
			//sleep
			continue;
		}

		struct epoll_event ev;
		ev.data.ptr = comm;
		ev.events = EPOLLIN;
		epoll_ctl(r->epfd,EPOLL_CTL_ADD,comm->fd,&ev);
	}
	pthread_exit(NULL);
}

int recvTimerData(int fd,void *arg){
	requests_t* r = (requests_t*)arg;
	uint64_t count;
	read(fd,&count,sizeof(uint64_t));
	if(r->done_connections > 0){
		printf("alive request %d, total_request %lu r, total_second %lf s，average cost %lf s/r\n",
		r->connections_alive,r->done_connections,r->done_connections_time/1000000.0,r->done_connections_time/1000000.0/r->done_connections);
	}else{
		printf("alive request %d, total_request %lu r, total_second %lf s，average cost %lf s/r\n",
		r->connections_alive,r->done_connections,r->done_connections_time/1000000.0,0.0);
	}
	return 0;
}

int request_set_timerfd(requests_t* r){
	r->timerfd = timerfd_create(CLOCK_REALTIME, TFD_NONBLOCK);
	struct timespec startTime, intervalTime;
    startTime.tv_sec = 1;    
    startTime.tv_nsec = 0;                                
    intervalTime.tv_sec = 1;                             
    intervalTime.tv_nsec = 0;
	struct itimerspec newValue;
    newValue.it_value = startTime;                        //首次超时时间
    newValue.it_interval = intervalTime;                  //首次超时后，每隔it_interval超时一次
	timerfd_settime(r->timerfd, 0, &newValue, NULL);

	COMM *comm = malloc(sizeof(COMM));
	comm->fd = r->timerfd;
	comm->addr = NULL;
	comm->len = 0;
	comm->arg = r;
	comm->sendData = NULL;
	comm->recvData = recvTimerData;

	struct epoll_event ev;
	ev.data.ptr = comm;
	ev.events = EPOLLIN;
	epoll_ctl(r->epfd,EPOLL_CTL_ADD,r->timerfd,&ev);
	return 0;
}

int request_loop(requests_t* r){
	pthread_t tid_recv_handler;
	pthread_t *tid_send_handler = malloc(sizeof(pthread_t) * r->threads);
	int i;

	r->alive = 1;
	r->epfd = epoll_create(1);
	request_set_timerfd(r);
	pthread_mutex_init(&r->mutex,NULL);
	pthread_cond_init(&r->cond,NULL);
	
	pthread_create(&tid_recv_handler,NULL,recv_handler,r);

	pthread_barrier_init(&r->barrier,NULL,r->threads+1);
	for(i=0;i<r->threads;i++){
		pthread_create(&tid_send_handler[i],NULL,send_handler,r);
	}
	
	pthread_barrier_wait(&r->barrier);
	pthread_barrier_destroy(&r->barrier);
	
	for(i=0;i<r->threads;i++){
		pthread_join(tid_send_handler[i],NULL);
	}
	
	r->alive = 0;
	pthread_join(tid_recv_handler,NULL);
	
	pthread_mutex_lock(&r->mutex);
	pthread_cond_destroy(&r->cond);
	pthread_mutex_destroy(&r->mutex);

	close(r->epfd);
	return 0;
}

void request_destroy(requests_t* r){
	COMM *comm;
	queue_t *node;

	while(!queue_empty(&r->root)){
		node = queue_head(&r->root);
		comm = queue_data(node,COMM,node);
		queue_remove(&comm->node);
		free(comm);
	}

	free(r);
}