/*
 * Based on arch/arm/include/asm/thread_info.h
 *
 * Copyright (C) 2002 Russell King.
 * Copyright (C) 2012 ARM Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#ifndef __ASM_THREAD_INFO_H
#define __ASM_THREAD_INFO_H

#ifdef __KERNEL__

#include <linux/compiler.h>

#ifndef __ASSEMBLY__

struct task_struct;

#include <asm/memory.h>
#include <asm/stack_pointer.h>
#include <asm/types.h>

typedef unsigned long mm_segment_t;

/*
 * low level task data that entry.S needs immediate access to.
 */
struct thread_info {
	unsigned long		flags;		/* low level flags */
	mm_segment_t		addr_limit;	/* address limit */
#ifdef CONFIG_ARM64_SW_TTBR0_PAN
	u64			ttbr0;		/* saved TTBR0_EL1 */
#endif
	int			preempt_count;	/* 0 => preemptable, <0 => bug */
};

#define thread_saved_pc(tsk)	\
	((unsigned long)(tsk->thread.cpu_context.pc))
#define thread_saved_sp(tsk)	\
	((unsigned long)(tsk->thread.cpu_context.sp))
#define thread_saved_fp(tsk)	\
	((unsigned long)(tsk->thread.cpu_context.fp))

void arch_setup_new_exec(void);
#define arch_setup_new_exec     arch_setup_new_exec

void arch_release_task_struct(struct task_struct *tsk);

#endif

/*
 * thread information flags:
 *  TIF_SYSCALL_TRACE	- syscall trace active
 *  TIF_SYSCALL_TRACEPOINT - syscall tracepoint for ftrace
 *  TIF_SYSCALL_AUDIT	- syscall auditing
 *  TIF_SECOMP		- syscall secure computing
 *  TIF_SIGPENDING	- signal pending
 *  TIF_NEED_RESCHED	- rescheduling necessary
 *  TIF_NOTIFY_RESUME	- callback before returning to user
 *  TIF_USEDFPU		- FPU was used by this task this quantum (SMP)
 *  TIF_POLLING_NRFLAG - idle is polling for TIF_NEED_RESCHED
 */
#define TIF_SIGPENDING		0
#define TIF_NEED_RESCHED	1
#define TIF_NOTIFY_RESUME	2	/* callback before returning to user */
#define TIF_FOREIGN_FPSTATE	3	/* CPU's FP state is not current's */
#define TIF_UPROBE		4	/* uprobe breakpoint or singlestep */
#define TIF_FSCHECK		5	/* Check FS is USER_DS on return */
#define TIF_SEA_NOTIFY          6       /* notify to do an error recovery */
#define TIF_NOHZ		7
#define TIF_SYSCALL_TRACE	8
#define TIF_SYSCALL_AUDIT	9
#define TIF_SYSCALL_TRACEPOINT	10
#define TIF_SECCOMP		11
#define TIF_POLLING_NRFLAG	16
#define TIF_MEMDIE		18	/* is terminating due to OOM killer */
#define TIF_FREEZE		19
#define TIF_RESTORE_SIGMASK	20
#define TIF_SINGLESTEP		21
#define TIF_32BIT		22	/* AARCH32 process */
#define TIF_SVE			23	/* Scalable Vector Extension in use */
#define TIF_SVE_VL_INHERIT	24	/* Inherit sve_vl_onexec across exec */
#define TIF_SSBD		25	/* Wants SSB mitigation */
#define TIF_32BIT_AARCH64	26	/* 32 bit process on AArch64(ILP32) */

#ifdef CONFIG_UCE_KERNEL_RECOVERY
#define TIF_UCE_KERNEL_RECOVERY 27
#endif

#define _TIF_SIGPENDING		(1 << TIF_SIGPENDING)
#define _TIF_NEED_RESCHED	(1 << TIF_NEED_RESCHED)
#define _TIF_NOTIFY_RESUME	(1 << TIF_NOTIFY_RESUME)
#define _TIF_FOREIGN_FPSTATE	(1 << TIF_FOREIGN_FPSTATE)
#define _TIF_NOHZ		(1 << TIF_NOHZ)
#define _TIF_SEA_NOTIFY         (1 << TIF_SEA_NOTIFY)
#define _TIF_SYSCALL_TRACE	(1 << TIF_SYSCALL_TRACE)
#define _TIF_SYSCALL_AUDIT	(1 << TIF_SYSCALL_AUDIT)
#define _TIF_SYSCALL_TRACEPOINT	(1 << TIF_SYSCALL_TRACEPOINT)
#define _TIF_SECCOMP		(1 << TIF_SECCOMP)
#define _TIF_UPROBE		(1 << TIF_UPROBE)
#define _TIF_FSCHECK		(1 << TIF_FSCHECK)
#define _TIF_32BIT		(1 << TIF_32BIT)
#define _TIF_SVE		(1 << TIF_SVE)
#define _TIF_32BIT_AARCH64	(1 << TIF_32BIT_AARCH64)
#define _TIF_POLLING_NRFLAG	(1 << TIF_POLLING_NRFLAG)

#ifdef CONFIG_UCE_KERNEL_RECOVERY
#define _TIF_UCE_KERNEL_RECOVERY       (1 << TIF_UCE_KERNEL_RECOVERY)
#endif

#define _TIF_WORK_MASK		(_TIF_NEED_RESCHED | _TIF_SIGPENDING | \
				 _TIF_NOTIFY_RESUME | _TIF_FOREIGN_FPSTATE | \
				 _TIF_UPROBE | _TIF_FSCHECK | _TIF_SEA_NOTIFY)

#define _TIF_SYSCALL_WORK	(_TIF_SYSCALL_TRACE | _TIF_SYSCALL_AUDIT | \
				 _TIF_SYSCALL_TRACEPOINT | _TIF_SECCOMP | \
				 _TIF_NOHZ)

#define INIT_THREAD_INFO(tsk)						\
{									\
	.flags		= _TIF_FOREIGN_FPSTATE,				\
	.preempt_count	= INIT_PREEMPT_COUNT,				\
	.addr_limit	= KERNEL_DS,					\
}

#endif /* __KERNEL__ */
#endif /* __ASM_THREAD_INFO_H */
