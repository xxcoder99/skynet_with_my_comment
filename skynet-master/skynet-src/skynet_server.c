#include "skynet.h"

#include "skynet_server.h"
#include "skynet_module.h"
#include "skynet_handle.h"
#include "skynet_mq.h"
#include "skynet_timer.h"
#include "skynet_harbor.h"
#include "skynet_env.h"
#include "skynet_monitor.h"
#include "skynet_imp.h"
#include "skynet_log.h"
#include "skynet_timer.h"
#include "spinlock.h"
#include "atomic.h"

#include <pthread.h>

#include <string.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>

#ifdef CALLING_CHECK

#define CHECKCALLING_BEGIN(ctx) if (!(spinlock_trylock(&ctx->calling))) { assert(0); }
#define CHECKCALLING_END(ctx) spinlock_unlock(&ctx->calling);
#define CHECKCALLING_INIT(ctx) spinlock_init(&ctx->calling);
#define CHECKCALLING_DESTROY(ctx) spinlock_destroy(&ctx->calling);
#define CHECKCALLING_DECL struct spinlock calling;

#else

#define CHECKCALLING_BEGIN(ctx)
#define CHECKCALLING_END(ctx)
#define CHECKCALLING_INIT(ctx)
#define CHECKCALLING_DESTROY(ctx)
#define CHECKCALLING_DECL

#endif


//skynet 主要功能 加载服务和通知服务
/*
*	一个模块(.so)加载到skynet框架中，创建出来的一个实例就是一个服务
*	为每个服务分配一个skynet_context结构
*/

//每一个服务对应的sy==skynet_ctx结构

//主要承载框架的调度逻辑 简称  sc

struct skynet_context {

	//契约函数create创建
	//调用动态库中的xxxxx_create()函数的返回值  即mod指向的结构体中的,自定义的与模块相关的结构体
	void * instance;		//模块xxx_create函数返回的实例 对应模块的句柄

	//皮囊对象
	struct skynet_module * mod;		//模块,就是 封装动态库的结构体 


	//回调函数的用户数据
	void * cb_ud;		//传递给回调函数的参数，一般是xxx_create函数返回的实例

	//处理消息的回调函数，由皮囊逻辑里注册
	//typedef int (*skynet_cb)(struct skynet_context * context, void *ud, int type, int session, uint32_t source , const void * msg, size_t sz);

	skynet_cb cb;		//回调函数

	//actor的信箱，存放受到的消息
	struct message_queue *queue;	//一级消息队列

	FILE * logfile;		//日志文件句柄

	//记录调用回调函数处理消息所用时间
	uint64_t cpu_cost;	// in microsec
	uint64_t cpu_start;	// in microsec

	//handler的16进制字符，便于传递
	char result[32];	//保存命令执行返回结果

	//handle是一个uint32_t的整数，高八位表示远程节点(这是框架自带的集群设施，后面的分析都会无视该部分
	
	uint32_t handle;	//服务句柄,实质就是该结构体指针存放在一个全局的指针数组中的一个编号

	//上一次分配的session,用于分配不重复的session
	int session_id;		//会话id

	//引用计数
	int ref;			//线程安全的引用计数，保证在使用的时候，没有被其他线程释放
	int message_count;	//个数

	bool init;	//是否初始化

	//是否在处理消息时死循环
	bool endless;	//是否无限循环

	bool profile;

	CHECKCALLING_DECL
};


//skynet 的节点结构
struct skynet_node {

	int total;	//一个skynet_node的服务数 一个node的服务数量

	int init;

	uint32_t monitor_exit;

	pthread_key_t handle_key;
	bool profile;	// default is off
};

//全局变量
static struct skynet_node G_NODE;


//获取该skynet_node的 total
int 
skynet_context_total() {
	return G_NODE.total;
}


//增加skynet_node的 total
static void
context_inc() {
	ATOM_INC(&G_NODE.total);
}


//减少skynet_node的 total
static void
context_dec() {
	ATOM_DEC(&G_NODE.total);
}


//获取线程全局变量
uint32_t 
skynet_current_handle(void) {
	if (G_NODE.init) {
		void * handle = pthread_getspecific(G_NODE.handle_key);
		return (uint32_t)(uintptr_t)handle;
	} else {
		uint32_t v = (uint32_t)(-THREAD_MAIN);
		return v;
	}
}


//将id转换成16进制的str
static void
id_to_hex(char * str, uint32_t id) {
	int i;
	static char hex[16] = { '0','1','2','3','4','5','6','7','8','9','A','B','C','D','E','F' };
	str[0] = ':';
	for (i=0;i<8;i++) {

		//转换 16进制的0xff ff ff ff 8位
		str[i+1] = hex[(id >> ((7-i) * 4))&0xf];//依次取4位，从最高的4位开始取
		
	}
	str[9] = '\0';
}

struct drop_t {
	uint32_t handle;
};


