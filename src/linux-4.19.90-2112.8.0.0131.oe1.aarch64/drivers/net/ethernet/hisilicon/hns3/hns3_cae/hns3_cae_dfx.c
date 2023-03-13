// SPDX-License-Identifier: GPL-2.0+
// Copyright (c) 2016-2017 Hisilicon Limited.

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/netdevice.h>
#include <linux/phy_fixed.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include "hclge_cmd.h"
#include "hnae3.h"
#include "hclge_main.h"
#include "hns3_enet.h"
#include "hns3_cae_cmd.h"
#include "hns3_cae_dfx.h"

static int hns3_cae_operate_nic_regs(struct hclge_dev *hdev,
				     struct hns3_cae_reg_param *info)
{
	struct hclge_desc desc;
	int ret;

	if (info->is_read) {
		hns3_cae_cmd_setup_basic_desc(&desc,
					      OPC_WRITE_READ_REG_CMD, true);
		desc.data[0] = (u32)(info->addr & 0xffffffff);
		desc.data[1] = (u32)(info->addr >> 32);
		desc.data[4] = (u32)info->bit_width;
		ret = hns3_cae_cmd_send(hdev, &desc, 1);
		if (ret) {
			dev_err(&hdev->pdev->dev,
				"read addr 0x%llx failed! ret = %d.\n",
				info->addr, ret);
			return ret;
		}
		info->value = (u64)desc.data[2] | ((u64)desc.data[3] << 32);
	} else {
		hns3_cae_cmd_setup_basic_desc(&desc, OPC_WRITE_READ_REG_CMD,
					      false);
		desc.data[0] = (u32)(info->addr & 0xffffffff);
		desc.data[1] = (u32)(info->addr >> 32);
		desc.data[2] = (u32)(info->value & 0xffffffff);
		desc.data[3] = (u32)(info->value >> 32);
		desc.data[4] = (u32)info->bit_width;
		ret = hns3_cae_cmd_send(hdev, &desc, 1);
		if (ret) {
			dev_err(&hdev->pdev->dev,
				"write addr 0x%llx value 0x%llx failed! ret = %d.\n",
				info->addr, info->value, ret);
			return ret;
		}
	}

	return 0;
}

static int hns3_cae_get_chip_and_mac_id(struct hnae3_handle *handle,
					u32 *chip_id, u32 *mac_id)
{
#define HNS3_CAE_GET_CHIP_MAC_ID_CMD	0x7003
	struct hclge_vport *vport = hns3_cae_get_vport(handle);
	struct hclge_dev *hdev = vport->back;
	struct hclge_desc desc;
	int ret;

	hns3_cae_cmd_setup_basic_desc(&desc,
				      HNS3_CAE_GET_CHIP_MAC_ID_CMD, true);
	ret = hns3_cae_cmd_send(hdev, &desc, 1);
	if (ret) {
		dev_err(&hdev->pdev->dev, "get chip id and mac id failed %d\n",
			ret);
		return ret;
	}
	*chip_id = desc.data[0];
	*mac_id = desc.data[1];

	return 0;
}

