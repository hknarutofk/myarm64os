#ifndef LINUX_SHARE_POOL_H
#define LINUX_SHARE_POOL_H

#include <linux/mman.h>
#include <linux/mm_types.h>
#include <linux/notifier.h>
#include <linux/vmalloc.h>
#include <linux/printk.h>
#include <linux/hashtable.h>

#define SP_HUGEPAGE		(1 << 0)
#define SP_HUGEPAGE_ONLY	(1 << 1)
#define SP_DVPP			(1 << 2)
#define DEVICE_ID_MASK		0x3ff
#define DEVICE_ID_SHIFT		32
#define SP_FLAG_MASK		(SP_HUGEPAGE | SP_HUGEPAGE_ONLY | SP_DVPP | \
				(_AC(DEVICE_ID_MASK, UL) << DEVICE_ID_SHIFT))

#define SPG_ID_NONE	(-1)	/* not associated with sp_group, only for specified thread */
#define SPG_ID_DEFAULT	0	/* use the spg id of current thread */
#define SPG_ID_MIN	1	/* valid id should be >= 1 */
#define SPG_ID_MAX	99999
#define SPG_ID_AUTO_MIN 100000
#define SPG_ID_AUTO_MAX 199999
#define SPG_ID_AUTO     200000  /* generate group id automatically */

#define MAX_DEVID 2	/* the max num of Da-vinci devices */

extern int sysctl_share_pool_hugepage_enable;

extern int sysctl_ac_mode;

extern int sysctl_sp_debug_mode;

extern int enable_ascend_share_pool;

extern int sysctl_share_pool_map_lock_enable;

extern int sysctl_sp_compact_enable;
extern unsigned long sysctl_sp_compact_interval;
extern unsigned long sysctl_sp_compact_interval_max;
extern int sysctl_sp_perf_alloc;

extern int sysctl_sp_perf_k2u;

#ifdef CONFIG_HAVE_ARCH_HUGE_VMALLOC
extern bool vmap_allow_huge;
#endif

/* we estimate an sp-group ususally contains at most 64 sp-group */
#define SP_SPG_HASH_BITS 6

struct sp_spg_stat {
	int spg_id;
	/* record the number of hugepage allocation failures */
	atomic_t hugepage_failures;
	/* number of sp_area */
	atomic_t	 spa_num;
	/* total size of all sp_area from sp_alloc and k2u */
	atomic64_t	 size;
	/* total size of all sp_area from sp_alloc 0-order page */
	atomic64_t	 alloc_nsize;
	/* total size of all sp_area from sp_alloc hugepage */
	atomic64_t	 alloc_hsize;
	/* total size of all sp_area from ap_alloc */
	atomic64_t	 alloc_size;
	/* total size of all sp_area from sp_k2u */
	atomic64_t	 k2u_size;
	struct mutex	 lock;  /* protect hashtable */
	DECLARE_HASHTABLE(hash, SP_SPG_HASH_BITS);
};

/* we estimate a process ususally belongs to at most 16 sp-group */
#define SP_PROC_HASH_BITS 4

/* per process memory usage statistics indexed by tgid */
struct sp_proc_stat {
	atomic_t use_count;
	int tgid;
	struct mm_struct *mm;
	struct mutex lock;  /* protect hashtable */
	DECLARE_HASHTABLE(hash, SP_PROC_HASH_BITS);
	char comm[TASK_COMM_LEN];
	/*
	 * alloc amount minus free amount, may be negative when freed by
	 * another task in the same sp group.
	 */
	atomic64_t alloc_size;
	atomic64_t k2u_size;
};

