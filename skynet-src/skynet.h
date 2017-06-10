#ifndef SKYNET_H
#define SKYNET_H

#include "skynet_malloc.h"

#include <stddef.h>
#include <stdint.h>

#define PTYPE_TEXT 0
#define PTYPE_RESPONSE 1
#define PTYPE_MULTICAST 2
#define PTYPE_CLIENT 3
#define PTYPE_SYSTEM 4
#define PTYPE_HARBOR 5
#define PTYPE_SOCKET 6
// read lualib/skynet.lua examples/simplemonitor.lua
#define PTYPE_ERROR 7	
// read lualib/skynet.lua lualib/mqueue.lua lualib/snax.lua
#define PTYPE_RESERVED_QUEUE 8
#define PTYPE_RESERVED_DEBUG 9
#define PTYPE_RESERVED_LUA 10
#define PTYPE_RESERVED_SNAX 11

//skynet约定，每个服务发送出去的包都是复制到用 malloc 分配出来的连续内存。
//接收方在处理完这个数据块（在处理的 callback 函数调用完毕）后，会默认调用 free 函数释放掉所占的内存。
//即，发送方申请内存，接收方释放。
//** 如果不想让框架分配内存，然后拷贝内容进去，发送时设置该标志，callback中返回1
#define PTYPE_TAG_DONTCOPY 0x10000 //skynet在服务间发送消息不拷贝，直接发送指针和大小
#define PTYPE_TAG_ALLOCSESSION 0x20000 //是否分配session

struct skynet_context;

void skynet_error(struct skynet_context * context, const char *msg, ...);
const char * skynet_command(struct skynet_context * context, const char * cmd , const char * parm);
uint32_t skynet_queryname(struct skynet_context * context, const char * name);
int skynet_send(struct skynet_context * context, uint32_t source, uint32_t destination , int type, int session, void * msg, size_t sz);
int skynet_sendname(struct skynet_context * context, uint32_t source, const char * destination , int type, int session, void * msg, size_t sz);

int skynet_isremote(struct skynet_context *, uint32_t handle, int * harbor);

typedef int (*skynet_cb)(struct skynet_context * context, void *ud, int type, int session, uint32_t source , const void * msg, size_t sz);
void skynet_callback(struct skynet_context * context, void *ud, skynet_cb cb);

uint32_t skynet_current_handle(void);
uint64_t skynet_now(void);
void skynet_debug_memory(const char *info);	// for debug use, output current service memory to stderr

#endif