//销毁message
static void
drop_message(struct skynet_message *msg, void *ud) {

	struct drop_t *d = ud;

	skynet_free(msg->data);
	
	uint32_t source = d->handle;
	assert(source);
	
	// report error to the message source
	//发送消息
	skynet_send(NULL, source, msg->source, PTYPE_ERROR, 0, NULL, 0);
}

//创建新的ctx
/*
1.加载skynet_module对象，调用create取得用户对象
2.分配sc,注册handle,分配信箱
3.调用init初始化用户对象

之所以到处有一些CALLINGCHECK宏，主要是为了检测调度是否正确，因为skynet调度时，每个actor只会被
一个线程持有调度，也就是消息处理时单线程的

*/
struct skynet_context * 
skynet_context_new(const char * name, const char *param) {
	//加载boostrap模块时，参数是 snlua  boostrap
	
	//根据名称从全局的modules 中查找模块(动态库)，如果没有找到，该函数中会则加载该模块 模块对应一个.so库，即动态库,会初始化
	//skynet_module中的函数指针
	//模块的路径 optstring("cpath","./cservice/?.so");
	struct skynet_module * mod = skynet_module_query(name);

	if (mod == NULL)
		return NULL;

	//调用模块创建函数
	//如果m->create函数指针不为空则调用m->create,该create函数也是在动态库中定义的,inst为调用的返回值
	//如果m->create函数指针为空，则返回 return (void *)(intptr_t)(~0);

	//实质就是调用动态库中的xxxx_create函数
	
	void *inst = skynet_module_instance_create(mod);
	
	if (inst == NULL)
		return NULL;

	//分配内存,一个模块(动态库的抽象)属于一个skynet_context
	struct skynet_context * ctx = skynet_malloc(sizeof(*ctx));
	//#define CHECKCALLING_INIT(ctx) spinlock_init(&ctx->calling);
	CHECKCALLING_INIT(ctx)

	//skynet_context
	//初始化模块指针，即获得到的动态库结构体的地址，该结构体中有动态库中的函数指针
	ctx->mod = mod;

	//inst是调用模块中的xxxx_create函数的返回值，是一个结构体指针
	ctx->instance = inst;

   //初始化skynet_context的引用计数为2
	ctx->ref = 2;

	//设置回调函数指针为空
	ctx->cb = NULL;
	ctx->cb_ud = NULL;

	
	ctx->session_id = 0;
	
	ctx->logfile = NULL;

	//表示未初始化
	ctx->init = false;
	ctx->endless = false;

	ctx->cpu_cost = 0;
	ctx->cpu_start = 0;

	//消息队列中的消息数量
	ctx->message_count = 0;
	ctx->profile = G_NODE.profile;
	
	// Should set to 0 first to avoid skynet_handle_retireall get an uninitialized handle
	ctx->handle = 0;	


	//注册，得到一个唯一的句柄
	//实质就是有将 创建的ctx指针 保存到一个全局的 指针数组中，是利用hash存储的，
	//只不过该数组封装成结构体  handler_storage
	//handle就是 根据存放位置，以及一些特性计算出来的一个值，相当于一个标号
	ctx->handle = skynet_handle_register(ctx);


	//为一级消息队列 分配内存
	struct message_queue * queue = ctx->queue = skynet_mq_create(ctx->handle);

	//节点服务数加1
	// init function maybe use ctx->handle, so it must init at last
	context_inc();

	//#define CHECKCALLING_BEGIN(ctx) if (!(spinlock_trylock(&ctx->calling))) { assert(0); }
	CHECKCALLING_BEGIN(ctx)

	//调用mod的初始化
	//该函数内部 return m->init(inst, ctx, parm);
	//主要是会设置ctx的回调函数指针						boostrap


	//调用动态库的xxxx_init函数
	//会初始化skynet_context的回调函数
	int r = skynet_module_instance_init(mod, inst, ctx, param);
	
	CHECKCALLING_END(ctx)
		
	if (r == 0) {

		//将引用计数减一,减为0时则删除skynet_context,若销毁，返回NULL
		struct skynet_context * ret = skynet_context_release(ctx);
		if (ret) {
			ctx->init = true;
		}
		/*
			ctx的初始化流程是可以发送消息出去的(同时也可以接受消息)，但在初始化流程完成之前
			接受到的消息都必须缓存在mq中，不能处理。我用了一个小技巧解决这个问题，就是在初始化
			流程开始前，假装只在globalmq中(这是有mq中的一个标记位决定的)。这样，向他发送消息，
			并不会把mq压入globalmq,自然也不会被工作线程取到。等初始化流程结束，在强制把mq压入
			globalmq(无论是否为空),即使失败也要进行这个操作
		*/

		//初始化流程结构后将这个ctx对应的mq强制压入 globalmq

		//即将 一级消息队列加入到 全局的二级消息队列(消息队列中存放消息队列)中，
		skynet_globalmq_push(queue);
		
		if (ret) {
			skynet_error(ret, "LAUNCH %s %s", name, param ? param : "");
		}
		return ret;
	} else {
		skynet_error(ctx, "FAILED launch %s", name);
		uint32_t handle = ctx->handle;


		//将引用计数减一,减为0时则删除skynet_context
		skynet_context_release(ctx);

		//根据标号handle删除指针
		skynet_handle_retire(handle);

		struct drop_t d = { handle };
		//销毁queue
		skynet_mq_release(queue, drop_message, &d);
		return NULL;
	}
}