/* Processes in the same sp_group can share memory.
 * Memory layout for share pool:
 *
 * |-------------------- 8T -------------------|---|------ 8T ------------|
 * |		Device 0	   |  Device 1 |...|                      |
 * |----------------------------------------------------------------------|
 * |------------- 16G -------------|    16G    |   |                      |
 * | DVPP GROUP0   | DVPP GROUP1   | ... | ... |...|  sp normal memory    |
 * |     sp        |    sp         |     |     |   |                      |
 * |----------------------------------------------------------------------|
 *
 * The host SVM feature reserves 8T virtual memory by mmap, and due to the
 * restriction of DVPP, while SVM and share pool will both allocate memory
 * for DVPP, the memory have to be in the same 32G range.
 *
 * Share pool reserves 16T memory, with 8T for normal uses and 8T for DVPP.
 * Within this 8T DVPP memory, SVM will call sp_config_dvpp_range() to
 * tell us which 16G memory range is reserved for share pool .
 *
 * In some scenarios where there is no host SVM feature, share pool uses
 * the default 8G memory setting for DVPP.
 */
struct sp_group {
	int		 id;
	struct file	 *file;
	struct file	 *file_hugetlb;
	/* number of process in this group */
	int		 proc_num;
	/* list head of processes (sp_group_node, each represents a process) */
	struct list_head procs;
	/* list head of sp_area. it is protected by spin_lock sp_area_lock */
	struct list_head spa_list;
	/* group statistics */
	struct sp_spg_stat *stat;
	/* we define the creator process of a sp_group as owner */
	struct task_struct *owner;
	/* is_alive == false means it's being destroyed */
	bool		 is_alive;
	atomic_t	 use_count;
	/* protect the group internal elements, except spa_list */
	struct rw_semaphore	rw_lock;
};

/* a per-process(per mm) struct which manages a sp_group_node list */
struct sp_group_master {
	/*
	 * number of sp groups the process belongs to,
	 * a.k.a the number of sp_node in node_list
	 */
	unsigned int count;
	/* list head of sp_node */
	struct list_head node_list;
	struct mm_struct *mm;
	struct sp_proc_stat *stat;
};

/*
 * each instance represents an sp group the process belongs to
 * sp_group_master    : sp_group_node   = 1 : N
 * sp_group_node->spg : sp_group        = 1 : 1
 * sp_group_node      : sp_group->procs = N : 1
 */
struct sp_group_node {
	/* list node in sp_group->procs */
	struct list_head proc_node;
	/* list node in sp_group_maseter->node_list */
	struct list_head group_node;
	struct sp_group_master *master;
	struct sp_group *spg;
	unsigned long prot;
};

struct sp_walk_data {
	struct page **pages;
	unsigned int page_count;
	unsigned long uva_aligned;
	unsigned long page_size;
	bool is_hugepage;
	pmd_t *pmd;
};

#define MAP_SHARE_POOL			0x100000

#define MMAP_TOP_4G_SIZE		0x100000000UL

/* 8T size */
#define MMAP_SHARE_POOL_NORMAL_SIZE	0x80000000000UL
/* 8T size*/
#define MMAP_SHARE_POOL_DVPP_SIZE	0x80000000000UL
/* 16G size */
#define MMAP_SHARE_POOL_16G_SIZE	0x400000000UL
#define MMAP_SHARE_POOL_SIZE		(MMAP_SHARE_POOL_NORMAL_SIZE + MMAP_SHARE_POOL_DVPP_SIZE)
/* align to 2M hugepage size, and MMAP_SHARE_POOL_TOP_16G_START should be align to 16G */
#define MMAP_SHARE_POOL_END		((TASK_SIZE - MMAP_SHARE_POOL_DVPP_SIZE) & ~((1 << 21) - 1))
#define MMAP_SHARE_POOL_START		(MMAP_SHARE_POOL_END - MMAP_SHARE_POOL_SIZE)
#define MMAP_SHARE_POOL_16G_START	(MMAP_SHARE_POOL_END - MMAP_SHARE_POOL_DVPP_SIZE)

#ifdef CONFIG_ASCEND_SHARE_POOL

static inline void sp_init_mm(struct mm_struct *mm)
{
	mm->sp_group_master = NULL;
}

