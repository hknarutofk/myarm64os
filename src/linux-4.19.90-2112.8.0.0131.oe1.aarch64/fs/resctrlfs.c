// SPDX-License-Identifier: GPL-2.0+
/*
 * User interface for ARM v8 MPAM
 *
 * Copyright (C) 2016 Intel Corporation
 * Copyright (C) 2018-2019 Huawei Technologies Co., Ltd
 *
 * Author:
 *   Fenghua Yu <fenghua.yu@intel.com>
 *   Xie XiuQi <xiexiuqi@huawei.com>
 *
 * Code was partially borrowed from arch/x86/kernel/cpu/intel_rdt*.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * More information about MPAM be found in the Arm Architecture Reference
 * Manual.
 *
 * https://static.docs.arm.com/ddi0598/a/DDI0598_MPAM_supp_armv8a.pdf
 */

#define pr_fmt(fmt)	KBUILD_MODNAME ": " fmt

#include <linux/cpu.h>
#include <linux/fs.h>
#include <linux/sysfs.h>
#include <linux/kernfs.h>
#include <linux/seq_buf.h>
#include <linux/seq_file.h>
#include <linux/sched/signal.h>
#include <linux/sched/task.h>
#include <linux/slab.h>
#include <linux/resctrlfs.h>

#include <uapi/linux/magic.h>

#include <asm/resctrl.h>
#include <asm/mpam.h>

DEFINE_STATIC_KEY_FALSE(resctrl_enable_key);
DEFINE_STATIC_KEY_FALSE(resctrl_mon_enable_key);
DEFINE_STATIC_KEY_FALSE(resctrl_alloc_enable_key);
static struct kernfs_root *resctrl_root;
struct resctrl_group resctrl_group_default;
LIST_HEAD(resctrl_all_groups);

/* Kernel fs node for "info" directory under root */
static struct kernfs_node *kn_info;

/* Kernel fs node for "mon_groups" directory under root */
static struct kernfs_node *kn_mongrp;

/* Kernel fs node for "mon_data" directory under root */
static struct kernfs_node *kn_mondata;

/* set uid and gid of resctrl_group dirs and files to that of the creator */
static int resctrl_group_kn_set_ugid(struct kernfs_node *kn)
{
	struct iattr iattr = { .ia_valid = ATTR_UID | ATTR_GID,
				.ia_uid = current_fsuid(),
				.ia_gid = current_fsgid(), };

	if (uid_eq(iattr.ia_uid, GLOBAL_ROOT_UID) &&
	    gid_eq(iattr.ia_gid, GLOBAL_ROOT_GID))
		return 0;

	return kernfs_setattr(kn, &iattr);
}

static int resctrl_group_add_file(struct kernfs_node *parent_kn, struct rftype *rft)
{
	struct kernfs_node *kn;
	int ret;

	kn = __kernfs_create_file(parent_kn, rft->name, rft->mode,
				  GLOBAL_ROOT_UID, GLOBAL_ROOT_GID,
				  0, rft->kf_ops, rft, NULL, NULL);
	if (IS_ERR(kn))
		return PTR_ERR(kn);

	ret = resctrl_group_kn_set_ugid(kn);
	if (ret) {
		kernfs_remove(kn);
		return ret;
	}

	return 0;
}

static struct rftype *res_common_files;
static size_t res_common_files_len;

int register_resctrl_specific_files(struct rftype *files, size_t len)
{
	if (res_common_files) {
		pr_err("Only allowed register specific files once\n");
		return -EINVAL;
	}

	if (!files) {
		pr_err("Invalid input files\n");
		return -EINVAL;
	}

	res_common_files = files;
	res_common_files_len = len;

	return 0;
}

static int __resctrl_group_add_files(struct kernfs_node *kn, unsigned long fflags,
				     struct rftype *rfts, int len)
{
	struct rftype *rft;
	int ret = 0;

	lockdep_assert_held(&resctrl_group_mutex);

	for (rft = rfts; rft < rfts + len; rft++) {
		if (rft->enable && !rft->enable(NULL))
			continue;

		if ((fflags & rft->fflags) == rft->fflags) {
			ret = resctrl_group_add_file(kn, rft);
			if (ret)
				goto error;
		}
	}

