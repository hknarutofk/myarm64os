// SPDX-License-Identifier: GPL-2.0+
// Copyright (c) 2016-2017 Hisilicon Limited.

#include <linux/acpi.h>
#include <linux/device.h>
#include <linux/etherdevice.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/pci.h>
#include <linux/platform_device.h>
#include <linux/if_vlan.h>
#include <net/rtnetlink.h>
#include "kcompat.h"
#include "hclge_cmd.h"
#include "hclge_main.h"
#include "hnae3.h"
#include "hclge_main_it.h"
#ifdef CONFIG_HNS3_TEST
#include "hclge_sysfs.h"
#endif

#ifdef CONFIG_IT_VALIDATION
#define HCLGE_RESET_MAX_FAIL_CNT	1

static nic_event_fn_t nic_event_call;

int nic_register_event(nic_event_fn_t event_call)
{
	if (!event_call) {
		pr_err("register event handle is null.\n");
		return -EINVAL;
	}

	nic_event_call = event_call;

	pr_info("netdev register success.\n");
	return 0;
}
EXPORT_SYMBOL(nic_register_event);

int nic_unregister_event(void)
{
	nic_event_call = NULL;
	return 0;
}
EXPORT_SYMBOL(nic_unregister_event);

static void nic_call_event(struct net_device *netdev,
			   enum hnae3_event_type_custom event_t)
{
	if (nic_event_call) {
		nic_event_call(netdev, event_t);
		netdev_info(netdev, "report event %d\n", event_t);
	}
}

static void hclge_handle_imp_error_it(struct hnae3_handle *handle)
{
	struct hclge_vport *vport = hclge_get_vport(handle);
	struct hclge_dev *hdev = vport->back;
	struct net_device *netdev;
	u32 reg_val;

	netdev = hdev->vport[0].nic.netdev;

	if (test_and_clear_bit(HCLGE_IMP_RD_POISON, &hdev->imp_err_state)) {
		dev_err(&hdev->pdev->dev, "Detected IMP RD poison!\n");
		if (nic_event_call)
			nic_call_event(netdev, HNAE3_IMP_RD_POISON_CUSTOM);
		reg_val = hclge_read_dev(&hdev->hw, HCLGE_PF_OTHER_INT_REG) &
		    ~BIT(HCLGE_VECTOR0_IMP_RD_POISON_B);
		hclge_write_dev(&hdev->hw, HCLGE_PF_OTHER_INT_REG, reg_val);
	}

	if (test_and_clear_bit(HCLGE_IMP_CMDQ_ERROR, &hdev->imp_err_state)) {
		dev_err(&hdev->pdev->dev, "Detected CMDQ ECC error!\n");
		if (nic_event_call)
			nic_call_event(netdev, HNAE3_IMP_RESET_CUSTOM);
		reg_val = hclge_read_dev(&hdev->hw, HCLGE_PF_OTHER_INT_REG) &
		    ~BIT(HCLGE_VECTOR0_IMP_CMDQ_ERR_B);
		hclge_write_dev(&hdev->hw, HCLGE_PF_OTHER_INT_REG, reg_val);
	}

}

static void hclge_reset_task_schedule_it(struct hclge_dev *hdev)
{
	if (!test_bit(HCLGE_STATE_REMOVING, &hdev->state) &&
	    !test_and_set_bit(HCLGE_STATE_RST_SERVICE_SCHED, &hdev->state))
		mod_delayed_work_on(cpumask_first(&hdev->affinity_mask),
				    system_wq, &hdev->service_task, 0);
}