extern int mg_sp_group_add_task(int pid, unsigned long prot, int spg_id);
extern int sp_group_add_task(int pid, int spg_id);

extern int mg_sp_group_del_task(int pid, int spg_id);
extern int sp_group_del_task(int pid, int spg_id);

extern int sp_group_exit(struct mm_struct *mm);
extern void sp_group_post_exit(struct mm_struct *mm);

extern int mg_sp_group_id_by_pid(int pid, int *spg_ids, int *num);
extern int sp_group_id_by_pid(int pid);

extern int sp_group_walk(int spg_id, void *data, int (*func)(struct mm_struct *mm, void *));
extern int proc_sp_group_state(struct seq_file *m, struct pid_namespace *ns,
			struct pid *pid, struct task_struct *task);

extern void *sp_alloc(unsigned long size, unsigned long sp_flags, int spg_id);
extern void *mg_sp_alloc(unsigned long size, unsigned long sp_flags, int spg_id);

extern int sp_free(unsigned long addr);
extern int mg_sp_free(unsigned long addr);

extern void *sp_make_share_k2u(unsigned long kva, unsigned long size,
			unsigned long sp_flags, int pid, int spg_id);
extern void *mg_sp_make_share_k2u(unsigned long kva, unsigned long size,
			unsigned long sp_flags, int pid, int spg_id);

extern void *sp_make_share_u2k(unsigned long uva, unsigned long size, int pid);
extern void *mg_sp_make_share_u2k(unsigned long uva, unsigned long size, int pid);

extern int sp_unshare(unsigned long va, unsigned long size, int pid, int spg_id);
extern int mg_sp_unshare(unsigned long va, unsigned long size);

extern void sp_area_drop(struct vm_area_struct *vma);

extern int sp_walk_page_range(unsigned long uva, unsigned long size,
	struct task_struct *tsk, struct sp_walk_data *sp_walk_data);
extern int mg_sp_walk_page_range(unsigned long uva, unsigned long size,
	struct task_struct *tsk, struct sp_walk_data *sp_walk_data);

extern void sp_walk_page_free(struct sp_walk_data *sp_walk_data);
extern void mg_sp_walk_page_free(struct sp_walk_data *sp_walk_data);

extern int sp_register_notifier(struct notifier_block *nb);
extern int sp_unregister_notifier(struct notifier_block *nb);

extern bool sp_config_dvpp_range(size_t start, size_t size, int device_id, int pid);
extern bool mg_sp_config_dvpp_range(size_t start, size_t size, int device_id, int pid);

extern bool is_sharepool_addr(unsigned long addr);
extern bool mg_is_sharepool_addr(unsigned long addr);

extern struct sp_proc_stat *sp_get_proc_stat_ref(struct mm_struct *mm);
extern void sp_proc_stat_drop(struct sp_proc_stat *stat);
extern void spa_overview_show(struct seq_file *seq);
extern void spg_overview_show(struct seq_file *seq);
extern void proc_sharepool_init(void);
extern int sp_node_id(struct vm_area_struct *vma);

static inline struct task_struct *sp_get_task(struct mm_struct *mm)
{
	if (enable_ascend_share_pool)
		return mm->owner;
	else
		return current;
}

static inline bool sp_check_hugepage(struct page *p)
{
	if (enable_ascend_share_pool && PageHuge(p))
		return true;

	return false;
}

static inline bool sp_is_enabled(void)
{
	return enable_ascend_share_pool ? true : false;
}

static inline bool sp_check_vm_huge_page(unsigned long flags)
{
	if (enable_ascend_share_pool && (flags & VM_HUGE_PAGES))
		return true;

	return false;
}