	return 0;
error:
	pr_warn("Failed to add %s, err=%d\n", rft->name, ret);
	while (--rft >= rfts) {
		if ((fflags & rft->fflags) == rft->fflags)
			kernfs_remove_by_name(kn, rft->name);
	}
	return ret;
}

int resctrl_group_add_files(struct kernfs_node *kn, unsigned long fflags)
{
	int ret = 0;

	if (res_common_files)
		ret = __resctrl_group_add_files(kn, fflags, res_common_files,
						res_common_files_len);

	return ret;
}

/*
 * We don't allow resctrl_group directories to be created anywhere
 * except the root directory. Thus when looking for the resctrl_group
 * structure for a kernfs node we are either looking at a directory,
 * in which case the resctrl_group structure is pointed at by the "priv"
 * field, otherwise we have a file, and need only look to the parent
 * to find the resctrl_group.
 */
static struct resctrl_group *kernfs_to_resctrl_group(struct kernfs_node *kn)
{
	if (kernfs_type(kn) == KERNFS_DIR) {
		/*
		 * All the resource directories use "kn->priv"
		 * to point to the "struct resctrl_group" for the
		 * resource. "info" and its subdirectories don't
		 * have resctrl_group structures, so return NULL here.
		 */
		if (kn == kn_info || kn->parent == kn_info)
			return NULL;
		else
			return kn->priv;
	} else {
		return kn->parent->priv;
	}
}

struct resctrl_group *resctrl_group_kn_lock_live(struct kernfs_node *kn)
{
	struct resctrl_group *rdtgrp = kernfs_to_resctrl_group(kn);

	if (!rdtgrp)
		return NULL;

	atomic_inc(&rdtgrp->waitcount);
	kernfs_break_active_protection(kn);

	mutex_lock(&resctrl_group_mutex);

	/* Was this group deleted while we waited? */
	if (rdtgrp->flags & RDT_DELETED)
		return NULL;

	return rdtgrp;
}

void resctrl_group_kn_unlock(struct kernfs_node *kn)
{
	struct resctrl_group *rdtgrp = kernfs_to_resctrl_group(kn);

	if (!rdtgrp)
		return;

	mutex_unlock(&resctrl_group_mutex);

	if (atomic_dec_and_test(&rdtgrp->waitcount) &&
	    (rdtgrp->flags & RDT_DELETED)) {
		kernfs_unbreak_active_protection(kn);
		kernfs_put(rdtgrp->kn);
		kfree(rdtgrp);
	} else {
		kernfs_unbreak_active_protection(kn);
	}
}

static int
mongroup_create_dir(struct kernfs_node *parent_kn, struct resctrl_group *prgrp,
		    char *name, struct kernfs_node **dest_kn)
{
	struct kernfs_node *kn;
	int ret;

	/* create the directory */
	kn = kernfs_create_dir(parent_kn, name, parent_kn->mode, prgrp);
	if (IS_ERR(kn)) {
		return PTR_ERR(kn);
	}

	if (dest_kn)
		*dest_kn = kn;

	/*
	 * This extra ref will be put in kernfs_remove() and guarantees
	 * that @rdtgrp->kn is always accessible.
	 */
	kernfs_get(kn);

	ret = resctrl_group_kn_set_ugid(kn);
	if (ret)
		goto out_destroy;

	kernfs_activate(kn);

	return 0;

out_destroy:
	kernfs_remove(kn);
	return ret;
}

static void mkdir_mondata_all_prepare_clean(struct resctrl_group *prgrp)
{
	if (prgrp->type == RDTCTRL_GROUP && prgrp->closid.intpartid)
		closid_free(prgrp->closid.intpartid);
	rmid_free(prgrp->mon.rmid);
}

static int mkdir_mondata_all_prepare(struct resctrl_group *rdtgrp)
{
	struct resctrl_group *prgrp;

	if (rdtgrp->type == RDTMON_GROUP) {
		prgrp = rdtgrp->mon.parent;
		rdtgrp->closid.intpartid = prgrp->closid.intpartid;
	}

	return 0;
}

