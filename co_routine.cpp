/*
* Tencent is pleased to support the open source community by making Libco available.

* Copyright (C) 2014 THL A29 Limited, a Tencent company. All rights reserved.
*
* Licensed under the Apache License, Version 2.0 (the "License"); 
* you may not use this file except in compliance with the License. 
* You may obtain a copy of the License at
*
*	http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, 
* software distributed under the License is distributed on an "AS IS" BASIS, 
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. 
* See the License for the specific language governing permissions and 
* limitations under the License.
*/

#include "co_routine.h"
#include "co_routine_inner.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <string>
#include <map>

#include <poll.h>
#include <sys/epoll.h>
#include <sys/time.h>
#include <errno.h>

#include <assert.h>

#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/syscall.h>
#include <unistd.h>

extern "C"
{
	extern void coctx_swap( coctx_t *,coctx_t* ) asm("coctx_swap");
};
using namespace std;
stCoRoutine_t *GetCurrCo( stCoRoutineEnv_t *env );
struct stCoEpoll_t;

/*
一个进程或线程stCoRoutineEnv_t 
协程运行环境 
coroutine依赖线程，来实现获取调度
*/

struct stCoRoutineEnv_t
{
	stCoRoutine_t *pCallStack[ 128 ];  /* coroutine对应的task */
	int iCallStackSize;
	stCoEpoll_t *pEpoll;  /* 网络IO信息  需要一个epoll获取事件通知 */ 
};
//int socket(int domain, int type, int protocol);
void co_log_err( const char *fmt,... )
{
}

#if defined( __LIBCO_RDTSCP__) 
static unsigned long long counter(void)
{
    register uint32_t lo, hi;
    register unsigned long long o;
    __asm__ __volatile__ (
        "rdtscp" : "=a"(lo), "=d"(hi)
    );
    o = hi;
    o <<= 32;
    return (o | lo);
}

static unsigned long long getCpuKhz()
{
    FILE *fp = fopen("/proc/cpuinfo","r");
    if(!fp) return 1;
    char buf[4096] = {0};
    fread(buf,1,sizeof(buf),fp);
    fclose(fp);

    char *lp = strstr(buf,"cpu MHz");
    if(!lp) return 1;
    lp += strlen("cpu MHz");
    while(*lp == ' ' || *lp == '\t' || *lp == ':')
    {
        ++lp;
    }
	
    double mhz = atof(lp);
    unsigned long long u = (unsigned long long)(mhz * 1000);
    return u;
}
#endif

static unsigned long long GetTickMS()
{
#if defined( __LIBCO_RDTSCP__) 
    static uint32_t khz = getCpuKhz();
    return counter() / khz;
#else
    struct timeval now = { 0 };
    gettimeofday( &now,NULL );
    unsigned long long u = now.tv_sec;
    u *= 1000;
    u += now.tv_usec / 1000;
    return u;
#endif
}

static pid_t GetPid()
{
    static __thread pid_t pid = 0;
    static __thread pid_t tid = 0;
    if( !pid || !tid || pid != getpid() )
    {
        pid = getpid();
        tid = syscall( __NR_gettid );
    }
    return tid;

}
/*
static pid_t GetPid()
{
	char **p = (char**)pthread_self();
	return p ? *(pid_t*)(p + 18) : getpid();
}
*/
template <class T,class TLink>
void RemoveFromLink(T *ap)
{
	TLink *lst = ap->pLink;
	if(!lst) return ;
	assert( lst->head && lst->tail );

	if( ap == lst->head )
	{
		lst->head = ap->pNext;
		if(lst->head)
		{
			lst->head->pPrev = NULL;
		}
	}
	else
	{
		if(ap->pPrev)
		{
			ap->pPrev->pNext = ap->pNext;
		}
	}

	if( ap == lst->tail )
	{
		lst->tail = ap->pPrev;
		if(lst->tail)
		{
			lst->tail->pNext = NULL;
		}
	}
	else
	{
		ap->pNext->pPrev = ap->pPrev;
	}

	ap->pPrev = ap->pNext = NULL;
	ap->pLink = NULL;
}