int hns3_cae_get_dfx_info(const struct hns3_nic_priv *net_priv,
			  void *buf_in, u32 in_size,
			  void *buf_out, u32 out_size)
{
#define HNS3_CAE_MAC_MODE_ADDR		0x10000000U
#define HNS3_CAE_MAC_MAP_ADDR		0x10000008U
	struct hns3_cae_dfx_param *out_info =
					   (struct hns3_cae_dfx_param *)buf_out;
	struct hns3_cae_reg_param reg_info;
	struct hnae3_handle *handle = NULL;
	struct hclge_vport *vport = NULL;
	struct hclge_dev *hdev = NULL;
	u32 chip_id;
	u32 mac_id;
	bool check = !buf_out || out_size < sizeof(struct hns3_cae_dfx_param);
	int ret;
	u32 i;

	if (check) {
		pr_err("input param buf_out error in %s function\n", __func__);
		return -EFAULT;
	}

	handle = net_priv->ae_handle;
	vport = hns3_cae_get_vport(handle);
	hdev = vport->back;

	ret = hns3_cae_get_chip_and_mac_id(handle, &chip_id, &mac_id);
	if (ret)
		return ret;
	out_info->chip_id = (u8)chip_id;
	out_info->mac_id = (u8)mac_id;
	out_info->func_id = (u8)hdev->pdev->devfn;
	out_info->is_cs_board = (handle->pdev->revision > 0x20) ? true : false;
	reg_info.addr = HNS3_CAE_MAC_MODE_ADDR;
	reg_info.bit_width = HNS3_CAE_BITWIDTH_32BIT;
	reg_info.is_read = true;
	ret = hns3_cae_operate_nic_regs(hdev, &reg_info);
	if (ret) {
		dev_err(&hdev->pdev->dev,
			"read chip%d's work mode failed!\n", chip_id);
		return ret;
	}
	out_info->work_mode = reg_info.value;
	reg_info.addr = HNS3_CAE_MAC_MAP_ADDR;
	reg_info.bit_width = HNS3_CAE_BITWIDTH_64BIT;
	reg_info.is_read = true;
	ret = hns3_cae_operate_nic_regs(hdev, &reg_info);
	if (ret) {
		dev_err(&hdev->pdev->dev, "read mac's map info failed!\n");
		return ret;
	}
	for (i = 0; i < HNS3_CAE_MAX_MAC_NUMBER; i++)
		out_info->mac_used |= ((reg_info.value >> (i * 8)) & 0xff);

	return 0;
}

int hns3_cae_read_dfx_info(const struct hns3_nic_priv *net_priv,
			   void *buf_in, u32 in_size,
			   void *buf_out, u32 out_size)
{
	struct hns3_cae_reg_param *out_info =
					   (struct hns3_cae_reg_param *)buf_out;
	struct hns3_cae_reg_param *in_info =
					    (struct hns3_cae_reg_param *)buf_in;
	struct hclge_vport *vport = NULL;
	struct hclge_dev *hdev = NULL;
	bool check = !buf_in || in_size < sizeof(struct hns3_cae_reg_param);
	int ret;

	if (check) {
		pr_err("input param buf_in error in %s function\n", __func__);
		return -EFAULT;
	}

	vport = hns3_cae_get_vport(net_priv->ae_handle);
	hdev = vport->back;

	if (in_info->is_read) {
		check = !buf_out ||
			out_size < sizeof(struct hns3_cae_reg_param);
		if (check) {
			pr_err("input param buf_out error in %s function\n",
			       __func__);
			return -EFAULT;
		}
		out_info->addr = in_info->addr;
		out_info->is_read = true;
		out_info->bit_width = in_info->bit_width;

		ret = hns3_cae_operate_nic_regs(hdev, out_info);
		if (ret)
			return ret;
	} else {
		ret = hns3_cae_operate_nic_regs(hdev, in_info);
		if (ret)
			return ret;
	}

	return 0;
}

int hns3_cae_event_injection(const struct hns3_nic_priv *net_priv,
			     void *buf_in, u32 in_size,
			     void *buf_out, u32 out_size)
{
	struct hns3_cae_event_param *in_info =
					  (struct hns3_cae_event_param *)buf_in;
	struct hns3_cae_reg_param reg_info;
	struct hclge_vport *vport = NULL;
	struct hclge_dev *hdev = NULL;
	bool check = !buf_in || in_size < sizeof(struct hns3_cae_event_param);
	int ret;

	if (check) {
		pr_err("input param buf_in error in %s function\n", __func__);
		return -EFAULT;
	}

	vport = hns3_cae_get_vport(net_priv->ae_handle);
	hdev = vport->back;

	reg_info.addr = in_info->addr;
	reg_info.bit_width = HNS3_CAE_BITWIDTH_32BIT;
	reg_info.is_read = false;
	reg_info.value = in_info->value;
	dev_info(&hdev->pdev->dev,
		 "Injection event: %s start.\n", in_info->event_name);
	ret = hns3_cae_operate_nic_regs(hdev, &reg_info);
	if (ret) {
		dev_err(&hdev->pdev->dev, "Injection event error!\n");
		return ret;
	}
	dev_info(&hdev->pdev->dev,
		 "Injection event: %s end.\n", in_info->event_name);

	return ret;
}
