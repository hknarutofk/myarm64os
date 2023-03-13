// SPDX-License-Identifier: GPL-2.0+
// Copyright (c) 2016-2017 Hisilicon Limited.

#include <linux/mm.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/io.h>
#include <linux/pci.h>

#define	CHIP_OFFSET		0x200000000000UL
#define	APB_SUBCTRL_BASE	0x148070000UL
#define	NVME_BAR_BASE		0x148800000UL
#define	VIRTIO_BAR_BASE		0x148a00000UL
#define CHIP_MMAP_MASK		0xf
#define TYPE_MMAP_MASK		0xf0
#define SYSCTRL_SC_ECO_RSV1 0x9401ff04
#define PCIE_REG_SIZE 0X390000UL
#define NVME_BAR_SIZE 0x200000UL
#define VIRTIO_BAR_SIZE 0x200000UL
#define MAX_CHIP_NUM 4
#define CHIP_INFO_REG_SIZE 4
#define TYPE_SHIFT 4
#define BIT_SHIFT_8 8
#define PCIE_CMD_GET_CHIPNUMS 0x01
#define HI1620_PCI_VENDOR_ID 0x19e5
#define HI1620_PCI_DEVICE_ID 0xa120
#define	DEVICE_NAME "pcie_reg_dev"

enum chip_type_t {
	CHIP1620 = 0x13,
	CHIP1620s = 0x12,
	CHIP1601 = 0x10,
	CHIPNONE = 0x0,
};

enum {
	MMAP_TYPE_APB,
	MMAP_TYPE_NVME,
	MMAP_TYPE_VIRTIO
};

static u32 current_chip_nums;

static const struct vm_operations_struct mmap_pcie_mem_ops = {
#ifdef CONFIG_HAVE_IOREMAP_PROT
	.access = generic_access_phys
#endif
};

static int pcie_reg_mmap(struct file *filep, struct vm_area_struct *vma)
{
	u64 size = vma->vm_end - vma->vm_start;
	u32 chip_id = (u32)vma->vm_pgoff & CHIP_MMAP_MASK;
	u32 type = ((u32)vma->vm_pgoff & TYPE_MMAP_MASK) >> TYPE_SHIFT;
	u64 phy_addr;

	if (chip_id >= current_chip_nums) {
		pr_err("pcie_cae input chip_id %u is invalid\n", chip_id);
		return -EINVAL;
	}

	/* It's illegal to wrap around the end of the physical address space. */
	switch (type) {
	case MMAP_TYPE_APB:
		phy_addr = APB_SUBCTRL_BASE + CHIP_OFFSET * chip_id;
		if (size > PCIE_REG_SIZE) {
			pr_err("pcie_cae mmap_type_apb map size is invalid\n");
			return -EINVAL;
		}
		break;
	case MMAP_TYPE_NVME:
		phy_addr = NVME_BAR_BASE + CHIP_OFFSET * chip_id;
		if (size > NVME_BAR_SIZE) {
			pr_err("pcie_cae mmap_type_nvme map size is invalid\n");
			return -EINVAL;
		}
		break;
	case MMAP_TYPE_VIRTIO:
		phy_addr = VIRTIO_BAR_BASE + CHIP_OFFSET * chip_id;
		if (size > VIRTIO_BAR_SIZE) {
			pr_err("pcie_cae mmap_type_virtio map size is invalid\n");
			return -EINVAL;
		}
		break;
	default:
		pr_err("pcie_cae input addr type %u is invalid\n", type);
		return -EINVAL;
	}
	vma->vm_pgoff = phy_addr >> PAGE_SHIFT;
	vma->vm_page_prot =  pgprot_device(vma->vm_page_prot);
	vma->vm_ops = &mmap_pcie_mem_ops;
	/* Remap-pfn-range will mark the range VM_IO */
	if (remap_pfn_range(vma,
			    vma->vm_start,
			    vma->vm_pgoff,
			    size,
			    vma->vm_page_prot)) {
		pr_err("pcie_cae map pcie reg zone failed\n");
		return -EAGAIN;
	}

	return 0;
}

u32 pcie_get_chipnums(u32 cpu_info)
{
	int i;
	u32 chip_count = 0;
	u32 chip_i_info;

	for (i = 0; i < MAX_CHIP_NUM; i++) {
		chip_i_info = ((cpu_info & (0xFF << (BIT_SHIFT_8 * i))) >>
						(BIT_SHIFT_8 * i));
		if ((chip_i_info == CHIP1620) ||
			(chip_i_info == CHIP1620s) ||
			(chip_i_info == CHIP1601)) {
			chip_count++;
		}
	}

	return chip_count;
}

static int pcie_open(struct inode *inode, struct file *f)
{
	void __iomem *addr_base;
	u32 val;
	struct pci_dev *dev;
	int type;

	dev = pci_get_device(HI1620_PCI_VENDOR_ID, HI1620_PCI_DEVICE_ID, NULL);
	if (!dev) {
		pr_err("pcie_cae can only work at Hi1620 series chip\n");
		return -EINVAL;
	}

	type = pci_pcie_type(dev);
	pr_info("pcie_cae detect chip PCIe Vendor ID:0x%x, Device ID:0x%x\n",
		dev->vendor, dev->device);
	pci_dev_put(dev);

	if (type != PCI_EXP_TYPE_ROOT_PORT) {
		pr_err("pcie_cae can not support this chip\n");
		return -EINVAL;
	}

	addr_base = ioremap_nocache(SYSCTRL_SC_ECO_RSV1, CHIP_INFO_REG_SIZE);
	if (!addr_base) {
		pr_err("pcie_cae map chip_info_reg zone failed\n");
		return -EPERM;
	}

	val = readl(addr_base);
	current_chip_nums = pcie_get_chipnums(val);

	iounmap(addr_base);

	return 0;
}

static int pcie_release(struct inode *inode, struct file *f)
{
	return 0;
}

static long pcie_reg_ioctl(struct file *pfile, unsigned int cmd,
			   unsigned long arg)
{
	int ret = 0;

	switch (cmd) {
	case PCIE_CMD_GET_CHIPNUMS:
		if ((void *)arg == NULL) {
			pr_err("pcie_cae invalid arg address\n");
			ret = -EINVAL;
			break;
		}

		if (copy_to_user((void *)arg, (void *)&current_chip_nums,
				 sizeof(int))) {
			pr_err("pcie_cae copy chip_nums to usr failed\n");
			ret =  -EINVAL;
		}
		break;

	default:
		pr_err("pcie_cae invalid pcie ioctl cmd:%u\n", cmd);
		ret = -EINVAL;
		break;
	}

	return ret;
}

static const struct file_operations pcie_cae_fops = {
	.owner          = THIS_MODULE,
	.open           = pcie_open,
	.release        = pcie_release,
	.llseek         = noop_llseek,
	.mmap           = pcie_reg_mmap,
	.unlocked_ioctl = pcie_reg_ioctl,
};

static struct miscdevice pcie_cae_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.fops = &pcie_cae_fops,
	.name = DEVICE_NAME,
};

static int __init misc_dev_init(void)
{
	return misc_register(&pcie_cae_misc);
}

static void __exit misc_dev_exit(void)
{
	(void)misc_deregister(&pcie_cae_misc);
}

module_init(misc_dev_init);
module_exit(misc_dev_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Huawei Technology Company");
MODULE_DESCRIPTION("PCIe CAE Driver");
MODULE_VERSION("V1.2");