/*
 * This creates a directory mon_data which contains the monitored data.
 *
 * mon_data has one directory for each domain whic are named
 * in the format mon_<domain_name>_<domain_id>. For ex: A mon_data
 * with L3 domain looks as below:
 * ./mon_data:
 * mon_L3_00
 * mon_L3_01
 * mon_L3_02
 * ...
 *
 * Each domain directory has one file per event:
 * ./mon_L3_00/:
 * llc_occupancy
 *
 */
static int mkdir_mondata_all(struct kernfs_node *parent_kn,
			     struct resctrl_group *prgrp,
			     struct kernfs_node **dest_kn)
{
	struct kernfs_node *kn;
	int ret;

	/*
	 * Create the mon_data directory first.
	 */
	ret = mongroup_create_dir(parent_kn, prgrp, "mon_data", &kn);
	if (ret)
		return ret;

	if (dest_kn)
		*dest_kn = kn;

	ret = resctrl_mkdir_mondata_all_subdir(kn, prgrp);
	if (ret)
		goto out_destroy;

	kernfs_activate(kn);

	return 0;

out_destroy:
	kernfs_remove(kn);
	return ret;
}

static struct dentry *resctrl_mount(struct file_system_type *fs_type,
				int flags, const char *unused_dev_name,
				void *data)
{
	struct dentry *dentry;
	int ret;

	cpus_read_lock();
	mutex_lock(&resctrl_group_mutex);
	/*
	 * resctrl file system can only be mounted once.
	 */
	if (static_branch_unlikely(&resctrl_enable_key)) {
		dentry = ERR_PTR(-EBUSY);
		goto out;
	}

	ret = parse_resctrl_group_fs_options(data);
	if (ret) {
		dentry = ERR_PTR(ret);
		goto out_options;
	}
	ret = schemata_list_init();
	if (ret) {
		dentry = ERR_PTR(ret);
		goto out_options;
	}
	ret = resctrl_id_init();
	if (ret) {
		dentry = ERR_PTR(ret);
		goto out_schema;
	}

	ret = resctrl_group_init_alloc(&resctrl_group_default);
	if (ret < 0) {
		dentry = ERR_PTR(ret);
		goto out_schema;
	}

	ret = resctrl_group_create_info_dir(resctrl_group_default.kn, &kn_info);
	if (ret) {
		dentry = ERR_PTR(ret);
		goto out_schema;
	}

	if (resctrl_mon_capable) {
		ret = mongroup_create_dir(resctrl_group_default.kn,
					  NULL, "mon_groups",
					  &kn_mongrp);
		if (ret) {
			dentry = ERR_PTR(ret);
			goto out_info;
		}
		kernfs_get(kn_mongrp);

		ret = mkdir_mondata_all_prepare(&resctrl_group_default);
		if (ret < 0) {
			dentry = ERR_PTR(ret);
			goto out_mongrp;
		}
		ret = mkdir_mondata_all(resctrl_group_default.kn,
					&resctrl_group_default, &kn_mondata);
		if (ret) {
			dentry = ERR_PTR(ret);
			goto out_mongrp;
		}
		kernfs_get(kn_mondata);
		resctrl_group_default.mon.mon_data_kn = kn_mondata;
	}

	dentry = kernfs_mount(fs_type, flags, resctrl_root,
			      RDTGROUP_SUPER_MAGIC, NULL);
	if (IS_ERR(dentry))
		goto out_mondata;

	resctrl_cdp_update_cpus_state(&resctrl_group_default);

	post_resctrl_mount();

	goto out;

out_mondata:
	if (resctrl_mon_capable)
		kernfs_remove(kn_mondata);
out_mongrp:
	if (resctrl_mon_capable)
		kernfs_remove(kn_mongrp);
out_info:
	kernfs_remove(kn_info);
out_schema:
	schemata_list_destroy();
out_options:
	release_resctrl_group_fs_options();
out:
	rdt_last_cmd_clear();
	mutex_unlock(&resctrl_group_mutex);
	cpus_read_unlock();

	return dentry;
}

static inline bool
is_task_match_resctrl_group(struct task_struct *t, struct resctrl_group *r)
{
	return (t->closid == r->closid.intpartid);
}