template <class TNode,class TLink>
void inline AddTail(TLink*apLink,TNode *ap)
{
	if( ap->pLink )
	{
		return ;
	}
	if(apLink->tail)
	{
		apLink->tail->pNext = (TNode*)ap;
		ap->pNext = NULL;
		ap->pPrev = apLink->tail;
		apLink->tail = ap;
	}
	else
	{
		apLink->head = apLink->tail = ap;
		ap->pNext = ap->pPrev = NULL;
	}
	ap->pLink = apLink;
}
template <class TNode,class TLink>
void inline PopHead( TLink*apLink )
{
	if( !apLink->head ) 
	{
		return ;
	}
	TNode *lp = apLink->head;
	if( apLink->head == apLink->tail )
	{
		apLink->head = apLink->tail = NULL;
	}
	else
	{
		apLink->head = apLink->head->pNext;
	}

	lp->pPrev = lp->pNext = NULL;
	lp->pLink = NULL;

	if( apLink->head )
	{
		apLink->head->pPrev = NULL;
	}
}

template <class TNode,class TLink>
void inline Join( TLink*apLink,TLink *apOther )
{
	//printf("apOther %p\n",apOther);
	if( !apOther->head )
	{
		return ;
	}
	TNode *lp = apOther->head;
	while( lp )
	{
		lp->pLink = apLink;
		lp = lp->pNext;
	}
	lp = apOther->head;
	if(apLink->tail)
	{
		apLink->tail->pNext = (TNode*)lp;
		lp->pPrev = apLink->tail;
		apLink->tail = apOther->tail;
	}
	else
	{
		apLink->head = apOther->head;
		apLink->tail = apOther->tail;
	}

	apOther->head = apOther->tail = NULL;
}


// ----------------------------------------------------------------------------
struct stTimeoutItemLink_t;
struct stTimeoutItem_t;

/*
epoll事件驱动 
每线程一个
*/
struct stCoEpoll_t
{
	int iEpollFd;
	static const int _EPOLL_SIZE = 1024 * 10;

	struct stTimeout_t *pTimeout;
	// 超时coroutine 
	struct stTimeoutItemLink_t *pstTimeoutList;
	// 活跃coroutine
	struct stTimeoutItemLink_t *pstActiveList; 

};

//
typedef void (*OnPreparePfn_t)( stTimeoutItem_t *,struct epoll_event &ev, stTimeoutItemLink_t *active );

// 
typedef void (*OnProcessPfn_t)( stTimeoutItem_t *);
/*
一次超时处理处理
*/
struct stTimeoutItem_t
{

	enum
	{
		eMaxTimeout = 20 * 1000 //20s
	};
	stTimeoutItem_t *pPrev;
	stTimeoutItem_t *pNext;
	stTimeoutItemLink_t *pLink;

	unsigned long long ullExpireTime; /* 超时时间 */

	OnPreparePfn_t pfnPrepare; /* 预处理 OnPollPreparePfn */
	OnProcessPfn_t pfnProcess; /* 事件处理函数处理 */

	void *pArg; // routine 
	bool bTimeout; /* 是否超时，true表示已超时 */ 
};
struct stTimeoutItemLink_t
{
	stTimeoutItem_t *head;
	stTimeoutItem_t *tail;

};
struct stTimeout_t
{
	stTimeoutItemLink_t *pItems;
	int iItemSize;