void hclge_reset_event_it(struct pci_dev *pdev, struct hnae3_handle *handle)
{
	struct hnae3_ae_dev *ae_dev = pci_get_drvdata(pdev);
	struct hclge_dev *hdev = ae_dev->priv;
	struct net_device *netdev;

	netdev = hdev->vport[0].nic.netdev;

	/* We might end up getting called broadly because of 2 below cases:
	 * 1. Recoverable error was conveyed through APEI and only way to bring
	 *    normalcy is to reset.
	 * 2. A new reset request from the stack due to timeout
	 *
	 * For the first case,error event might not have ae handle available.
	 * check if this is a new reset request and we are not here just because
	 * last reset attempt did not succeed and watchdog hit us again. We will
	 * know this if last reset request did not occur very recently (watchdog
	 * timer = 5*HZ, let us check after sufficiently large time, say 4*5*Hz)
	 * In case of new request we reset the "reset level" to PF reset.
	 * And if it is a repeat reset request of the most recent one then we
	 * want to make sure we throttle the reset request. Therefore, we will
	 * not allow it again before 12*HZ times.
	 */
	if (time_before(jiffies, (hdev->last_reset_time +
				  HCLGE_RESET_INTERVAL))) {
		mod_timer(&hdev->reset_timer, jiffies + HCLGE_RESET_INTERVAL);
		return;
	} else if (hdev->default_reset_request) {
		hdev->reset_level =
		    hclge_get_reset_level(ae_dev, &hdev->default_reset_request);
	} else if (time_after(jiffies, (hdev->last_reset_time + 4 * 5 * HZ))) {
		hdev->reset_level = HNAE3_FUNC_RESET;
	}

	dev_info(&hdev->pdev->dev, "IT received reset event, reset type is %d",
		 hdev->reset_level);

	if (hdev->ppu_poison_ras_err && nic_event_call) {
		nic_call_event(netdev, HNAE3_PPU_POISON_CUSTOM);
		hdev->ppu_poison_ras_err = false;
	}

	if (nic_event_call) {
		nic_call_event(netdev, hdev->reset_level);
	} else {
		/* request reset & schedule reset task */
		set_bit(hdev->reset_level, &hdev->reset_request);
		hclge_reset_task_schedule_it(hdev);
	}
}

bool hclge_reset_end_it(struct hnae3_handle *handle, bool done)
{
	struct hclge_vport *vport = hclge_get_vport(handle);
	struct hclge_dev *hdev = vport->back;
	struct net_device *netdev;

	netdev = hdev->vport[0].nic.netdev;

	if (done) {
		dev_info(&hdev->pdev->dev, "IT Report Reset DONE!\n");
		if (nic_event_call)
			nic_call_event(netdev, HNAE3_RESET_DONE_CUSTOM);
	}

	if (hdev->reset_fail_cnt >= HCLGE_RESET_MAX_FAIL_CNT) {
		dev_err(&hdev->pdev->dev, "IT Report Reset fail!\n");
		if (nic_event_call) {
			if (hdev->reset_type == HNAE3_FUNC_RESET)
				nic_call_event(netdev,
					       HNAE3_FUNC_RESET_FAIL_CUSTOM);
			else if (hdev->reset_type == HNAE3_GLOBAL_RESET)
				nic_call_event(netdev,
					       HNAE3_GLOBAL_RESET_FAIL_CUSTOM);
			else if (hdev->reset_type == HNAE3_IMP_RESET)
				nic_call_event(netdev,
					       HNAE3_IMP_RESET_FAIL_CUSTOM);
		}
	}

	return done;
}

#ifdef CONFIG_HNS3_TEST
void hclge_ext_init(struct hnae3_handle *handle)
{
	hclge_sysfs_init(handle);
}

void hclge_ext_uninit(struct hnae3_handle *handle)
{
	struct hclge_vport *vport = hclge_get_vport(handle);
	struct hclge_dev *hdev = vport->back;

	hclge_reset_pf_rate(hdev);
	hclge_sysfs_uninit(handle);
}

void hclge_ext_reset_done(struct hnae3_handle *handle)
{
	struct hclge_vport *vport = hclge_get_vport(handle);
	struct hclge_dev *hdev = vport->back;

	hclge_resume_pf_rate(hdev);
}
#endif

int hclge_init_it(void)
{
#ifdef CONFIG_HNS3_TEST
	hclge_ops.ext_init = hclge_ext_init;
	hclge_ops.ext_uninit = hclge_ext_uninit;
	hclge_ops.ext_reset_done = hclge_ext_reset_done;
#endif

	hclge_ops.reset_event = hclge_reset_event_it;
	hclge_ops.reset_end = hclge_reset_end_it;
	hclge_ops.handle_imp_error = hclge_handle_imp_error_it;

	return hclge_init();
}

module_init(hclge_init_it);
#endif