/*
 * Move tasks from one to the other group. If @from is NULL, then all tasks
 * in the systems are moved unconditionally (used for teardown).
 *
 * If @mask is not NULL the cpus on which moved tasks are running are set
 * in that mask so the update smp function call is restricted to affected
 * cpus.
 */
static void resctrl_move_group_tasks(struct resctrl_group *from, struct resctrl_group *to,
				 struct cpumask *mask)
{
	struct task_struct *p, *t;

	read_lock(&tasklist_lock);
	for_each_process_thread(p, t) {
		if (!from || is_task_match_resctrl_group(t, from)) {
			t->closid = resctrl_navie_closid(to->closid);
			t->rmid = resctrl_navie_rmid(to->mon.rmid);

#ifdef CONFIG_SMP
			/*
			 * This is safe on x86 w/o barriers as the ordering
			 * of writing to task_cpu() and t->on_cpu is
			 * reverse to the reading here. The detection is
			 * inaccurate as tasks might move or schedule
			 * before the smp function call takes place. In
			 * such a case the function call is pointless, but
			 * there is no other side effect.
			 */
			if (mask && t->on_cpu)
				cpumask_set_cpu(task_cpu(t), mask);
#endif
		}
	}
	read_unlock(&tasklist_lock);
}

static void free_all_child_rdtgrp(struct resctrl_group *rdtgrp)
{
	struct resctrl_group *sentry, *stmp;
	struct list_head *head;

	head = &rdtgrp->mon.crdtgrp_list;
	list_for_each_entry_safe(sentry, stmp, head, mon.crdtgrp_list) {
		/* rmid may not be used */
		rmid_free(sentry->mon.rmid);
		list_del(&sentry->mon.crdtgrp_list);
		kfree(sentry);
	}
}

/*
 * Forcibly remove all of subdirectories under root.
 */
static void rmdir_all_sub(void)
{
	struct resctrl_group *rdtgrp, *tmp;

	/* Move all tasks to the default resource group */
	resctrl_move_group_tasks(NULL, &resctrl_group_default, NULL);

	list_for_each_entry_safe(rdtgrp, tmp, &resctrl_all_groups, resctrl_group_list) {
		/* Free any child rmids */
		free_all_child_rdtgrp(rdtgrp);

		/* Remove each resctrl_group other than root */
		if (rdtgrp == &resctrl_group_default)
			continue;

		/*
		 * Give any CPUs back to the default group. We cannot copy
		 * cpu_online_mask because a CPU might have executed the
		 * offline callback already, but is still marked online.
		 */
		cpumask_or(&resctrl_group_default.cpu_mask,
			   &resctrl_group_default.cpu_mask, &rdtgrp->cpu_mask);

		rmid_free(rdtgrp->mon.rmid);

		kernfs_remove(rdtgrp->kn);
		list_del(&rdtgrp->resctrl_group_list);
		kfree(rdtgrp);
	}
	/* Notify online CPUs to update per cpu storage and PQR_ASSOC MSR */
	update_closid_rmid(cpu_online_mask, &resctrl_group_default);

	kernfs_remove(kn_info);
	kernfs_remove(kn_mongrp);
	kernfs_remove(kn_mondata);
}

static void resctrl_kill_sb(struct super_block *sb)
{

	cpus_read_lock();
	mutex_lock(&resctrl_group_mutex);

	resctrl_resource_reset();

	schemata_list_destroy();

	rmdir_all_sub();
	static_branch_disable_cpuslocked(&resctrl_alloc_enable_key);
	static_branch_disable_cpuslocked(&resctrl_mon_enable_key);
	static_branch_disable_cpuslocked(&resctrl_enable_key);
	kernfs_kill_sb(sb);
	mutex_unlock(&resctrl_group_mutex);
	cpus_read_unlock();
}

static struct file_system_type resctrl_fs_type = {
	.name    = "resctrl",
	.mount   = resctrl_mount,
	.kill_sb = resctrl_kill_sb,
};