//根据skynet_context分配一个sesssion id
int
skynet_context_newsession(struct skynet_context *ctx) {
	// session always be a positive number
	int session = ++ctx->session_id;
	if (session <= 0) {
		ctx->session_id = 1;
		return 1;
	}
	return session;
}


//skynet_context引用计数加1
void 
skynet_context_grab(struct skynet_context *ctx) {
	ATOM_INC(&ctx->ref);
}

//节点对应的服务数减 1
void
skynet_context_reserve(struct skynet_context *ctx) {
	skynet_context_grab(ctx);
	// don't count the context reserved, because skynet abort (the worker threads terminate) only when the total context is 0 .
	// the reserved context will be release at last.

	//减少total
	context_dec();
}

/*
	问题就在这里:
	handle 和 ctx 的绑定关系是在 ctx 模块外部操作的（不然也做不到 ctx 的正确销毁），

	无法确保从 handle 确认对应的 ctx 无效的同时，ctx 真的已经被销毁了。
	所以，当工作线程判定 mq 可以销毁时（对应的 handle 无效），ctx 可能还活着（另一个工作线程还持有其引用），
	持有这个 ctx 的工作线程可能正在它生命的最后一刻，向其发送消息。结果 mq 已经销毁了。

	当 ctx 销毁前，由它向其 mq 设入一个清理标记。然后在 globalmq 取出 mq ，发现已经找不到 handle 对应的 ctx 时，
	先判断是否有清理标记。如果没有，再将 mq 重放进 globalmq ，直到清理标记有效，在销毁 mq 。
*/

//销毁skynet_context
static void 
delete_context(struct skynet_context *ctx) {
	//关闭日志文件句柄
	if (ctx->logfile) {
		fclose(ctx->logfile);
	}

	//调用对应动态库的xxxx_release函数
	skynet_module_instance_release(ctx->mod, ctx->instance);
	
	//设置标记位,标记该循环队列要删除，并且把它压入 global mq
	skynet_mq_mark_release(ctx->queue);
	
	CHECKCALLING_DESTROY(ctx)

	skynet_free(ctx);

	//这个节点对应的服务数也 减 1
	context_dec();
}

//将引用计数减一 , 减为0时则删除skynet_context
struct skynet_context * 
skynet_context_release(struct skynet_context *ctx) {
	//引用计数减 1，减为0时则删除skynet_context
	if (ATOM_DEC(&ctx->ref) == 0) {
		
		delete_context(ctx);
		return NULL;
	}
	return ctx;
}

//在handle标识的skynet_context的消息队列中插入一条消息
//handle就是一个标号，所有的skynet_context的指针都保存到一个数组中,通过handle可以获得对应的指针
int
skynet_context_push(uint32_t handle, struct skynet_message *message) {

	//通过handle获取skynet_context,skynet_context的引用计数加1
	
	struct skynet_context * ctx = skynet_handle_grab(handle);
	
	if (ctx == NULL) {
		return -1;
	}

	//将message加入到cte的queue中
	skynet_mq_push(ctx->queue, message);

	//引用计数减一,减为0时则删除skynet_context
	skynet_context_release(ctx);

	return 0;
}


//设置ctx->endless = true;
void 
skynet_context_endless(uint32_t handle) {

	//根据handle标号获得skynet_cotext指针
	struct skynet_context * ctx = skynet_handle_grab(handle);
	if (ctx == NULL) {
		return;
	}
	
	ctx->endless = true;

	skynet_context_release(ctx);
}


//判断是否是远程消息
int 
skynet_isremote(struct skynet_context * ctx, uint32_t handle, int * harbor) {

	//判断是否是远程消息
	int ret = skynet_harbor_message_isremote(handle);
	
	if (harbor) {
		//返回harbor(注:高八位存的是harbor)
		*harbor = (int)(handle >> HANDLE_REMOTE_SHIFT);
	}
	return ret;
}


