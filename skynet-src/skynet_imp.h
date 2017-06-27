#ifndef SKYNET_IMP_H
#define SKYNET_IMP_H

//配置文件结构体
struct skynet_config {
	int thread;
	int harbor;
	int profile;
	const char * daemon;
	const char * module_path;
	const char * bootstrap;
	const char * logger;
	const char * logservice;
};

#define THREAD_WORKER 0 //工作线程
#define THREAD_MAIN 1 //主线程
#define THREAD_SOCKET 2 //socket线程
#define THREAD_TIMER 3 //时钟线程
#define THREAD_MONITOR 4 //监视器线程

void skynet_start(struct skynet_config * config);

#endif
