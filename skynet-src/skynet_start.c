#include "skynet.h"
#include "skynet_server.h"
#include "skynet_imp.h"
#include "skynet_mq.h"
#include "skynet_handle.h"
#include "skynet_module.h"
#include "skynet_timer.h"
#include "skynet_monitor.h"
#include "skynet_socket.h"
#include "skynet_daemon.h"
#include "skynet_harbor.h"

#include <pthread.h>
#include <unistd.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

//全局的监测器
struct monitor {
	int count; //工作线程数量
	struct skynet_monitor ** m; //存储工作线程的监测器的指针数组，每个工作线程都有一个监测器
	pthread_cond_t cond; //条件变量
	pthread_mutex_t mutex; //互斥锁
	int sleep; //挂起的工作线程数量
	int quit; //标记是否退出
};

//工作线程的参数，monitor
struct worker_parm {
	struct monitor *m; //指向全局的检测器
	int id; //第几个工作线程
	int weight; //权重
};

static int SIG = 0;

static void
handle_hup(int signal) {
	if (signal == SIGHUP) {
		SIG = 1;
	}
}

//检查是否还有服务，无服务是，break 退出循环
#define CHECK_ABORT if (skynet_context_total()==0) break;

//创建线程
static void
create_thread(pthread_t *thread, void *(*start_routine) (void *), void *arg) {
	if (pthread_create(thread,NULL, start_routine, arg)) {
		fprintf(stderr, "Create thread failed");
		exit(1);
	}
}

//唤醒，给挂起线程发送信号，一次唤醒一个
static void
wakeup(struct monitor *m, int busy) {
	if (m->sleep >= m->count - busy) {
		// signal sleep worker, "spurious wakeup" is harmless
		pthread_cond_signal(&m->cond);
	}
}

//socket 线程
static void *
thread_socket(void *p) {
	struct monitor * m = p;
	skynet_initthread(THREAD_SOCKET);
	for (;;) {
		int r = skynet_socket_poll();
		if (r==0)
			break;
		if (r<0) {
			CHECK_ABORT
			continue;
		}
		//全部工作线程挂起时，则需要唤醒一个工作线程，处理socket消息
		wakeup(m,0);
	}
	return NULL;
}

//释放监控
static void
free_monitor(struct monitor *m) {
	int i;
	int n = m->count;
	for (i=0;i<n;i++) {
		skynet_monitor_delete(m->m[i]);
	}
	pthread_mutex_destroy(&m->mutex);
	pthread_cond_destroy(&m->cond);
	skynet_free(m->m);
	skynet_free(m);
}

//监控线程
static void *
thread_monitor(void *p) {
	struct monitor * m = p;
	int i;
	int n = m->count;
	skynet_initthread(THREAD_MONITOR);
	for (;;) {
		// #define CHECK_ABORT if (skynet_context_total()==0) break;
		CHECK_ABORT

		//监测每个工作线程
		for (i=0;i<n;i++) {
			skynet_monitor_check(m->m[i]);
		}
		//每五秒检查一次
		for (i=0;i<5;i++) {
			CHECK_ABORT
			sleep(1);
		}
	}

	return NULL;
}

static void
signal_hup() {
	// make log file reopen
	struct skynet_message smsg;
	smsg.source = 0;
	smsg.session = 0;
	smsg.data = NULL;
	smsg.sz = (size_t)PTYPE_SYSTEM << MESSAGE_TYPE_SHIFT;
	uint32_t logger = skynet_handle_findname("logger");
	if (logger) {
		skynet_context_push(logger, &smsg);
	}
}

static void *
thread_timer(void *p) {
	struct monitor * m = p;
	skynet_initthread(THREAD_TIMER);
	for (;;) {
		skynet_updatetime();
		CHECK_ABORT
		wakeup(m,m->count-1); //只要有挂起的线程，就唤醒一个
		usleep(2500); //挂起2.5毫秒
		if (SIG) {
			signal_hup();
			SIG = 0;
		}
	}
	// wakeup socket thread
	skynet_socket_exit();
	// wakeup all worker thread
	//标记退出，唤醒所有工作线程，使工作线程结束循环
	pthread_mutex_lock(&m->mutex);
	m->quit = 1; 
	pthread_cond_broadcast(&m->cond);
	pthread_mutex_unlock(&m->mutex);
	return NULL;
}