static int find_rdtgrp_allocable_rmid(struct resctrl_group *rdtgrp)
{
	int ret, rmid, reqpartid;
	struct resctrl_group *prgrp, *entry;
	struct list_head *head;

	prgrp = rdtgrp->mon.parent;
	if (prgrp == &resctrl_group_default) {
		rmid = rmid_alloc(-1);
		if (rmid < 0)
			return rmid;
	} else {
		do {
			rmid = rmid_alloc(prgrp->closid.reqpartid);
			if (rmid >= 0)
				break;

			head = &prgrp->mon.crdtgrp_list;
			list_for_each_entry(entry, head, mon.crdtgrp_list) {
				if (entry == rdtgrp)
					continue;
				rmid = rmid_alloc(entry->closid.reqpartid);
				if (rmid >= 0)
					break;
			}
		} while (0);
	}

	if (rmid < 0)
		rmid = rmid_alloc(-1);

	ret = mpam_rmid_to_partid_pmg(rmid, &reqpartid, NULL);
	if (ret)
		return ret;
	rdtgrp->mon.rmid = rmid;
	rdtgrp->closid.reqpartid = reqpartid;

	return rmid;
}

static int mkdir_resctrl_prepare(struct kernfs_node *parent_kn,
			     struct kernfs_node *prgrp_kn,
			     const char *name, umode_t mode,
			     enum rdt_group_type rtype, struct resctrl_group **r)
{
	struct resctrl_group *prdtgrp, *rdtgrp;
	struct kernfs_node *kn;
	uint files = 0;
	int ret;

	prdtgrp = resctrl_group_kn_lock_live(prgrp_kn);
	rdt_last_cmd_clear();
	if (!prdtgrp) {
		ret = -ENODEV;
		rdt_last_cmd_puts("directory was removed\n");
		goto out_unlock;
	}

	/* allocate the resctrl_group. */
	rdtgrp = kzalloc(sizeof(*rdtgrp), GFP_KERNEL);
	if (!rdtgrp) {
		ret = -ENOSPC;
		rdt_last_cmd_puts("kernel out of memory\n");
		goto out_unlock;
	}
	*r = rdtgrp;
	rdtgrp->mon.parent = prdtgrp;
	rdtgrp->type = rtype;

	/*
	 * for ctrlmon group, intpartid is used for
	 * applying configuration, reqpartid is
	 * used for following this configuration and
	 * getting monitoring for child mon groups.
	 */
	if (rdtgrp->type == RDTCTRL_GROUP) {
		ret = closid_alloc();
		if (ret < 0) {
			rdt_last_cmd_puts("out of CLOSIDs\n");
			goto out_free_rdtgrp;
		}
		rdtgrp->closid.intpartid = ret;
	}

	ret = find_rdtgrp_allocable_rmid(rdtgrp);
	if (ret < 0) {
		rdt_last_cmd_puts("out of RMIDs\n");
		goto out_free_closid;
	}
	rdtgrp->mon.rmid = ret;

	INIT_LIST_HEAD(&rdtgrp->mon.crdtgrp_list);

	/* kernfs creates the directory for rdtgrp */
	kn = kernfs_create_dir(parent_kn, name, mode, rdtgrp);
	if (IS_ERR(kn)) {
		ret = PTR_ERR(kn);
		rdt_last_cmd_puts("kernfs create error\n");
		goto out_free_rmid;
	}
	rdtgrp->kn = kn;

	/*
	 * kernfs_remove() will drop the reference count on "kn" which
	 * will free it. But we still need it to stick around for the
	 * resctrl_group_kn_unlock(kn} call below. Take one extra reference
	 * here, which will be dropped inside resctrl_group_kn_unlock().
	 */
	kernfs_get(kn);

	ret = resctrl_group_kn_set_ugid(kn);
	if (ret) {
		rdt_last_cmd_puts("kernfs perm error\n");
		goto out_destroy;
	}

	files = RFTYPE_BASE | BIT(RF_CTRLSHIFT + rtype);
	ret = resctrl_group_add_files(kn, files);
	if (ret) {
		rdt_last_cmd_puts("kernfs fill error\n");
		goto out_destroy;
	}

	if (resctrl_mon_capable) {
		ret = mkdir_mondata_all_prepare(rdtgrp);
		if (ret < 0) {
			goto out_destroy;
		}

		ret = mkdir_mondata_all(kn, rdtgrp, &rdtgrp->mon.mon_data_kn);
		if (ret) {
			rdt_last_cmd_puts("kernfs subdir error\n");
			goto out_prepare_clean;
		}
	}

