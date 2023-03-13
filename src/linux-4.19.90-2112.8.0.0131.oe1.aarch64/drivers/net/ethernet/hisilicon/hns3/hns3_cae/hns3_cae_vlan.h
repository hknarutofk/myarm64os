/* SPDX-License-Identifier: GPL-2.0+ */
/* Copyright (c) 2016-2019 Hisilicon Limited. */

#ifndef __HNS3_CAE_VLAN_H__
#define __HNS3_CAE_VLAN_H__

#define HNS3_CAE_VLANUP_MODULE_FLAG		0x01
#define HNS3_CAE_VLANUP_PF_CFG_FLAG		0x02
#define HNS3_CAE_VLANUP_VF_CFG_FLAG		0x04
#define HNS3_CAE_VLANUP_TC_CFG_FLAG		0x08
#define HNS3_CAE_VLANUP_TI2OUPM_FLAG		0x10
#define HNS3_CAE_VLANUP_TV2PUPM_FLAG		0x20
#define HNS3_CAE_VLANUP_TP2NUPM_FLAG		0x40
#define HNS3_CAE_VLANUP_CTRL_CFG_FLAG		0x80

#define HNS3_CAE_TAGEN_MASK	0x3
#define HNS3_CAE_TCID_MASK	0x7
#define HNS3_CAE_PFID_MASK	0x7
#define HNS3_CAE_VFID_MASK	0x7F8
#define HNS3_CAE_PFVLD_MASK	0x1000
#define HNS3_CAE_MODULE_MASK	0x1

struct hns3_cae_vlanup_param {
	u8 is_read;
	u32 ti2oupm;
	u32 tv2pupm;
	u32 tp2nupm;
	u32 vf_id;
	u32 map_flag;
	u8 pf_valid;
	u8 pf_id;
	u8 tc_id;
	u8 tag_en;
	u8 module;
};

int hns3_cae_upmapping_cfg(const struct hns3_nic_priv *net_priv,
			   void *buf_in, u32 in_size,
			   void *buf_out, u32 out_size);

#endif