static inline void sp_area_work_around(struct vm_unmapped_area_info *info,
				       unsigned long flags)
{
	/* the MAP_DVPP couldn't work with MAP_SHARE_POOL. In addition, the
	 * address ranges corresponding to the two flags must not overlap.
	 */
	if (enable_ascend_share_pool && !(flags & MAP_DVPP))
		info->high_limit = min(info->high_limit, MMAP_SHARE_POOL_START);
}

extern struct page *sp_alloc_pages(struct vm_struct *area, gfp_t mask,
						 unsigned int page_order, int node);
static inline bool sp_check_vm_share_pool(unsigned long vm_flags)
{
	if (enable_ascend_share_pool && (vm_flags & VM_SHARE_POOL))
		return true;

	return false;
}

static inline bool is_vm_huge_special(struct vm_area_struct *vma)
{
	return !!(enable_ascend_share_pool && (vma->vm_flags & VM_HUGE_SPECIAL));
}

static inline bool sp_mmap_check(unsigned long flags)
{
	if (enable_ascend_share_pool && (flags & MAP_SHARE_POOL))
		return true;

	return false;
}

static inline void sp_dump_stack(void)
{
	if (sysctl_sp_debug_mode)
		dump_stack();
}

static inline bool ascend_sp_oom_show(void)
{
	return enable_ascend_share_pool ? true : false;
}

vm_fault_t sharepool_no_page(struct mm_struct *mm,
			struct vm_area_struct *vma,
			struct address_space *mapping, pgoff_t idx,
			unsigned long address, pte_t *ptep, unsigned int flags);

void sp_exit_mm(struct mm_struct *mm);

static inline bool is_vmalloc_huge(unsigned long vm_flags)
{
	if (enable_ascend_share_pool && (vm_flags & VM_HUGE_PAGES))
		return true;

	return false;
}

static inline bool is_vmalloc_sharepool(unsigned long vm_flags)
{
	if (enable_ascend_share_pool && (vm_flags & VM_SHAREPOOL))
		return true;

	return false;
}

static inline void sp_free_pages(struct page *page, struct vm_struct *area)
{
	if (PageHuge(page))
		put_page(page);
	else
		__free_pages(page, is_vmalloc_huge(area->flags) ? PMD_SHIFT - PAGE_SHIFT : 0);
}

extern bool sp_check_addr(unsigned long addr);
extern bool sp_check_mmap_addr(unsigned long addr, unsigned long flags);

#else

static inline int mg_sp_group_add_task(int pid, unsigned long prot, int spg_id)
{
	return -EPERM;
}

static inline int sp_group_add_task(int pid, int spg_id)
{
	return -EPERM;
}

static inline int mg_sp_group_del_task(int pid, int spg_id)
{
	return -EPERM;
}

static inline int sp_group_del_task(int pid, int spg_id)
{
	return -EPERM;
}

static inline int sp_group_exit(struct mm_struct *mm)
{
	return 0;
}

static inline void sp_group_post_exit(struct mm_struct *mm)
{
}

static inline int mg_sp_group_id_by_pid(int pid, int *spg_ids, int *num)
{
	return -EPERM;
}

static inline int sp_group_id_by_pid(int pid)
{
	return -EPERM;
}

static inline  int proc_sp_group_state(struct seq_file *m, struct pid_namespace *ns,
			       struct pid *pid, struct task_struct *task)
{
	return -EPERM;
}

static inline void *sp_alloc(unsigned long size, unsigned long sp_flags, int sp_id)
{
	return NULL;
}

static inline void *mg_sp_alloc(unsigned long size, unsigned long sp_flags, int spg_id)
{
	return NULL;
}

static inline int sp_free(unsigned long addr)
{
	return -EPERM;
}

static inline int mg_sp_free(unsigned long addr)
{
	return -EPERM;
}

static inline void *sp_make_share_k2u(unsigned long kva, unsigned long size,
		      unsigned long sp_flags, int pid, int spg_id)
{
	return NULL;
}

static inline void *mg_sp_make_share_k2u(unsigned long kva, unsigned long size,
			unsigned long sp_flags, int pid, int spg_id)
{
	return NULL;
}