//如果skynet_context中的回调函数指针不是空 ,调用此函数,消息分发
static void
dispatch_message(struct skynet_context *ctx, struct skynet_message *msg) {

	assert(ctx->init);
	CHECKCALLING_BEGIN(ctx)

	pthread_setspecific(G_NODE.handle_key, (void *)(uintptr_t)(ctx->handle));

	//type是请求类型
	int type = msg->sz >> MESSAGE_TYPE_SHIFT;	//高8位消息类别
	size_t sz = msg->sz & MESSAGE_TYPE_MASK;	//低24位消息大小
	
	//打印日志
	if (ctx->logfile) {
		skynet_log_output(ctx->logfile, msg->source, type, msg->session, msg->data, sz);
	}

	
	++ctx->message_count;
	
	int reserve_msg;

	if (ctx->profile) {
		ctx->cpu_start = skynet_thread_time();
		
		//调用回调函数,根据返回值决定是否要释放消息
		reserve_msg = ctx->cb(ctx, ctx->cb_ud, type, msg->session, msg->source, msg->data, sz);

		uint64_t cost_time = skynet_thread_time() - ctx->cpu_start;
		ctx->cpu_cost += cost_time;
	} 
	else 
	{
		//调用回调函数,根据返回值决定是否要释放消息
		reserve_msg = ctx->cb(ctx, ctx->cb_ud, type, msg->session, msg->source, msg->data, sz);
	}
	if (!reserve_msg) {
		skynet_free(msg->data);
	}

	CHECKCALLING_END(ctx)
}

//对skynet_context的消息队列中的 所有的msg调用dispatch_message
//处理ctx的循环消息队列中的所有消息,调用 回调函数
void 
skynet_context_dispatchall(struct skynet_context * ctx) {
	// for skynet_error
	struct skynet_message msg;
	struct message_queue *q = ctx->queue;
	
	while (!skynet_mq_pop(q,&msg)) {
		dispatch_message(ctx, &msg);
	}
}


//message_queue的调度,在工作线程中被调用
//函数的做用是；调度传入的2级队列，并返回下一个可调度的2级队列
struct message_queue * 
skynet_context_message_dispatch(struct skynet_monitor *sm, struct message_queue *q, int weight) {
	//在工作线程中，传入的q==NULL
	if (q == NULL) {
		//从全局的二级消息队列的头部 弹出一个消息队列
		q = skynet_globalmq_pop();
		if (q==NULL)
			return NULL;
	}

	//得到消息队列所属的服务句柄,

	//实质就是获得消息队列所属的skynet_context的标号
	uint32_t handle = skynet_mq_handle(q);

	//根据handle获取skynet_conext
	struct skynet_context * ctx = skynet_handle_grab(handle);
	
	if (ctx == NULL) {
		struct drop_t d = { handle };
		skynet_mq_release(q, drop_message, &d);
		return skynet_globalmq_pop();
	}

	int i,n=1;
	struct skynet_message msg;

	for (i=0;i<n;i++) {

		//当2级队列为空时并没有将其压入1级队列，那它从此消失了吗，不是，这样做是为了减少空转1级队列
		//那这个二级队列是是么时候压回的呢，在message_queue中，有一个in_global标记是否在1级队列中
		//当2级队列的出队(skynet_mq_pop)失败时，这个标记位0，在二级队列入队时(skynet_mq_push)
		//会判断这个标记，如果为0,就会将自己压入1级队列。(skynet_mq_mark_realeas也会判断)
		//所以这个2级队列在下次入队时会 压回

		//从队列中取出一条消息,msg为传出参数
		if (skynet_mq_pop(q,&msg)) {
			
			skynet_context_release(ctx);
			return skynet_globalmq_pop();
			
		} else if (i==0 && weight >= 0) {

			//修改了n的大小,即修改了for循环的次数，也就是每次调度处理多少条消息。这个此时与传入的weight
			//有关。
			n = skynet_mq_length(q);

			//大概就是，把工作线程分组，前四组每组8个，超过的归入第五组。�
			//A,E组每次调度处理一条消息，B组每次处理n/2条，C组每次处理n/4条，
			//D组每次处理n/8条。是为了均匀使用多核
			n >>= weight;
		}

		//做了一个负载判断。负载的阈值是1024。不过也只是仅仅输出一条log提醒而已
		int overload = skynet_mq_overload(q);
		if (overload) {
			skynet_error(ctx, "May overload, message queue length = %d", overload);
		}

		//触发了一下monitor,这个监控是用来检测消息处理是否发生了死循环，不过也只是输出一条lig提醒一下
		//这个检测是放在一个专门的监控线程里做的，判断死循环的时间是5秒
		/*
		void 
		skynet_monitor_trigger(struct skynet_monitor *sm, uint32_t source, uint32_t destination) {
			sm->source = source;
			sm->destination = destination;
			ATOM_INC(&sm->version);
		}

		*/

		//sm是传入，被修改,在monitrr线程中会检测sm的值
		skynet_monitor_trigger(sm, msg.source , handle);

		//如果skynet_context的回调函数的函数指针为空
		if (ctx->cb == NULL) 
		{
			skynet_free(msg.data);
		}
		else 
		{
			/*
					struct skynet_message {
						uint32_t source;	//消息源所属的skynet的标号
						int session;		//用来做上下文的标识
						void * data;		//消息指针
						size_t sz;			//消息长度,消息的请求类型定义在高八位
						};

			*/
		
			//消息分发	实质就是调用skynet_context中的回调函数处理消息
			dispatch_message(ctx, &msg);
		}

		
		//触发了一下monitor,这个监控是用来检测消息处理是否发生了死循环，不过也只是输出一条lig提醒一下
		//这个检测是放在一个专门的监控线程里做的，判断死循环的时间是5秒
		skynet_monitor_trigger(sm, 0,0);
	}

	assert(q == ctx->queue);

	//从全局二级消息队列中移除一个消息队列，从头部移除
	struct message_queue *nq = skynet_globalmq_pop();
	if (nq) {
		// If global mq is not empty , push q back, and return next queue (nq)
		// Else (global mq is empty or block, don't push q back, and return q again (for next dispatch)

		//将处理的消息的消息队列q重新加入到全局的消息队列中
		skynet_globalmq_push(q);

		q = nq;
	} 
	skynet_context_release(ctx);

	return q;
}

