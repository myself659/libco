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

#define _esp 0
#define _eip 4
/* ------ */
#define _rsp 0
#define _rip 8
#define _rbx 16
#define _rdi 24
#define _rsi 32

.globl coctx_swap 
.type  coctx_swap, @function
coctx_swap:

#if defined(__i386__)

	leal -4(%esp),%esp
	pushl %ebx
	pushl %ecx
	pushl %edx

	pushl %edi
	pushl %esi
	pushl %ebp
	leal 28(%esp),%esp

	/* sp,ip */
	/* param old ctx */
	movl 4(%esp), %eax

	movl (%esp), %ecx
	movl %ecx, _eip(%eax)

	leal 4(%esp), %ecx
	movl %ecx, _esp(%eax)

	/* param new ctx */
	movl 8(%esp), %eax

	/* push ret ( ip,sp ) */
	movl _eip(%eax), %ecx
	movl _esp(%eax), %esp
	pushl %ecx

	leal -28(%esp),%esp
	popl %ebp
	popl %esi
	popl %edi

	popl %edx
	popl %ecx
	popl %ebx
	leal 4(%esp),%esp
	
	movl $0, %eax
	ret

#elif defined(__x86_64__)
	/* 
	rsp stack poiter 
	修改rsp地址,保存当前寄存器 
	*/
	/*
	rsp：对应32位esp寄存器，保存当前堆栈栈顶指针的寄存器
	rsp = rsp -8 
	向下生长
	*/
	leaq -8(%rsp),%rsp
	/* 
	callee-saved register, optionally used as frame pointer 
	当前堆栈基地址指针的寄存器
	*/
	pushq %rbp
	/* callee save register */
	pushq %r12
	pushq %r13
	pushq %r14
	pushq %r15
	/* rdx 第3个参数 rcx 第4个参数 */
	pushq %rdx
	pushq %rcx	
	/*r8 第5个参数  r9 第6个参数 */
	pushq %r8
	pushq %r9
	
	/*  
	rsp = rsp +80 
	恢复rsp，保存到内存
	*/
	leaq 80(%rsp),%rsp	
	
	/* rdi 第一个入参 */
	/* 保存上一个ctx的寄存器信息到内存 */
	movq %rbx,_rbx(%rdi)
	movq %rdi,_rdi(%rdi)
	movq %rsi,_rsi(%rdi)
	/* sp */
	movq (%rsp), %rcx
	movq %rcx, _rip(%rdi)
	leaq 8(%rsp), %rcx		
	movq %rcx, _rsp(%rdi)

	/* rsi 第二个入参 */
	/* 将下一个ctx的函数更新到寄存器 */
	/* sp */
	movq _rip(%rsi), %rcx
	
	/* 目的rsp保存到rsp */
	movq _rsp(%rsi), %rsp
	
	/* 目的rip入栈  */
	pushq %rcx

	movq _rbx(%rsi),%rbx
	movq _rdi(%rsi),%rdi
	movq _rsi(%rsi),%rsi
	
	/* rsp = rsp - 80 */
	leaq -80(%rsp),%rsp
	/* 按入栈顺序逆序出栈 */
	popq %r9
	popq %r8
	popq %rcx	
	popq %rdx
	popq %r15
	popq %r14
	popq %r13
	popq %r12
	/* 恢复栈底 */
	popq %rbp
	/* 恢复原来rsp  rsp = rsp+8 */
	leaq 8(%rsp),%rsp	
	/* 异或运算，设置EAX为0 相比较于mov更高效  */
	xorl %eax, %eax
	/* 返回  */
	ret
#endif

/*
不变的cpu执行 
可变的寄存器 
实现函数的调用
*/