	unsigned long long ullStart;
	long long llStartIdx;
};
stTimeout_t *AllocTimeout( int iSize )
{
	stTimeout_t *lp = (stTimeout_t*)calloc( 1,sizeof(stTimeout_t) );	

	lp->iItemSize = iSize;
	lp->pItems = (stTimeoutItemLink_t*)calloc( 1,sizeof(stTimeoutItemLink_t) * lp->iItemSize );

	lp->ullStart = GetTickMS();
	lp->llStartIdx = 0;

	return lp;
}
void FreeTimeout( stTimeout_t *apTimeout )
{
	free( apTimeout->pItems );
	free ( apTimeout );
}
/*
添加超时item 
*/
int AddTimeout( stTimeout_t *apTimeout,stTimeoutItem_t *apItem ,unsigned long long allNow )
{
	if( apTimeout->ullStart == 0 )
	{
		apTimeout->ullStart = allNow;
		apTimeout->llStartIdx = 0;
	}
	if( allNow < apTimeout->ullStart )
	{
		co_log_err("CO_ERR: AddTimeout line %d allNow %llu apTimeout->ullStart %llu",
					__LINE__,allNow,apTimeout->ullStart);

		return __LINE__;
	}
	if( apItem->ullExpireTime < allNow )
	{
		co_log_err("CO_ERR: AddTimeout line %d apItem->ullExpireTime %llu allNow %llu apTimeout->ullStart %llu",
					__LINE__,apItem->ullExpireTime,allNow,apTimeout->ullStart);

		return __LINE__;
	}
	int diff = apItem->ullExpireTime - apTimeout->ullStart;

	if( diff >= apTimeout->iItemSize )
	{
		co_log_err("CO_ERR: AddTimeout line %d diff %d",
					__LINE__,diff);

		return __LINE__;
	}
	AddTail( apTimeout->pItems + ( apTimeout->llStartIdx + diff ) % apTimeout->iItemSize , apItem );

	return 0;
}
inline void TakeAllTimeout( stTimeout_t *apTimeout,unsigned long long allNow,stTimeoutItemLink_t *apResult )
{
	if( apTimeout->ullStart == 0 )
	{
		apTimeout->ullStart = allNow;
		apTimeout->llStartIdx = 0;
	}

	if( allNow < apTimeout->ullStart )
	{
		return ;
	}
	int cnt = allNow - apTimeout->ullStart + 1;
	if( cnt > apTimeout->iItemSize )
	{
		cnt = apTimeout->iItemSize;
	}
	if( cnt < 0 )
	{
		return;
	}
	for( int i = 0;i<cnt;i++)
	{
		int idx = ( apTimeout->llStartIdx + i) % apTimeout->iItemSize;
		Join<stTimeoutItem_t,stTimeoutItemLink_t>( apResult,apTimeout->pItems + idx  );
	}
	apTimeout->ullStart = allNow;
	apTimeout->llStartIdx += cnt - 1;


}
/*
调度routine后的执行函数，执行对应routine的回调
再一次进行封装调用
*/
static int CoRoutineFunc( stCoRoutine_t *co,void * )
{
	if( co->pfn )
	{	
		/* routine执行函数 */
		co->pfn( co->arg );
	}
	/* 执行完成 */
	co->cEnd = 1;

	stCoRoutineEnv_t *env = co->env;
/* 主动执行切换 */
	co_yield_env( env );

	return 0;
}



struct stCoRoutine_t *co_create_env( stCoRoutineEnv_t * env,pfn_co_routine_t pfn,void *arg )
{
	stCoRoutine_t *lp = (stCoRoutine_t*)malloc( sizeof(stCoRoutine_t) );

	memset( lp,0,(long)((stCoRoutine_t*)0)->sRunStack );

	lp->env = env;
	lp->pfn = pfn;
	lp->arg = arg;

	lp->ctx.ss_sp = lp->sRunStack ;
	lp->ctx.ss_size = sizeof(lp->sRunStack) ;

	return lp;
}
/*
创建一个CoRoutine
stCoRoutine_t **ppco       		返回创建coroutine的指针的地址
const stCoRoutineAttr_t *attr      coroutine属性
pfn_co_routine_t pfn    			回调处理指针 
void *arg              			回调处理参数 
*/
int co_create( stCoRoutine_t **ppco,const stCoRoutineAttr_t *attr,pfn_co_routine_t pfn,void *arg )
{
	if( !co_get_curr_thread_env() ) 
	{
		co_init_curr_thread_env();
	}
	stCoRoutine_t *co = co_create_env( co_get_curr_thread_env(),pfn,arg );
	*ppco = co;
	return 0;
}
void co_free( stCoRoutine_t *co )
{
	free( co );
}
void co_release( stCoRoutine_t *co )
{
	if( co->cEnd )
	{
		free( co );
	}
}
/*
启动coroutine
*/
/* 
与golang还是有很大差距 没有channel来实现routine之间的通信 
这样在应用上没有golang灵活，但是可以促进开发人员匹配模型，作更加简单的设计 

没有channel怎么办?可以通过socket来实现通信  

编译上也更多  
需要部署动态lib

*/
void co_resume( stCoRoutine_t *co )
{
	stCoRoutineEnv_t *env = co->env;
	stCoRoutine_t *lpCurrRoutine = env->pCallStack[ env->iCallStackSize - 1 ];
	if( !co->cStart )
	{
		
		coctx_make( &co->ctx,(coctx_pfn_t)CoRoutineFunc,co,0 );
		co->cStart = 1;
	}
	/* 保存coroutine */
	env->pCallStack[ env->iCallStackSize++ ] = co;
	/* 交换ctx */
	coctx_swap( &(lpCurrRoutine->ctx),&(co->ctx) );


}
/*
coroutine 切换
*/
void co_yield_env( stCoRoutineEnv_t *env )
{
	
	stCoRoutine_t *last = env->pCallStack[ env->iCallStackSize - 2 ];
	stCoRoutine_t *curr = env->pCallStack[ env->iCallStackSize - 1 ];
	/* 减到0不作安全判断，在哪保证*/
	env->iCallStackSize--;

	coctx_swap( &curr->ctx, &last->ctx );
}