//工作线程
static void *
thread_worker(void *p) {
	struct worker_parm *wp = p;
	int id = wp->id;
	int weight = wp->weight;
	struct monitor *m = wp->m;
	struct skynet_monitor *sm = m->m[id];
	skynet_initthread(THREAD_WORKER);
	struct message_queue * q = NULL;
	while (!m->quit) {
		//分发消息，没消息处理就挂起
		q = skynet_context_message_dispatch(sm, q, weight);
		if (q == NULL) {
			if (pthread_mutex_lock(&m->mutex) == 0) {
				++ m->sleep;
				// "spurious wakeup" is harmless,
				// because skynet_context_message_dispatch() can be call at any time.
				if (!m->quit)
					pthread_cond_wait(&m->cond, &m->mutex); //挂起当前线程，等待条件，并解锁互斥量。当被唤醒返回时，再次锁住互斥量
				-- m->sleep;
				if (pthread_mutex_unlock(&m->mutex)) {
					fprintf(stderr, "unlock mutex error");
					exit(1);
				}
			}
		}
	}
	return NULL;
}

//开启线程
static void
start(int thread) {
	pthread_t pid[thread+3]; //skynet的线程数组，thread为工作线程数，外加 时钟线程 监视器线程 socket线程

	struct monitor *m = skynet_malloc(sizeof(*m));
	memset(m, 0, sizeof(*m));
	m->count = thread; //工作线程的数量
	m->sleep = 0;

	//为每个工作线程有一个监测器
	m->m = skynet_malloc(thread * sizeof(struct skynet_monitor *));
	int i;
	for (i=0;i<thread;i++) {
		m->m[i] = skynet_monitor_new(); //指针指向skynet_monitor
	}
	if (pthread_mutex_init(&m->mutex, NULL)) { //初始化互斥锁
		fprintf(stderr, "Init mutex error");
		exit(1);
	}
	if (pthread_cond_init(&m->cond, NULL)) { //初始化条件变量
		fprintf(stderr, "Init cond error");
		exit(1);
	}

	create_thread(&pid[0], thread_monitor, m); //创建monitor线程
	create_thread(&pid[1], thread_timer, m); //创建timer线程
	create_thread(&pid[2], thread_socket, m); //创建socket线程

	//创建thread个工作线程
	static int weight[] = { 
		-1, -1, -1, -1, 0, 0, 0, 0,
		1, 1, 1, 1, 1, 1, 1, 1, 
		2, 2, 2, 2, 2, 2, 2, 2, 
		3, 3, 3, 3, 3, 3, 3, 3, };
	struct worker_parm wp[thread];
	for (i=0;i<thread;i++) {
		wp[i].m = m;
		wp[i].id = i;
		if (i < sizeof(weight)/sizeof(weight[0])) {
			wp[i].weight= weight[i];
		} else {
			wp[i].weight = 0;
		}
		create_thread(&pid[i+3], thread_worker, &wp[i]);
	}

	//对非工作线程的三个线程进行回收
	for (i=0;i<thread+3;i++) {
		pthread_join(pid[i], NULL); 
	}

	//释放monitor
	free_monitor(m);
}

//启动bootstrap服务
//logger服务
//cmdline = "snlua bootstrap"
static void
bootstrap(struct skynet_context * logger, const char * cmdline) {
	int sz = strlen(cmdline);
	char name[sz+1];
	char args[sz+1];
	sscanf(cmdline, "%s %s", name, args); //name="snlua" args="bootstrap"
	struct skynet_context *ctx = skynet_context_new(name, args); //开启snlua服务
	if (ctx == NULL) {
		//创建失败，退出
		skynet_error(NULL, "Bootstrap error : %s\n", cmdline);
		skynet_context_dispatchall(logger);
		exit(1);
	}
}

void 
skynet_start(struct skynet_config * config) {
	//注册SIGHUP信号处理器
	// register SIGHUP for log file reopen
	struct sigaction sa;
	sa.sa_handler = &handle_hup; //信号处理器
	sa.sa_flags = SA_RESTART; //自动重启由信号处理器程序中断的系统调用，信号处理后，继续原来的系统调用
	sigfillset(&sa.sa_mask);
	sigaction(SIGHUP, &sa, NULL);

	//守护进程，后台启动
	if (config->daemon) {
		if (daemon_init(config->daemon)) {
			exit(1);
		}
	}
	skynet_harbor_init(config->harbor); //初始化harbor
	skynet_handle_init(config->harbor); //初始化服务地址管理
	skynet_mq_init(); //初始化消息队列
	skynet_module_init(config->module_path); //初始化服务模块
	skynet_timer_init(); //初始化时钟
	skynet_socket_init(); //初始化socket
	skynet_profile_enable(config->profile); //是否其中skynet统计

	//创建logger服务 skynet的第一个服务
	struct skynet_context *ctx = skynet_context_new(config->logservice, config->logger);
	if (ctx == NULL) {
		fprintf(stderr, "Can't launch %s service\n", config->logservice);
		exit(1);
	}

	//skynet的启动服务 skynet的第二个服务
	bootstrap(ctx, config->bootstrap);

	start(config->thread);

	// harbor_exit may call socket send, so it should exit before socket_free
	skynet_harbor_exit();
	skynet_socket_free();
	if (config->daemon) {
		daemon_exit(config->daemon);
	}
}