//将addr指向的数据复制到name中
static void
copy_name(char name[GLOBALNAME_LENGTH], const char * addr) {
	int i;
	for (i=0;i<GLOBALNAME_LENGTH && addr[i];i++) {
		name[i] = addr[i];
	}
	for (;i<GLOBALNAME_LENGTH;i++) {
		name[i] = '\0';
	}
}


//根据字符串查找handle标号，name可以是handle对应的名字，也可以使 :xxxxx
uint32_t 
skynet_queryname(struct skynet_context * context, const char * name) {
	switch(name[0]) {
	case ':':
		//strtoul  将字符串类型转换为unsigned long类型
		return strtoul(name+1,NULL,16);
	case '.':
		
		//根据名称查找handle
		return skynet_handle_findname(name + 1);
	}
	skynet_error(context, "Don't support query global name %s",name);
	return 0;
}

//将handle对应的skynet_context从handle_storage中删除
static void
handle_exit(struct skynet_context * context, uint32_t handle) {
	if (handle == 0) {
		handle = context->handle;
		skynet_error(context, "KILL self");
	} else {
		skynet_error(context, "KILL :%0x", handle);
	}
	if (G_NODE.monitor_exit) {
		skynet_send(context,  handle, G_NODE.monitor_exit, PTYPE_CLIENT, 0, NULL, 0);
	}
	
	//收回handle,即销毁handle对应的skynet_context在数组中的指针
	skynet_handle_retire(handle);
}

// skynet command

//命令名称与回调函数对应的结构体
struct command_func {
	const char *name;
	const char * (*func)(struct skynet_context * context, const char * param);
};

//"timeout"命令对应的回调函数
//注意是调用 skynet_timeout(),插入定时器
static const char *
cmd_timeout(struct skynet_context * context, const char * param) {
	char * session_ptr = NULL;

	//long int strtol (const char* str, char** endptr, int base);
	//base是进制  param是需要转换的字符串  session_ptr==NULL时，该参数无效,

	//通过字符串获得数字
	//将param转为10进制数
	int ti = strtol(param, &session_ptr, 10);

		//获得一个session   会话id
	int session = skynet_context_newsession(context);
	
	//插入定时器，time的单位是0.01秒，如time=300,表示3秒
	skynet_timeout(context->handle, ti, session);

	sprintf(context->result, "%d", session);
	return context->result;
}

//"reg"对应回调函数 ,为context命名
static const char *
cmd_reg(struct skynet_context * context, const char * param) {

	if (param == NULL || param[0] == '\0') 
	{
		sprintf(context->result, ":%x", context->handle);
		return context->result;
	}
		//命名是，格式是        .名字
	else if (param[0] == '.') 
	{
		//实质就是将handle,name插入到handle_storage的name数组中
		//为skynet_context通过一个名称
		return skynet_handle_namehandle(context->handle, param + 1);
	}
	else 
	{
		skynet_error(context, "Can't register global name %s in C", param);
		return NULL;
	}
}

//"query"对应的回调函数
//根据名字查找context对应的handle标号
static const char *
cmd_query(struct skynet_context * context, const char * param) {
	//名字的格式  .名字
	if (param[0] == '.') {
		
		uint32_t handle = skynet_handle_findname(param+1);

		//将handle写入在context的result中 
		if (handle) {
			sprintf(context->result, ":%x", handle);
			return context->result;
		}
	}
	return NULL;
}

//"name"对应的回调函数
//为handle对应的skynet_context命名,只是handle,name都在param中
static const char *
cmd_name(struct skynet_context * context, const char * param) {
	int size = strlen(param);
	char name[size+1];
	char handle[size+1];
	
	sscanf(param,"%s %s",name,handle);

	if (handle[0] != ':') {
		return NULL;
	}
	//得到handle
	uint32_t handle_id = strtoul(handle+1, NULL, 16);
	if (handle_id == 0) {
		return NULL;
	}
	
	if (name[0] == '.') {
		//实质就是将handle,name插入到handle_storage的name数组中
		//为skynet_context通过一个名称
		return skynet_handle_namehandle(handle_id, name + 1);
	} else {
		skynet_error(context, "Can't set global name %s in C", name);
	}
	return NULL;
}


//"exit"对应的回调函数
//调用 handle_exit()
//将context退出
static const char *
cmd_exit(struct skynet_context * context, const char * param) {
	handle_exit(context, 0);
	return NULL;
}