void co_yield_ct()
{

	co_yield_env( co_get_curr_thread_env() );
}
void co_yield( stCoRoutine_t *co )
{
	co_yield_env( co->env );
}



//int poll(struct pollfd fds[], nfds_t nfds, int timeout);
// { fd,events,revents }
struct stPollItem_t ;
struct stPoll_t : public stTimeoutItem_t 
{
	struct pollfd *fds;
	nfds_t nfds; // typedef unsigned long int nfds_t;

	stPollItem_t *pPollItems;

	int iAllEventDetach;

	int iEpollFd;

	int iRaiseCnt;


};

/*
epoll item 
*/
struct stPollItem_t : public stTimeoutItem_t
{
	struct pollfd *pSelf;
	stPoll_t *pPoll;

	struct epoll_event stEvent;
};
/*
 *   EPOLLPRI 		POLLPRI    // There is urgent data to read.  
 *   EPOLLMSG 		POLLMSG
 *
 *   				POLLREMOVE
 *   				POLLRDHUP
 *   				POLLNVAL
 *
 * */
static uint32_t PollEvent2Epoll( short events )
{
	uint32_t e = 0;	
	if( events & POLLIN ) 	e |= EPOLLIN;
	if( events & POLLOUT )  e |= EPOLLOUT;
	if( events & POLLHUP ) 	e |= EPOLLHUP;
	if( events & POLLERR )	e |= EPOLLERR;
	return e;
}
static short EpollEvent2Poll( uint32_t events )
{
	short e = 0;	
	if( events & EPOLLIN ) 	e |= POLLIN;
	if( events & EPOLLOUT ) e |= POLLOUT;
	if( events & EPOLLHUP ) e |= POLLHUP;
	if( events & EPOLLERR ) e |= POLLERR;
	return e;
}
/* 进程pid 100k  占用静态内存区 */
static stCoRoutineEnv_t* g_arrCoEnvPerThread[ 102400 ] = { 0 };

/*

线程初始化  

每个线程都有一个stCoRoutineEnv_t

*/
void co_init_curr_thread_env()
{
	pid_t pid = GetPid();	
	g_arrCoEnvPerThread[ pid ] = (stCoRoutineEnv_t*)calloc( 1,sizeof(stCoRoutineEnv_t) );
	stCoRoutineEnv_t *env = g_arrCoEnvPerThread[ pid ];
	printf("init pid %ld env %p\n",(long)pid,env);

	env->iCallStackSize = 0;
	struct stCoRoutine_t *self = co_create_env( env,NULL,NULL );
	self->cIsMain = 1; /* 主coroutine */

	coctx_init( &self->ctx );

	env->pCallStack[ env->iCallStackSize++ ] = self;

	stCoEpoll_t *ev = AllocEpoll();
	SetEpoll( env,ev );
}
stCoRoutineEnv_t *co_get_curr_thread_env()
{
	return g_arrCoEnvPerThread[ GetPid() ];
}

/*  事件通知处理，启动对应coroutine   */
void OnPollProcessEvent( stTimeoutItem_t * ap )
{
	stCoRoutine_t *co = (stCoRoutine_t*)ap->pArg;
	co_resume( co );
}