static inline void *sp_make_share_u2k(unsigned long uva, unsigned long size, int pid)
{
	return NULL;
}

static inline void *mg_sp_make_share_u2k(unsigned long uva, unsigned long size, int pid)
{
	return NULL;
}

static inline int sp_unshare(unsigned long va, unsigned long size, int pid, int spg_id)
{
	return -EPERM;
}

static inline int mg_sp_unshare(unsigned long va, unsigned long size)
{
	return -EPERM;
}


static inline void sp_init_mm(struct mm_struct *mm)
{
}

static inline void sp_area_drop(struct vm_area_struct *vma)
{
}

static inline int sp_walk_page_range(unsigned long uva, unsigned long size,
	struct task_struct *tsk, struct sp_walk_data *sp_walk_data)
{
	return 0;
}

static inline int mg_sp_walk_page_range(unsigned long uva, unsigned long size,
	struct task_struct *tsk, struct sp_walk_data *sp_walk_data)
{
	return 0;
}

static inline void sp_walk_page_free(struct sp_walk_data *sp_walk_data)
{
}

static inline void mg_sp_walk_page_free(struct sp_walk_data *sp_walk_data)
{
}

static inline int sp_register_notifier(struct notifier_block *nb)
{
	return -EPERM;
}

static inline int sp_unregister_notifier(struct notifier_block *nb)
{
	return -EPERM;
}

static inline bool sp_config_dvpp_range(size_t start, size_t size, int device_id, int pid)
{
	return false;
}

static inline bool mg_sp_config_dvpp_range(size_t start, size_t size, int device_id, int pid)
{
	return false;
}

static inline bool is_sharepool_addr(unsigned long addr)
{
	return false;
}

static inline bool mg_is_sharepool_addr(unsigned long addr)
{
	return false;
}

static inline struct sp_proc_stat *sp_get_proc_stat_ref(struct mm_struct *mm)
{
	return NULL;
}

static inline void sp_proc_stat_drop(struct sp_proc_stat *stat)
{
}

static inline void spa_overview_show(struct seq_file *seq)
{
}

static inline void spg_overview_show(struct seq_file *seq)
{
}

static inline void proc_sharepool_init(void)
{
}

static inline struct task_struct  *sp_get_task(struct mm_struct *mm)
{
	return current;
}

static inline bool sp_check_hugepage(struct page *p)
{
	return false;
}

static inline bool sp_is_enabled(void)
{
	return false;
}

static inline bool sp_check_vm_huge_page(unsigned long flags)
{
	return false;
}

static inline void sp_area_work_around(struct vm_unmapped_area_info *info,
				       unsigned long flags)
{
}

static inline struct page *sp_alloc_pages(void *area, gfp_t mask,
					  unsigned int page_order, int node)
{
	return NULL;
}

static inline bool sp_check_vm_share_pool(unsigned long vm_flags)
{
	return false;
}

static inline bool is_vm_huge_special(struct vm_area_struct *vma)
{
	return false;
}

static inline bool sp_mmap_check(unsigned long flags)
{
	return false;
}

static inline void sp_dump_stack(void)
{
}

static inline bool ascend_sp_oom_show(void)
{
	return false;
}

static inline bool is_vmalloc_huge(unsigned long vm_flags)
{
	return NULL;
}

static inline bool is_vmalloc_sharepool(unsigned long vm_flags)
{
	return NULL;
}

static inline void sp_free_pages(struct page *page, struct vm_struct *area)
{
}

static inline int sp_node_id(struct vm_area_struct *vma)
{
	return numa_node_id();
}

static inline bool sp_check_addr(unsigned long addr)
{
	return false;
}

static inline bool sp_check_mmap_addr(unsigned long addr, unsigned long flags)
{
	return false;
}

#endif

#endif /* LINUX_SHARE_POOL_H */
