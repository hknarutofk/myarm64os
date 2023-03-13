/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __RAS_H__
#define __RAS_H__

#include <asm/errno.h>
#include <linux/uuid.h>
#include <linux/cper.h>

#ifdef CONFIG_DEBUG_FS
int ras_userspace_consumers(void);
void ras_debugfs_init(void);
int ras_add_daemon_trace(void);
#else
static inline int ras_userspace_consumers(void) { return 0; }
static inline void ras_debugfs_init(void) { }
static inline int ras_add_daemon_trace(void) { return 0; }
#endif

#ifdef CONFIG_RAS_CEC
void __init cec_init(void);
int __init parse_cec_param(char *str);
int cec_add_elem(u64 pfn);
#else
static inline void __init cec_init(void)	{ }
static inline int cec_add_elem(u64 pfn)		{ return -ENODEV; }
#endif

#ifdef CONFIG_RAS
void log_non_standard_event(const guid_t *sec_type,
			    const guid_t *fru_id, const char *fru_text,
			    const u8 sev, const u8 *err, const u32 len);
void log_arm_hw_error(struct cper_sec_proc_arm *err, const u8 sev);
#else
static inline void
log_non_standard_event(const guid_t *sec_type,
		       const guid_t *fru_id, const char *fru_text,
		       const u8 sev, const u8 *err, const u32 len)
{ return; }
static inline void
log_arm_hw_error(struct cper_sec_proc_arm *err, const u8 sev) { return; }
#endif

#if defined(CONFIG_ARM) || defined(CONFIG_ARM64)
#include <asm/smp_plat.h>
/*
 * Include ARM specific SMP header which provides a function mapping mpidr to
 * cpu logical index.
 */
#define GET_LOGICAL_INDEX(mpidr) get_logical_index(mpidr & MPIDR_HWID_BITMASK)
#else
#define GET_LOGICAL_INDEX(mpidr) -EINVAL
#endif /* CONFIG_ARM || CONFIG_ARM64 */

#endif /* __RAS_H__ */