void OnPollPreparePfn( stTimeoutItem_t * ap,struct epoll_event &e,stTimeoutItemLink_t *active )
{
	stPollItem_t *lp = (stPollItem_t *)ap;
	lp->pSelf->revents = EpollEvent2Poll( e.events );


	stPoll_t *pPoll = lp->pPoll;
	pPoll->iRaiseCnt++;

	if( !pPoll->iAllEventDetach )
	{
		pPoll->iAllEventDetach = 1;

		RemoveFromLink<stTimeoutItem_t,stTimeoutItemLink_t>( pPoll );

		AddTail( active,pPoll );

	}
}

/*
网络IO循环
调度coutine
*/
void co_eventloop( stCoEpoll_t *ctx,pfn_co_eventloop_t pfn,void *arg )
{
	epoll_event *result = (epoll_event*)calloc(1, sizeof(epoll_event) * stCoEpoll_t::_EPOLL_SIZE );

	for(;;)
	{
		int ret = epoll_wait( ctx->iEpollFd,result,stCoEpoll_t::_EPOLL_SIZE, 1 );

		stTimeoutItemLink_t *active = (ctx->pstActiveList);
		stTimeoutItemLink_t *timeout = (ctx->pstTimeoutList);

		memset( active,0,sizeof(stTimeoutItemLink_t) );
		memset( timeout,0,sizeof(stTimeoutItemLink_t) );

		for(int i=0;i<ret;i++)
		{
			/* 获取一次事件 */
			stTimeoutItem_t *item = (stTimeoutItem_t*)result[i].data.ptr;
			if( item->pfnPrepare )
			{
				/* 如果有必要进行预处理 */
				item->pfnPrepare( item,result[i],active );
			}
			else
			{
				/* 加入active，后面进行处理    */
				AddTail( active,item );
			}
		}


		unsigned long long now = GetTickMS();
		/* 进行一次超时检查 */
		TakeAllTimeout( ctx->pTimeout,now,timeout );

		stTimeoutItem_t *lp = timeout->head;
		while( lp )
		{
			//printf("raise timeout %p\n",lp);
			lp->bTimeout = true;
			lp = lp->pNext;
		}
		/* 合并事件 */
		Join<stTimeoutItem_t,stTimeoutItemLink_t>( active,timeout );
		/* 处理接收到事件 */
		lp = active->head;
		while( lp )
		{

			PopHead<stTimeoutItem_t,stTimeoutItemLink_t>( active );
			if( lp->pfnProcess )
			{
				lp->pfnProcess( lp );
			}

			lp = active->head;
		}
		if( pfn )
		{
			if( -1 == pfn( arg ) )
			{
				break;
			}
		}

	}
	free( result );
	result = 0;
}
void OnCoroutineEvent( stTimeoutItem_t * ap )
{
	stCoRoutine_t *co = (stCoRoutine_t*)ap->pArg;
	co_resume( co );
}

/* 
创建epoll 获得定时器与网络事件通知
*/
stCoEpoll_t *AllocEpoll()
{
	stCoEpoll_t *ctx = (stCoEpoll_t*)calloc( 1,sizeof(stCoEpoll_t) );
	/* 创建epoll */
	ctx->iEpollFd = epoll_create( stCoEpoll_t::_EPOLL_SIZE );
	ctx->pTimeout = AllocTimeout( 60 * 1000 );
	
	ctx->pstActiveList = (stTimeoutItemLink_t*)calloc( 1,sizeof(stTimeoutItemLink_t) );
	ctx->pstTimeoutList = (stTimeoutItemLink_t*)calloc( 1,sizeof(stTimeoutItemLink_t) );


	return ctx;
}

void FreeEpoll( stCoEpoll_t *ctx )
{
	if( ctx )
	{
		free( ctx->pstActiveList );
		free( ctx->pstTimeoutList );
		FreeTimeout( ctx->pTimeout );
	}
	free( ctx );
}

stCoRoutine_t *GetCurrCo( stCoRoutineEnv_t *env )
{
	return env->pCallStack[ env->iCallStackSize - 1 ];
}
stCoRoutine_t *GetCurrThreadCo( )
{
	stCoRoutineEnv_t *env = co_get_curr_thread_env();
	if( !env ) return 0;
	return GetCurrCo(env);
}


