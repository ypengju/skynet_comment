#include "skynet.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

struct logger {
	FILE * handle; //打开的log文件
	char * filename; //文件名称
	int close; // 文件状态， 0 表示关闭，1 表示打开
};

//创建一个looger结构
struct logger *
logger_create(void) {
	struct logger * inst = skynet_malloc(sizeof(*inst));
	inst->handle = NULL;
	inst->close = 0;
	inst->filename = NULL;

	return inst;
}

void
logger_release(struct logger * inst) {
	if (inst->close) {
		fclose(inst->handle);
	}
	skynet_free(inst->filename);
	skynet_free(inst);
}

//log服务的回掉
static int
logger_cb(struct skynet_context * context, void *ud, int type, int session, uint32_t source, const void * msg, size_t sz) {
	struct logger * inst = ud;
	switch (type) {
	case PTYPE_SYSTEM:
		if (inst->filename) {
			inst->handle = freopen(inst->filename, "a", inst->handle);
		}
		break;
	case PTYPE_TEXT:
		fprintf(inst->handle, "[:%08x] ",source);
		fwrite(msg, sz , 1, inst->handle);
		fprintf(inst->handle, "\n");
		fflush(inst->handle);
		break;
	}

	return 0;
}

//logger初始化，parm非空，则logger输出到parm指定的文件，否则输出的标准输出
int
logger_init(struct logger * inst, struct skynet_context *ctx, const char * parm) {
	if (parm) {
		inst->handle = fopen(parm,"w");
		if (inst->handle == NULL) {
			return 1;
		}
		inst->filename = skynet_malloc(strlen(parm)+1);
		strcpy(inst->filename, parm);
		inst->close = 1;
	} else {
		inst->handle = stdout;
	}
	if (inst->handle) {
		skynet_callback(ctx, inst, logger_cb);
		skynet_command(ctx, "REG", ".logger");
		return 0;
	}
	return 1;
}