//根据参数获得handle
static uint32_t
tohandle(struct skynet_context * context, const char * param) {
	uint32_t handle = 0;
	//如果是 :xxxxx
	if (param[0] == ':') {
		
		handle = strtoul(param+1, NULL, 16);
	} else if (param[0] == '.') {
		//如果是 .名字
		handle = skynet_handle_findname(param+1);
	} else {
		skynet_error(context, "Can't convert %s to handle",param);
	}

	return handle;
}


//"kill"对应的回调函数
//将param对应的handle退出
static const char *
cmd_kill(struct skynet_context * context, const char * param) {
	uint32_t handle = tohandle(context, param);
	if (handle) {
		handle_exit(context, handle);
	}
	return NULL;
}

//"launch"对应的回调函数,加载模块(动态库),
//创建一个skynet_context
static const char *
cmd_launch(struct skynet_context * context, const char * param) {
	size_t sz = strlen(param);
	char tmp[sz+1];
	strcpy(tmp,param);
	char * args = tmp;

	// aaa bbb ccc

	//分割字符串
	char * mod = strsep(&args, " \t\r\n");

	//分割字符串
	args = strsep(&args, "\r\n");
													//模块名 参数
	struct skynet_context * inst = skynet_context_new(mod,args);
	
	if (inst == NULL) {
		return NULL;
	} else {
	//将handle写入到result中
		id_to_hex(context->result, inst->handle);
		return context->result;
	}
}

//"getenv"对应的回调函数
//获取lua全局变量   
static const char *
cmd_getenv(struct skynet_context * context, const char * param) {
	return skynet_getenv(param);
}

//"setenv"对应的回调函数
//设置lua全局变量   变量名 值
static const char *
cmd_setenv(struct skynet_context * context, const char * param) {
	size_t sz = strlen(param);
	//保存的是变量名 
	char key[sz+1];
	int i;


	//获得变量名 
	for (i=0;param[i] != ' ' && param[i];i++) {
		key[i] = param[i];
	}
	if (param[i] == '\0')
		return NULL;

	key[i] = '\0';
	param += i+1;

	//设置
	skynet_setenv(key,param);
	return NULL;
}

//"starttime"对应的回调函数 
//调用skynet_starttime() 获取程序启动时间
static const char *
cmd_starttime(struct skynet_context * context, const char * param) {
	//返回启动该程序的时间
	uint32_t sec = skynet_starttime();

	//将时间写入到result中
	sprintf(context->result,"%u",sec);
	return context->result;
}

//"abort"对应的回调函数

//skynet_handle_retireall()
//回收所有的handler
//回收handle_storage中的指针数组中的所有skynet_context指针

static const char *
cmd_abort(struct skynet_context * context, const char * param) {
	skynet_handle_retireall();
	return NULL;
}

//"monitor"对应的回调函数
static const char *
cmd_monitor(struct skynet_context * context, const char * param) {
	uint32_t handle=0;
	if (param == NULL || param[0] == '\0') {
		if (G_NODE.monitor_exit) {
			// return current monitor serivce
			sprintf(context->result, ":%x", G_NODE.monitor_exit);
			return context->result;
		}
		return NULL;
	} else {
		handle = tohandle(context, param);
	}
	G_NODE.monitor_exit = handle;
	return NULL;
}

//"stat"对应的回调函数
//获取skynet_context中的一些状态信息
static const char *
cmd_stat(struct skynet_context * context, const char * param) {

	if (strcmp(param, "mqlen") == 0) {
		
		//获取skynet_context中的消息队列中的消息个数
		int len = skynet_mq_length(context->queue);
		//将结果写入到result中
		sprintf(context->result, "%d", len);

	} else if (strcmp(param, "endless") == 0) {
		if (context->endless) {
			strcpy(context->result, "1");
			context->endless = false;
		} else {
			strcpy(context->result, "0");
		}
	} else if (strcmp(param, "cpu") == 0) {
		double t = (double)context->cpu_cost / 1000000.0;	// microsec
		sprintf(context->result, "%lf", t);
	} else if (strcmp(param, "time") == 0) {
		if (context->profile) {
			uint64_t ti = skynet_thread_time() - context->cpu_start;
			double t = (double)ti / 1000000.0;	// microsec
			sprintf(context->result, "%lf", t);
		} else {
			strcpy(context->result, "0");
		}
	} else if (strcmp(param, "message") == 0) {
		sprintf(context->result, "%d", context->message_count);
	} else {
		context->result[0] = '\0';
	}
	return context->result;
}

//"logon"对应的回调函数
//作用是打开param中指定的handle对应的skynet_context的日志文件
static const char *
cmd_logon(struct skynet_context * context, const char * param) {

	//通过param获得handle
	uint32_t handle = tohandle(context, param);

	if (handle == 0)
		return NULL;

	//根据标号handle获得对应的skynet_context指针
	struct skynet_context * ctx = skynet_handle_grab(handle);
	if (ctx == NULL)
		return NULL;
	
	FILE *f = NULL;
	FILE * lastf = ctx->logfile;
	if (lastf == NULL) {
		//打开日志文件
		f = skynet_log_open(context, handle);
		if (f) {
			if (!ATOM_CAS_POINTER(&ctx->logfile, NULL, f)) {
				// logfile opens in other thread, close this one.
				fclose(f);
			}
		}
	}
	skynet_context_release(ctx);
	return NULL;
}