	kernfs_activate(kn);

	/*
	 * The caller unlocks the prgrp_kn upon success.
	 */
	return 0;

out_prepare_clean:
	mkdir_mondata_all_prepare_clean(rdtgrp);
out_destroy:
	kernfs_remove(rdtgrp->kn);
out_free_rmid:
	rmid_free(rdtgrp->mon.rmid);
out_free_closid:
	if (rdtgrp->type == RDTCTRL_GROUP)
		closid_free(rdtgrp->closid.intpartid);
out_free_rdtgrp:
	kfree(rdtgrp);
out_unlock:
	resctrl_group_kn_unlock(prgrp_kn);
	return ret;
}

static void mkdir_resctrl_prepare_clean(struct resctrl_group *rgrp)
{
	kernfs_remove(rgrp->kn);
	kfree(rgrp);
}

/*
 * Create a monitor group under "mon_groups" directory of a control
 * and monitor group(ctrl_mon). This is a resource group
 * to monitor a subset of tasks and cpus in its parent ctrl_mon group.
 */
static int resctrl_group_mkdir_mon(struct kernfs_node *parent_kn,
			      struct kernfs_node *prgrp_kn,
			      const char *name,
			      umode_t mode)
{
	struct resctrl_group *rdtgrp, *prgrp;
	int ret;

	ret = mkdir_resctrl_prepare(parent_kn, prgrp_kn, name, mode, RDTMON_GROUP,
				&rdtgrp);
	if (ret)
		return ret;

	prgrp = rdtgrp->mon.parent;
	/*
	 * Add the rdtgrp to the list of rdtgrps the parent
	 * ctrl_mon group has to track.
	 */
	list_add_tail(&rdtgrp->mon.crdtgrp_list, &prgrp->mon.crdtgrp_list);

	/*
	 * update all mon group's configuration under this parent group
	 * for master-slave model.
	 */
	ret = resctrl_update_groups_config(prgrp);

	resctrl_group_kn_unlock(prgrp_kn);
	return ret;
}

/*
 * These are resctrl_groups created under the root directory. Can be used
 * to allocate and monitor resources.
 */
static int resctrl_group_mkdir_ctrl_mon(struct kernfs_node *parent_kn,
				   struct kernfs_node *prgrp_kn,
				   const char *name, umode_t mode)
{
	struct resctrl_group *rdtgrp;
	struct kernfs_node *kn;
	int ret;

	ret = mkdir_resctrl_prepare(parent_kn, prgrp_kn, name, mode, RDTCTRL_GROUP,
				&rdtgrp);
	if (ret)
		return ret;

	kn = rdtgrp->kn;

	ret = resctrl_group_init_alloc(rdtgrp);
	if (ret < 0)
		goto out_common_fail;

	list_add(&rdtgrp->resctrl_group_list, &resctrl_all_groups);

	if (resctrl_mon_capable) {
		/*
		 * Create an empty mon_groups directory to hold the subset
		 * of tasks and cpus to monitor.
		 */
		ret = mongroup_create_dir(kn, NULL, "mon_groups", NULL);
		if (ret) {
			rdt_last_cmd_puts("kernfs subdir error\n");
			goto out_list_del;
		}
	}

	goto out_unlock;

out_list_del:
	list_del(&rdtgrp->resctrl_group_list);
out_common_fail:
	mkdir_resctrl_prepare_clean(rdtgrp);
out_unlock:
	resctrl_group_kn_unlock(prgrp_kn);
	return ret;
}

/*
 * We allow creating mon groups only with in a directory called "mon_groups"
 * which is present in every ctrl_mon group. Check if this is a valid
 * "mon_groups" directory.
 *
 * 1. The directory should be named "mon_groups".
 * 2. The mon group itself should "not" be named "mon_groups".
 *   This makes sure "mon_groups" directory always has a ctrl_mon group
 *   as parent.
 */
static bool is_mon_groups(struct kernfs_node *kn, const char *name)
{
	return (!strcmp(kn->name, "mon_groups") &&
		strcmp(name, "mon_groups"));
}