/*
需要重点分析  
*/
int co_poll( stCoEpoll_t *ctx,struct pollfd fds[], nfds_t nfds, int timeout )
{
	
	if( timeout > stTimeoutItem_t::eMaxTimeout )
	{
		timeout = stTimeoutItem_t::eMaxTimeout;
	}
	int epfd = ctx->iEpollFd;

	//1.struct change
	stPoll_t arg;
	memset( &arg,0,sizeof(arg) );

	arg.iEpollFd = epfd;
	arg.fds = fds;
	arg.nfds = nfds;

	stPollItem_t arr[2];
	if( nfds < sizeof(arr) / sizeof(arr[0]) )
	{
		arg.pPollItems = arr;
	}	
	else
	{
		arg.pPollItems = (stPollItem_t*)malloc( nfds * sizeof( stPollItem_t ) );
	}
	memset( arg.pPollItems,0,nfds * sizeof(stPollItem_t) );

	arg.pfnProcess = OnPollProcessEvent;
	arg.pArg = GetCurrCo( co_get_curr_thread_env() );
	
	//2.add timeout

	unsigned long long now = GetTickMS();
	arg.ullExpireTime = now + timeout;
	int ret = AddTimeout( ctx->pTimeout,&arg,now );
	if( ret != 0 )
	{
		co_log_err("CO_ERR: AddTimeout ret %d now %lld timeout %d arg.ullExpireTime %lld",
					ret,now,timeout,arg.ullExpireTime);
		errno = EINVAL;
		return -__LINE__;
	}
	//3. add epoll

	for(nfds_t i=0;i<nfds;i++)
	{
		arg.pPollItems[i].pSelf = fds + i;
		arg.pPollItems[i].pPoll = &arg;

		arg.pPollItems[i].pfnPrepare = OnPollPreparePfn;
		struct epoll_event &ev = arg.pPollItems[i].stEvent;

		if( fds[i].fd > -1 )
		{
			ev.data.ptr = arg.pPollItems + i;
			ev.events = PollEvent2Epoll( fds[i].events );
			// 添加到epoll 
			epoll_ctl( epfd,EPOLL_CTL_ADD, fds[i].fd, &ev );
		}
		//if fail,the timeout would work
		
	}

	co_yield_env( co_get_curr_thread_env() );

	RemoveFromLink<stTimeoutItem_t,stTimeoutItemLink_t>( &arg );
	for(nfds_t i = 0;i < nfds;i++)
	{
		int fd = fds[i].fd;
		if( fd > -1 )
		{
			// 从epoll中去注册 
			epoll_ctl( epfd,EPOLL_CTL_DEL,fd,&arg.pPollItems[i].stEvent );
		}
	}


	if( arg.pPollItems != arr )
	{
		free( arg.pPollItems );
		arg.pPollItems = NULL;
	}
	return arg.iRaiseCnt;
}

void SetEpoll( stCoRoutineEnv_t *env,stCoEpoll_t *ev )
{
	env->pEpoll = ev;
}
/*
获得CoEpoll
*/
stCoEpoll_t *co_get_epoll_ct()
{
	if( !co_get_curr_thread_env() )
	{
		co_init_curr_thread_env();
	}
	return co_get_curr_thread_env()->pEpoll;
}
struct stHookPThreadSpec_t
{
	stCoRoutine_t *co;
	void *value;

	enum 
	{
		size = 1024
	};
};
void *co_getspecific(pthread_key_t key)
{
	stCoRoutine_t *co = GetCurrThreadCo();
	if( !co || co->cIsMain )
	{
		return pthread_getspecific( key );
	}
	return co->aSpec[ key ].value;
}
int co_setspecific(pthread_key_t key, const void *value)
{
	stCoRoutine_t *co = GetCurrThreadCo();
	if( !co || co->cIsMain )
	{
		return pthread_setspecific( key,value );
	}
	co->aSpec[ key ].value = (void*)value;
	return 0;
}



void co_disable_hook_sys()
{
	stCoRoutine_t *co = GetCurrThreadCo();
	if( co )
	{
		co->cEnableSysHook = 0;
	}
}
bool co_is_enable_sys_hook()
{
	stCoRoutine_t *co = GetCurrThreadCo();
	return ( co && co->cEnableSysHook );
}

stCoRoutine_t *co_self()
{
	return GetCurrThreadCo();
}


