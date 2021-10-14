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
	uint8_t alive;                     // request是否alive
	uint8_t threads;                   // 线程数,Number of threads to use
	int32_t duration;                  // 持续时间,秒
	uint32_t comm_per_second_limit;    // 每秒连接数上限
	uint32_t comm_alive;               // 活跃的连接数
	uint64_t comm_total_count_limit;   // 总连接数上限
	uint64_t comm_done;                // 已完成连接数
	uint64_t comm_done_time;           // 已完成连接数耗时,毫秒
	int epfd;                          // epoll
	int timerfd;                       // 定时器
	int timeoutfd;
	pthread_mutex_t mutex;             // 锁
	pthread_cond_t cond;               // 条件等待
	pthread_barrier_t barrier;         // 栅栏
	queue_t addr_root;                 // 地址根
	queue_t root;                      // 任务队列根
};

typedef struct{
	struct sockaddr * addr;
	socklen_t len;
	queue_t node;
}ADDR;

typedef struct{
	int fd;                            // fd
	void *arg;                         // 
	uint64_t request_time;             // 请求时间,毫秒
	uint64_t response_time;            // 响应时间,毫秒
	callback sendData;                 // callback
	callback recvData;                 // callback
	queue_t node;                      // 任务队列结点
}COMM;

requests_t* create_request(uint8_t threads, int32_t duration, uint32_t comm_per_second_limit,uint64_t comm_total_count_limit){
	requests_t *r = malloc(sizeof(requests_t));
	r->alive = 0;
	r->threads = threads;
	r->duration = duration;
	r->comm_per_second_limit = comm_per_second_limit;
	r->comm_alive = 0;
	r->comm_total_count_limit = comm_total_count_limit;
	r->comm_done = 0;
	r->comm_done_time = 0;
	r->epfd = -1;
	r->timerfd = -1;
	r->timeoutfd = -1;
	queue_init(&r->addr_root);
	pthread_mutex_init(&r->mutex,NULL);
	pthread_cond_init(&r->cond,NULL);
	queue_init(&r->root);
	return r;
}

int request_set_host_port(requests_t* r,struct sockaddr * addr,socklen_t len){
	ADDR *d = malloc(sizeof(ADDR));
	d->addr = NULL;
	d->len = 0;
	queue_init(&d->node);

	d->addr = malloc(len);
	memcpy(d->addr,addr,len);
	d->len = len;

	queue_insert_tail(&r->addr_root,&d->node);
	return 0;
}

int request_add_trans(requests_t* r,void *arg,callback sendData,callback recvData){
	COMM *comm = malloc(sizeof(COMM));
	comm->fd = -1;
	comm->arg = arg;
	comm->request_time = 0;
	comm->response_time = 0;
	comm->sendData = sendData;
	comm->recvData = recvData;
	queue_insert_tail(&r->root,&comm->node);
	return 0;
}

void* recv_handler(void*arg){
	requests_t* r = (requests_t*)arg;
	COMM *comm;
	int nready,i;

	struct epoll_event *events = malloc(sizeof(struct epoll_event) * r->comm_per_second_limit);
	//当未关闭或者还有活跃的连接数
	for(;r->alive==1 || r->comm_alive>0;){
		nready = epoll_wait(r->epfd,events,r->comm_per_second_limit,100);
		if(nready == 0){
			continue;
		}

		if(nready == -1){
			continue;
		}

		for(i=0;i<nready;i++){
			comm = (COMM*)events[i].data.ptr;
			if(comm->recvData(comm->fd,comm->arg) == -1){
				fprintf(stderr,"recv fail %d,%s\n",errno,strerror(errno));
			}
			struct timeval tv;
			gettimeofday(&tv,NULL);
			comm->response_time = tv.tv_sec*1000000 + tv.tv_usec;
			if(comm->fd == r->timerfd){
				continue;
			}
			else if(comm->fd == r->timeoutfd){
				epoll_ctl(r->epfd,EPOLL_CTL_DEL,comm->fd,NULL);
				close(r->timeoutfd);
				r->timeoutfd = -1;
				continue;	
			}else{
				epoll_ctl(r->epfd,EPOLL_CTL_DEL,comm->fd,NULL);
				shutdown(comm->fd,SHUT_WR);
				close(comm->fd);
				comm->fd = -1;
				pthread_mutex_lock(&r->mutex);
				r->comm_done++;
				r->comm_done_time += (comm->response_time-comm->request_time);
				queue_insert_tail(&r->root,&comm->node);
				r->comm_alive--;
				pthread_cond_signal(&r->cond);
				pthread_mutex_unlock(&r->mutex);
			}
		}
	}
	free(events);
	// pthread_cond_broadcast(&r->cond);
	pthread_exit(NULL);
}