static int resctrl_group_mkdir(struct kernfs_node *parent_kn, const char *name,
			  umode_t mode)
{
	/* Do not accept '\n' to avoid unparsable situation. */
	if (strchr(name, '\n'))
		return -EINVAL;

	/*
	 * If the parent directory is the root directory and RDT
	 * allocation is supported, add a control and monitoring
	 * subdirectory
	 */
	if (resctrl_alloc_capable && parent_kn == resctrl_group_default.kn)
		return resctrl_group_mkdir_ctrl_mon(parent_kn, parent_kn, name, mode);

	/*
	 * If RDT monitoring is supported and the parent directory is a valid
	 * "mon_groups" directory, add a monitoring subdirectory.
	 */
	if (resctrl_mon_capable && is_mon_groups(parent_kn, name))
		return resctrl_group_mkdir_mon(parent_kn, parent_kn->parent, name, mode);

	return -EPERM;
}

static void resctrl_group_rm_mon(struct resctrl_group *rdtgrp,
			      cpumask_var_t tmpmask)
{
	struct resctrl_group *prdtgrp = rdtgrp->mon.parent;
	int cpu;

	/* Give any tasks back to the parent group */
	resctrl_move_group_tasks(rdtgrp, prdtgrp, tmpmask);

	/* Update per cpu closid and rmid of the moved CPUs first */
	for_each_cpu(cpu, &rdtgrp->cpu_mask) {
		per_cpu(pqr_state.default_closid, cpu) = resctrl_navie_closid(prdtgrp->closid);
		per_cpu(pqr_state.default_rmid, cpu) = resctrl_navie_rmid(prdtgrp->mon.rmid);
	}

	/*
	 * Update the MSR on moved CPUs and CPUs which have moved
	 * task running on them.
	 */
	cpumask_or(tmpmask, tmpmask, &rdtgrp->cpu_mask);
	update_closid_rmid(tmpmask, NULL);

	rdtgrp->flags |= RDT_DELETED;

	rmid_free(rdtgrp->mon.rmid);

	/*
	 * Remove the rdtgrp from the parent ctrl_mon group's list
	 */
	WARN_ON(list_empty(&prdtgrp->mon.crdtgrp_list));
	list_del(&rdtgrp->mon.crdtgrp_list);
}

static int resctrl_group_rmdir_mon(struct kernfs_node *kn, struct resctrl_group *rdtgrp,
			      cpumask_var_t tmpmask)
{
	resctrl_group_rm_mon(rdtgrp, tmpmask);

	/*
	 * one extra hold on this, will drop when we kfree(rdtgrp)
	 * in resctrl_group_kn_unlock()
	 */
	kernfs_get(kn);
	kernfs_remove(rdtgrp->kn);

	return 0;
}

static void resctrl_group_rm_ctrl(struct resctrl_group *rdtgrp, cpumask_var_t tmpmask)
{
	int cpu;

	/* Give any tasks back to the default group */
	resctrl_move_group_tasks(rdtgrp, &resctrl_group_default, tmpmask);

	/* Give any CPUs back to the default group */
	cpumask_or(&resctrl_group_default.cpu_mask,
		   &resctrl_group_default.cpu_mask, &rdtgrp->cpu_mask);

	/* Update per cpu closid and rmid of the moved CPUs first */
	for_each_cpu(cpu, &rdtgrp->cpu_mask) {
		per_cpu(pqr_state.default_closid, cpu) =
			resctrl_navie_closid(resctrl_group_default.closid);
		per_cpu(pqr_state.default_rmid, cpu) =
			resctrl_navie_rmid(resctrl_group_default.mon.rmid);
	}

	/*
	 * Update the MSR on moved CPUs and CPUs which have moved
	 * task running on them.
	 */
	cpumask_or(tmpmask, tmpmask, &rdtgrp->cpu_mask);
	update_closid_rmid(tmpmask, NULL);

	rdtgrp->flags |= RDT_DELETED;
	closid_free(rdtgrp->closid.intpartid);
	rmid_free(rdtgrp->mon.rmid);

	/*
	 * Free all the child monitor group rmids.
	 */
	free_all_child_rdtgrp(rdtgrp);

	list_del(&rdtgrp->resctrl_group_list);
}

