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


#ifndef __CO_ROUTINE_INNER_H__

#include "co_routine.h"
#include "coctx.h"
struct stCoRoutineEnv_t;
struct stCoSpec_t
{
	void *value;
};
/*
CoRoutine信息
*/
struct stCoRoutine_t
{
	/* 共享堆栈 */
	stCoRoutineEnv_t *env;
	/* 回调函数 */
	pfn_co_routine_t pfn;
	/* 回调入参  */
	void *arg;
	//ucontext_t ctx;
	coctx_t ctx;
	char cStart; //是否启动
	char cEnd; // routine活干完，可以回收了 
	stCoSpec_t aSpec[1024];
	char cIsMain;
	char cEnableSysHook; /* 是否开启系统调用hook， 为1表示开启  */

	char sRunStack[ 1024 * 128 ]; /* 运行堆栈 128k 关注局部变量大小控制 */

};



//1.env
void 				co_init_curr_thread_env();
stCoRoutineEnv_t *	co_get_curr_thread_env();

//2.coroutine
void    co_free( stCoRoutine_t * co );
void    co_yield_env(  stCoRoutineEnv_t *env );

//3.func



//-----------------------------------------------------------------------------------------------

struct stTimeout_t;
struct stTimeoutItem_t ;

stTimeout_t *AllocTimeout( int iSize );
void 	FreeTimeout( stTimeout_t *apTimeout );
int  	AddTimeout( stTimeout_t *apTimeout,stTimeoutItem_t *apItem ,uint64_t allNow );

struct stCoEpoll_t;
stCoEpoll_t * AllocEpoll();
void 		FreeEpoll( stCoEpoll_t *ctx );

stCoRoutine_t *		GetCurrThreadCo();
void 				SetEpoll( stCoRoutineEnv_t *env,stCoEpoll_t *ev );

typedef void (*pfnCoRoutineFunc_t)();

#endif

#define __CO_ROUTINE_INNER_H__
