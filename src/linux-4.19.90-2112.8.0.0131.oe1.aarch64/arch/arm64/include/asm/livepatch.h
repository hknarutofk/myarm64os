/* SPDX-License-Identifier: GPL-2.0
 *
 * Copyright (C) 2014-2019, Huawei.
 *	Author: Li Bin <huawei.libin@huawei.com>
 *	Author: Cheng Jian <cj.chengjian@huawei.com>
 *
 * livepatch.h - arm64-specific Kernel Live Patching Core
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#ifndef _ASM_ARM64_LIVEPATCH_H
#define _ASM_ARM64_LIVEPATCH_H

#include <linux/module.h>
#include <linux/livepatch.h>


#ifdef CONFIG_LIVEPATCH

struct klp_patch;
struct klp_func;

#define klp_smp_isb() isb()

static inline int klp_check_compiler_support(void)
{
	return 0;
}

int arch_klp_patch_func(struct klp_func *func);
void arch_klp_unpatch_func(struct klp_func *func);
int klp_check_calltrace(struct klp_patch *patch, int enable);
#else
#error Live patching support is disabled; check CONFIG_LIVEPATCH
#endif

#endif /* _ASM_ARM64_LIVEPATCH_H */