static int resctrl_group_rmdir_ctrl(struct kernfs_node *kn, struct resctrl_group *rdtgrp,
			       cpumask_var_t tmpmask)
{
	resctrl_group_rm_ctrl(rdtgrp, tmpmask);

	/*
	 * one extra hold on this, will drop when we kfree(rdtgrp)
	 * in resctrl_group_kn_unlock()
	 */
	kernfs_get(kn);
	kernfs_remove(rdtgrp->kn);

	return 0;
}

static int resctrl_group_rmdir(struct kernfs_node *kn)
{
	struct kernfs_node *parent_kn = kn->parent;
	struct resctrl_group *rdtgrp;
	cpumask_var_t tmpmask;
	int ret = 0;

	if (!zalloc_cpumask_var(&tmpmask, GFP_KERNEL))
		return -ENOMEM;

	rdtgrp = resctrl_group_kn_lock_live(kn);
	if (!rdtgrp) {
		ret = -EPERM;
		goto out;
	}

	/*
	 * If the resctrl_group is a ctrl_mon group and parent directory
	 * is the root directory, remove the ctrl_mon group.
	 *
	 * If the resctrl_group is a mon group and parent directory
	 * is a valid "mon_groups" directory, remove the mon group.
	 */
	if (rdtgrp->type == RDTCTRL_GROUP && parent_kn == resctrl_group_default.kn)
		ret = resctrl_group_rmdir_ctrl(kn, rdtgrp, tmpmask);
	else if (rdtgrp->type == RDTMON_GROUP &&
		 is_mon_groups(parent_kn, kn->name))
		ret = resctrl_group_rmdir_mon(kn, rdtgrp, tmpmask);
	else
		ret = -EPERM;

out:
	resctrl_group_kn_unlock(kn);
	free_cpumask_var(tmpmask);
	return ret;
}

static int resctrl_group_show_options(struct seq_file *seq, struct kernfs_root *kf)
{
	return __resctrl_group_show_options(seq);
}

static struct kernfs_syscall_ops resctrl_group_kf_syscall_ops = {
	.mkdir		= resctrl_group_mkdir,
	.rmdir		= resctrl_group_rmdir,
	.show_options	= resctrl_group_show_options,
};

static void resctrl_group_default_init(struct resctrl_group *r)
{
	r->closid.intpartid = 0;
	r->closid.reqpartid = 0;
	r->mon.rmid = 0;
	r->type = RDTCTRL_GROUP;
}

static int __init resctrl_group_setup_root(void)
{
	int ret;

	resctrl_root = kernfs_create_root(&resctrl_group_kf_syscall_ops,
				      KERNFS_ROOT_CREATE_DEACTIVATED,
				      &resctrl_group_default);
	if (IS_ERR(resctrl_root))
		return PTR_ERR(resctrl_root);

	mutex_lock(&resctrl_group_mutex);

	resctrl_group_default_init(&resctrl_group_default);
	INIT_LIST_HEAD(&resctrl_group_default.mon.crdtgrp_list);

	list_add(&resctrl_group_default.resctrl_group_list, &resctrl_all_groups);

	ret = resctrl_group_add_files(resctrl_root->kn, RF_CTRL_BASE);
	if (ret) {
		kernfs_destroy_root(resctrl_root);
		goto out;
	}

	resctrl_group_default.kn = resctrl_root->kn;
	kernfs_activate(resctrl_group_default.kn);

out:
	mutex_unlock(&resctrl_group_mutex);

	return ret;
}

/*
 * resctrl_group_init - resctrl_group initialization
 *
 * Setup resctrl file system including set up root, create mount point,
 * register resctrl_group filesystem, and initialize files under root directory.
 *
 * Return: 0 on success or -errno
 */
int resctrl_group_init(void)
{
	int ret = 0;

	ret = resctrl_group_setup_root();
	if (ret)
		return ret;

	ret = sysfs_create_mount_point(fs_kobj, "resctrl");
	if (ret)
		goto cleanup_root;

	ret = register_filesystem(&resctrl_fs_type);
	if (ret)
		goto cleanup_mountpoint;

	return 0;

cleanup_mountpoint:
	sysfs_remove_mount_point(fs_kobj, "resctrl");
cleanup_root:
	kernfs_destroy_root(resctrl_root);

	return ret;
}