//"logoff"对应的回调函数
//作用是关闭param中的handle对应的 skynet_context对应的日志文件

static const char *
cmd_logoff(struct skynet_context * context, const char * param) {

	//通过param获得handle
	uint32_t handle = tohandle(context, param);
	if (handle == 0)
		return NULL;

	//获得handle对应的skynet_context
	struct skynet_context * ctx = skynet_handle_grab(handle);
	if (ctx == NULL)
		return NULL;
	FILE * f = ctx->logfile;
	if (f) {
		// logfile may close in other thread
		if (ATOM_CAS_POINTER(&ctx->logfile, f, NULL)) {
			skynet_log_close(context, f, handle);
		}
	}
	skynet_context_release(ctx);
	return NULL;
}
//"SIGNAL" 命令对应的回调函数
//作用是调用 param的handle对应的 skynet_context对应的动态库中的xxxx_signal()函数
static const char *
cmd_signal(struct skynet_context * context, const char * param) {

	//获得handle
	uint32_t handle = tohandle(context, param);
	
	if (handle == 0)
		return NULL;
	//根据标号获得对应的skynet_context
	struct skynet_context * ctx = skynet_handle_grab(handle);
	
	if (ctx == NULL)
		return NULL;
	
	param = strchr(param, ' ');
	int sig = 0;

	if (param) {
		sig = strtol(param, NULL, 0);
	}
	// NOTICE: the signal function should be thread safe.

	//调用skynet_context对应的动态库中的xxxx_signal()函数
	skynet_module_instance_signal(ctx->mod, ctx->instance, sig);

	skynet_context_release(ctx);
	return NULL;
}

//命令名称与函数指针对应的结构体数组
static struct command_func cmd_funcs[] = {
	{ "TIMEOUT", cmd_timeout },
	{ "REG", cmd_reg },
	{ "QUERY", cmd_query },
	{ "NAME", cmd_name },
	{ "EXIT", cmd_exit },
	{ "KILL", cmd_kill },
	{ "LAUNCH", cmd_launch },
	{ "GETENV", cmd_getenv },
	{ "SETENV", cmd_setenv },
	{ "STARTTIME", cmd_starttime },
	{ "ABORT", cmd_abort },
	{ "MONITOR", cmd_monitor },
	{ "STAT", cmd_stat },
	{ "LOGON", cmd_logon },
	{ "LOGOFF", cmd_logoff },
	{ "SIGNAL", cmd_signal },
	{ NULL, NULL },
};

//根据命令名称查找对应的回调函数，并且调用
const char * 
skynet_command(struct skynet_context * context, const char * cmd , const char * param) {
	struct command_func * method = &cmd_funcs[0];
	while(method->name) {

		//如果找到 ，执行回调函数
		if (strcmp(cmd, method->name) == 0) {
			return method->func(context, param);
		}
		++method;
	}

	return NULL;
}

//参数过滤
//根据type做了两个处理
/*
1.(type & PTYPE_TAG_DONTCOPY) == 0
会将data复制一份用作实际发送，这种情况下原来的data就要由调用者负责释放
2.(type & PTYPE_TAG_ALLOCSESSION)>0
	会从sesson计数器分配一个session

处理完后，type会合并到sz的高8位
*/
static void
_filter_args(struct skynet_context * context, int type, int *session, void ** data, size_t * sz) {

	//是否                        0x10000
	int needcopy = !(type & PTYPE_TAG_DONTCOPY);
								//0x20000
	int allocsession = type & PTYPE_TAG_ALLOCSESSION;

	type &= 0xff;

	//会从sesson计数器分配一个session
	if (allocsession) {
		assert(*session == 0);
		//得到一个新的session id,就是将原来的context对应的session+1 
		*session = skynet_context_newsession(context);
	}

	//会将data数据复制  这种情况下原来的data就要由调用者负责释放
	if (needcopy && *data) {
		char * msg = skynet_malloc(*sz+1);
		memcpy(msg, *data, *sz);
		msg[*sz] = '\0';
		*data = msg;
	}

	//处理完后，type会合并到sz的高8位   24
	*sz |= (size_t)type << MESSAGE_TYPE_SHIFT;
}


/*
 * 向handle为destination的服务发送消息(注：handle为destination的服务不一定是本地的)
 * type中含有 PTYPE_TAG_ALLOCSESSION ，则session必须是0
 * type中含有 PTYPE_TAG_DONTCOPY ，则不需要拷贝数据
 */