int connect_sock(struct sockaddr* addr,socklen_t len){
	int socketfd = socket(addr->sa_family,SOCK_STREAM,0);
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
	ADDR *d;
	queue_t *addr_node;
	pthread_barrier_wait(&r->barrier);

	addr_node = queue_head(&r->addr_root);
	for(;r->alive==1 && r->comm_done < r->comm_total_count_limit;){
		pthread_mutex_lock(&r->mutex);
		while(queue_empty(&r->root) || r->comm_alive >= r->comm_per_second_limit){
			//当任务队列为空或者每秒连接数达到上限，则等待
			pthread_cond_wait(&r->cond,&r->mutex);
		}
		if(r->alive==0 || r->comm_done >= r->comm_total_count_limit){
			pthread_mutex_unlock(&r->mutex);
			break;
		}
		//获取任务队列头
		node = queue_head(&r->root);
		comm = queue_data(node,COMM,node);
		//并移除
		queue_remove(&comm->node);
		r->comm_alive++;
		pthread_mutex_unlock(&r->mutex);

		//connect
		d = queue_data(addr_node,ADDR,node);
		addr_node = queue_next(addr_node);
		if(addr_node == queue_sentinel(&r->addr_root)){
			addr_node = queue_head(&r->addr_root);
		}
		comm->fd = connect_sock(d->addr,d->len);
		if(comm->fd == -1){
			pthread_mutex_lock(&r->mutex);
			queue_insert_tail(&r->root,&comm->node);
			r->comm_alive--;
			pthread_mutex_unlock(&r->mutex);
			continue;
		}

		struct timeval tv;
		gettimeofday(&tv,NULL);
		comm->request_time = tv.tv_sec*1000000 + tv.tv_usec;

		//发送失败
		if(comm->sendData(comm->fd,comm->arg) == -1){
			fprintf(stderr,"send fail %d,%s\n",errno,strerror(errno));
			close(comm->fd);
			pthread_mutex_lock(&r->mutex);
			queue_insert_tail(&r->root,&comm->node);
			r->comm_alive--;
			pthread_mutex_unlock(&r->mutex);
			continue;
		}

		struct epoll_event ev;
		ev.data.ptr = comm;
		ev.events = EPOLLIN;
		epoll_ctl(r->epfd,EPOLL_CTL_ADD,comm->fd,&ev);
	}
	r->alive = 0;
	pthread_cond_broadcast(&r->cond);
	pthread_exit(NULL);
}

int recvTimerData(int fd,void *arg){
	static uint64_t old_comm_done = 0lu;
	requests_t* r = (requests_t*)arg;
	uint64_t count;
	read(fd,&count,sizeof(uint64_t));
	if(r->comm_done > 0){
		printf("alive %d, request %lu r/s, total_request %lu r, total_second %lf s，average cost %lf s/r\n",
		r->comm_alive,r->comm_done-old_comm_done,r->comm_done,
		r->comm_done_time/1000000.0,r->comm_done_time/1000000.0/r->comm_done);
	}else{
		printf("alive %d, request %lu r/s, total_request %lu r, total_second %lf s，average cost %lf s/r\n",
		r->comm_alive,r->comm_done-old_comm_done,r->comm_done,
		r->comm_done_time/1000000.0,0.0);
	}
	old_comm_done = r->comm_done;
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
	comm->arg = r;
	comm->request_time = 0;
	comm->response_time = 0;
	comm->sendData = NULL;
	comm->recvData = recvTimerData;

	struct epoll_event ev;
	ev.data.ptr = comm;
	ev.events = EPOLLIN;
	epoll_ctl(r->epfd,EPOLL_CTL_ADD,r->timerfd,&ev);
	return 0;
}

int timeout_handler(int fd,void *arg){
	requests_t* r = (requests_t*)arg;
	uint64_t count;
	read(fd,&count,sizeof(uint64_t));
	r->alive = 0;

	return 0;
}

