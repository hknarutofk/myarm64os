/* SPDX-License-Identifier: GPL-2.0+ */
/* Copyright (c) 2016-2019 Hisilicon Limited. */

#ifndef __HNS3_CAE_DFX_H
#define __HNS3_CAE_DFX_H

#define OPC_WRITE_READ_REG_CMD	0x7014

struct hns3_cae_reg_param {
	u8 is_read;
	u8 bit_width;
	u64 value;
	u64 addr;
};

struct hns3_cae_dfx_param {
	u8 is_cs_board;
	u8 work_mode;
	u8 mac_used;
	u8 chip_id;
	u8 mac_id;
	u8 func_id;
};

#define HNS3_CAE_EVENT_NAME_LEN	32

struct hns3_cae_event_param {
	u8 event_name[HNS3_CAE_EVENT_NAME_LEN];
	u64 value;
	u64 addr;
};

#define HNS3_READ_INFO_FLAG		0x1
#define HNS3_READ_REGS_FLAG		0x2

#define HNS3_CAE_MAX_MAC_NUMBER	0x8

#define HNS3_CAE_BITWIDTH_32BIT		32
#define HNS3_CAE_BITWIDTH_64BIT		64

int hns3_cae_get_dfx_info(const struct hns3_nic_priv *net_priv,
			  void *buf_in, u32 in_size,
			  void *buf_out, u32 out_size);
int hns3_cae_read_dfx_info(const struct hns3_nic_priv *net_priv,
			   void *buf_in, u32 in_size,
			   void *buf_out, u32 out_size);
int hns3_cae_event_injection(const struct hns3_nic_priv *net_priv,
			     void *buf_in, u32 in_size,
			     void *buf_out, u32 out_size);

#endif