//发送消息
int
skynet_send(struct skynet_context * context, uint32_t source, uint32_t destination , int type, int session, void * data, size_t sz) {

	//对消息长度限制 MESSAGE_TYPE_MASK就是 SIZE_MAX >> 8,最大只能为2^24,16MB
	if ((sz & MESSAGE_TYPE_MASK) != sz) {
		skynet_error(context, "The message to %x is too large", destination);
		if (type & PTYPE_TAG_DONTCOPY) {
			skynet_free(data);
		}
		return -1;
	}

	//参数过滤
	//根据type做了两个处理
	/*
		1.(type & PTYPE_TAG_DONTCOPY) == 0
			会将data复制一份用作实际发送，这种情况下原来的data就要由调用者负责释放
		2.(type & PTYPE_TAG_ALLOCSESSION)>0
		会从sesson计数器分配一个session
		
	*/
	_filter_args(context, type, &session, (void **)&data, &sz);

	if (source == 0) {
		source = context->handle;
	}

	if (destination == 0) {
		return session;
	}

	//投递到接受者的信箱,根据接受者句柄判读是否为远程节点，如果是就用harbo发送(内置的集群方案，
	//现在已经不推荐使用),成功返回session,失败返回-1,并且释放data

	// 如果消息时发给远程的
	if (skynet_harbor_message_isremote(destination)) {
		struct remote_message * rmsg = skynet_malloc(sizeof(*rmsg));
		rmsg->destination.handle = destination;
		rmsg->message = data;
		rmsg->sz = sz;
		
		skynet_harbor_send(rmsg, source, session);
	}
	// 本机消息 直接压入消息队列
	else
	{
		struct skynet_message smsg;
		smsg.source = source;
		smsg.session = session;
		smsg.data = data;
		smsg.sz = sz;

		//destination目的地的skynet_context对应的标号
		//就是将消息发送到destination对应的消息队列中
		if (skynet_context_push(destination, &smsg)) {
			skynet_free(data);
			return -1;
		}
	}
	return session;
}

//根据名字找到目的地 skynet_context，发送消息
int
skynet_sendname(struct skynet_context * context, uint32_t source, const char * addr , int type, int session, void * data, size_t sz) {
	if (source == 0) {
		source = context->handle;
	}
	uint32_t des = 0;

	//如果地址是 :xxxxx
	if (addr[0] == ':') 
	{
		//xxxx就是handle
		des = strtoul(addr+1, NULL, 16);
	}
	//如果地址是 .名字
	else if (addr[0] == '.') 
	{
	//根据名称找到对应的skynet_context的标号
		des = skynet_handle_findname(addr + 1);
		if (des == 0) 
		{
			if (type & PTYPE_TAG_DONTCOPY) {
				skynet_free(data);
			}
			return -1;
		}
	} 
	else
	{
		_filter_args(context, type, &session, (void **)&data, &sz);

	//远程消息
		struct remote_message * rmsg = skynet_malloc(sizeof(*rmsg));
		copy_name(rmsg->destination.name, addr);
		rmsg->destination.handle = 0;
		rmsg->message = data;
		rmsg->sz = sz;

		skynet_harbor_send(rmsg, source, session);
		return session;
	}
	
	return skynet_send(context, source, des, type, session, data, sz);
}

//获取skynet_context对应的标号handle
uint32_t 
skynet_context_handle(struct skynet_context *ctx) {
	return ctx->handle;
}

//初始化struct skynet_context的回调函数指针，回调函数参数
void 
skynet_callback(struct skynet_context * context, void *ud, skynet_cb cb) {
	context->cb = cb;
	context->cb_ud = ud;
}


// 向ctx服务发送消息(注：ctx服务一定是本地的)
//实质就是将消息加入到skynet_context的消息队列中
void
skynet_context_send(struct skynet_context * ctx, void * msg, size_t sz, uint32_t source, int type, int session) {
	struct skynet_message smsg;
	smsg.source = source;
	smsg.session = session;
	smsg.data = msg;
	smsg.sz = sz | (size_t)type << MESSAGE_TYPE_SHIFT;

	// 压入消息队列
	skynet_mq_push(ctx->queue, &smsg);
}


//在skynet_mian.c的main函数中调用
void 
skynet_globalinit(void) {

	G_NODE.total = 0;
	G_NODE.monitor_exit = 0;
	G_NODE.init = 1;

	//创建线程全局变量
	if (pthread_key_create(&G_NODE.handle_key, NULL)) {
		fprintf(stderr, "pthread_key_create failed");
		exit(1);
	}
	// set mainthread's key  	有#define THREAD_MAIN 1
	skynet_initthread(THREAD_MAIN); 
}

//销毁线程全局变量
void 
skynet_globalexit(void) {
	pthread_key_delete(G_NODE.handle_key);
}

//初始化线程,初始化线程全局变量
void
skynet_initthread(int m) {
	uintptr_t v = (uint32_t)(-m);
	pthread_setspecific(G_NODE.handle_key, (void *)v);
}

void
skynet_profile_enable(int enable) {
	G_NODE.profile = (bool)enable;
}