int request_set_timeoutfd(requests_t* r){
	r->timeoutfd = timerfd_create(CLOCK_REALTIME, TFD_NONBLOCK);
	struct timespec startTime, intervalTime;
	startTime.tv_sec = r->duration;    
	startTime.tv_nsec = 0;                                
	intervalTime.tv_sec = 0;                             
	intervalTime.tv_nsec = 0;
	struct itimerspec newValue;
	newValue.it_value = startTime;                        //首次超时时间
	newValue.it_interval = intervalTime;                  //首次超时后，每隔it_interval超时一次
	timerfd_settime(r->timeoutfd, 0, &newValue, NULL);

	COMM *comm = malloc(sizeof(COMM));
	comm->fd = r->timeoutfd;
	comm->arg = r;
	comm->request_time = 0;
	comm->response_time = 0;
	comm->sendData = NULL;
	comm->recvData = timeout_handler;

	struct epoll_event ev;
	ev.data.ptr = comm;
	ev.events = EPOLLIN;
	epoll_ctl(r->epfd,EPOLL_CTL_ADD,r->timeoutfd,&ev);
	return 0;
}

int request_loop(requests_t* r){
	if(queue_empty(&r->addr_root)){
		fprintf(stderr,"please set addr\n");
		return -1;
	}

	if(queue_empty(&r->root)){
		fprintf(stderr,"please set trans\n");
		return -1;
	}

	pthread_t tid_recv_handler;
	pthread_t *tid_send_handler = malloc(sizeof(pthread_t) * r->threads);
	int i;
	time_t now;

	r->alive = 1;
	r->epfd = epoll_create(1);
	//设置定时器定时输出信息
	request_set_timerfd(r);
	//设置定时器超时结束
	if(r->duration > 0){
		request_set_timeoutfd(r);
	}
	//初始化锁
	pthread_mutex_init(&r->mutex,NULL);
	pthread_cond_init(&r->cond,NULL);
	pthread_barrier_init(&r->barrier,NULL,r->threads+1);

	now = time(NULL);

	//初始化接收线程
	pthread_create(&tid_recv_handler,NULL,recv_handler,r);
	//初始化发送线程
	for(i=0;i<r->threads;i++){
		pthread_create(&tid_send_handler[i],NULL,send_handler,r);
	}

	pthread_barrier_wait(&r->barrier);
	pthread_barrier_destroy(&r->barrier);

	//等待发送线程
	for(i=0;i<r->threads;i++){
		pthread_join(tid_send_handler[i],NULL);
	}
	free(tid_send_handler);

	//等待接收线程
	pthread_join(tid_recv_handler,NULL);

	pthread_mutex_lock(&r->mutex);
	pthread_cond_destroy(&r->cond);
	pthread_mutex_destroy(&r->mutex);

	close(r->epfd);
	r->epfd = -1;

	if(r->timeoutfd != -1){
		epoll_ctl(r->epfd,EPOLL_CTL_DEL,r->timeoutfd,NULL);
		close(r->timeoutfd);
		r->timeoutfd = -1;
	}

	if(r->timerfd != -1){
		epoll_ctl(r->epfd,EPOLL_CTL_DEL,r->timerfd,NULL);
		close(r->timerfd);
		r->timerfd = -1;
	}

	now = time(NULL)-now;
	if(r->comm_done > 0){
		printf("total use time %lu s\n",now);
		printf("total_request %lu r, total_second %lf s，average cost %lf s/r\n",
		r->comm_done,
		r->comm_done_time/1000000.0,r->comm_done_time/1000000.0/r->comm_done);
	}else{
		printf("total use time %lu s\n",now);
		printf("total_request %lu r, total_second %lf s，average cost %lf s/r\n",
		r->comm_done,
		r->comm_done_time/1000000.0,0.0);
	}

	return 0;
}

void request_destroy(requests_t* r){
	COMM *comm;
	queue_t *node;

	ADDR *d;
	queue_t *addr_node;
	while(!queue_empty(&r->addr_root)){
		addr_node = queue_head(&r->addr_root);
		d = queue_data(addr_node,ADDR,node);
		queue_remove(&d->node);
		free(d->addr);
		free(d);
	}

	while(!queue_empty(&r->root)){
		node = queue_head(&r->root);
		comm = queue_data(node,COMM,node);
		queue_remove(&comm->node);
		free(comm);
	}

	free(r);
}