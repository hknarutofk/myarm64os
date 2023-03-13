/*
 * WangXun 10 Gigabit PCI Express Linux driver
 * Copyright (c) 2015 - 2017 Beijing WangXun Technology Co., Ltd.
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
 * The full GNU General Public License is included in this distribution in
 * the file called "COPYING".
 *
 * based on ixgbe_main.c, Copyright(c) 1999 - 2017 Intel Corporation.
 * Contact Information:
 * Linux NICS <linux.nics@intel.com>
 * e1000-devel Mailing List <e1000-devel@lists.sourceforge.net>
 * Intel Corporation, 5200 N.E. Elam Young Parkway, Hillsboro, OR 97124-6497
 *
 * Copyright (c)2006 - 2007 Myricom, Inc. for some LRO specific code
 */

#include <linux/types.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/netdevice.h>
#include <linux/vmalloc.h>
#include <linux/highmem.h>
#include <linux/string.h>
#include <linux/in.h>
#include <linux/ip.h>
#include <linux/tcp.h>
#include <linux/pkt_sched.h>
#include <linux/ipv6.h>
#include <net/checksum.h>
#include <net/ip6_checksum.h>
#include <linux/if_macvlan.h>
#include <linux/ethtool.h>
#include <linux/if_bridge.h>
#include <net/vxlan.h>

#include "txgbe.h"
#include "txgbe_hw.h"
#include "txgbe_phy.h"
#include "txgbe_bp.h"

char txgbe_driver_name[32] = TXGBE_NAME;
static const char txgbe_driver_string[] =
			"WangXun 10 Gigabit PCI Express Network Driver";

#define DRV_HW_PERF

#define FPGA

#define DRIVERIOV

#define BYPASS_TAG

#define RELEASE_TAG

#define DRV_VERSION     __stringify(1.1.17oe)

const char txgbe_driver_version[32] = DRV_VERSION;
static const char txgbe_copyright[] =
		"Copyright (c) 2015 -2017 Beijing WangXun Technology Co., Ltd";
static const char txgbe_overheat_msg[] =
		"Network adapter has been stopped because it has over heated. "
		"If the problem persists, restart the computer, or "
		"power off the system and replace the adapter";
static const char txgbe_underheat_msg[] =
		"Network adapter has been started again since the temperature "
		"has been back to normal state";

/* txgbe_pci_tbl - PCI Device ID Table
 *
 * Wildcard entries (PCI_ANY_ID) should come last
 * Last entry must be all 0s
 *
 * { Vendor ID, Device ID, SubVendor ID, SubDevice ID,
 *   Class, Class Mask, private data (not used) }
 */
static const struct pci_device_id txgbe_pci_tbl[] = {
	{ PCI_VDEVICE(TRUSTNETIC, TXGBE_DEV_ID_SP1000), 0},
	{ PCI_VDEVICE(TRUSTNETIC, TXGBE_DEV_ID_WX1820), 0},
	/* required last entry */
	{ .device = 0 }
};
MODULE_DEVICE_TABLE(pci, txgbe_pci_tbl);

MODULE_AUTHOR("Beijing WangXun Technology Co., Ltd, <linux.nic@trustnetic.com>");
MODULE_DESCRIPTION("WangXun(R) 10 Gigabit PCI Express Network Driver");
MODULE_LICENSE("GPL");
MODULE_VERSION(DRV_VERSION);

#define DEFAULT_DEBUG_LEVEL_SHIFT 3

static struct workqueue_struct *txgbe_wq;

static bool txgbe_is_sfp(struct txgbe_hw *hw);
static bool txgbe_check_cfg_remove(struct txgbe_hw *hw, struct pci_dev *pdev);
static void txgbe_clean_rx_ring(struct txgbe_ring *rx_ring);
static void txgbe_clean_tx_ring(struct txgbe_ring *tx_ring);
static void txgbe_napi_enable_all(struct txgbe_adapter *adapter);
static void txgbe_napi_disable_all(struct txgbe_adapter *adapter);

extern txgbe_dptype txgbe_ptype_lookup[256];

static inline txgbe_dptype txgbe_decode_ptype(const u8 ptype)
{
	return txgbe_ptype_lookup[ptype];
}

static inline txgbe_dptype
decode_rx_desc_ptype(const union txgbe_rx_desc *rx_desc)
{
	return txgbe_decode_ptype(TXGBE_RXD_PKTTYPE(rx_desc));
}

static void txgbe_check_minimum_link(struct txgbe_adapter *adapter,
				     int expected_gts)
{
	struct txgbe_hw *hw = &adapter->hw;
	struct pci_dev *pdev;

	/* Some devices are not connected over PCIe and thus do not negotiate
	 * speed. These devices do not have valid bus info, and thus any report
	 * we generate may not be correct.
	 */
	if (hw->bus.type == txgbe_bus_type_internal)
		return;

	pdev = adapter->pdev;
	pcie_print_link_status(pdev);
}

/**
 * txgbe_enumerate_functions - Get the number of ports this device has
 * @adapter: adapter structure
 *
 * This function enumerates the phsyical functions co-located on a single slot,
 * in order to determine how many ports a device has. This is most useful in
 * determining the required GT/s of PCIe bandwidth necessary for optimal
 * performance.
 **/
static inline int txgbe_enumerate_functions(struct txgbe_adapter *adapter)
{
	struct pci_dev *entry, *pdev = adapter->pdev;
	int physfns = 0;

	list_for_each_entry(entry, &pdev->bus->devices, bus_list) {
		/* When the devices on the bus don't all match our device ID,
		 * we can't reliably determine the correct number of
		 * functions. This can occur if a function has been direct
		 * attached to a virtual machine using VT-d, for example. In
		 * this case, simply return -1 to indicate this.
		 */
		if ((entry->vendor != pdev->vendor) ||
			(entry->device != pdev->device))
			return -1;

		physfns++;
	}

	return physfns;
}

void txgbe_service_event_schedule(struct txgbe_adapter *adapter)
{
	if (!test_bit(__TXGBE_DOWN, &adapter->state) &&
	    !test_bit(__TXGBE_REMOVING, &adapter->state) &&
	    !test_and_set_bit(__TXGBE_SERVICE_SCHED, &adapter->state))
		queue_work(txgbe_wq, &adapter->service_task);
}

static void txgbe_service_event_complete(struct txgbe_adapter *adapter)
{
	BUG_ON(!test_bit(__TXGBE_SERVICE_SCHED, &adapter->state));

	/* flush memory to make sure state is correct before next watchdog */
	smp_mb__before_atomic();
	clear_bit(__TXGBE_SERVICE_SCHED, &adapter->state);
}

static void txgbe_remove_adapter(struct txgbe_hw *hw)
{
	struct txgbe_adapter *adapter = hw->back;

	if (!hw->hw_addr)
		return;
	hw->hw_addr = NULL;
	e_dev_err("Adapter removed\n");
	if (test_bit(__TXGBE_SERVICE_INITED, &adapter->state))
		txgbe_service_event_schedule(adapter);
}

static void txgbe_check_remove(struct txgbe_hw *hw, u32 reg)
{
	u32 value;

	/* The following check not only optimizes a bit by not
	 * performing a read on the status register when the
	 * register just read was a status register read that
	 * returned TXGBE_FAILED_READ_REG. It also blocks any
	 * potential recursion.
	 */
	if (reg == TXGBE_CFG_PORT_ST) {
		txgbe_remove_adapter(hw);
		return;
	}
	value = rd32(hw, TXGBE_CFG_PORT_ST);
	if (value == TXGBE_FAILED_READ_REG)
		txgbe_remove_adapter(hw);
}

static u32 txgbe_validate_register_read(struct txgbe_hw *hw, u32 reg, bool quiet)
{
	int i;
	u32 value;
	u8 __iomem *reg_addr;
	struct txgbe_adapter *adapter = hw->back;

	reg_addr = READ_ONCE(hw->hw_addr);
	if (TXGBE_REMOVED(reg_addr))
		return TXGBE_FAILED_READ_REG;
	for (i = 0; i < TXGBE_DEAD_READ_RETRIES; ++i) {
		value = txgbe_rd32(reg_addr + reg);
		if (value != TXGBE_DEAD_READ_REG)
			break;
	}
	if (quiet)
		return value;
	if (value == TXGBE_DEAD_READ_REG)
		e_err(drv, "%s: register %x read unchanged\n", __func__, reg);
	else
		e_warn(hw, "%s: register %x read recovered after %d retries\n",
			__func__, reg, i + 1);
	return value;
}

/**
 * txgbe_read_reg - Read from device register
 * @hw: hw specific details
 * @reg: offset of register to read
 *
 * Returns : value read or TXGBE_FAILED_READ_REG if removed
 *
 * This function is used to read device registers. It checks for device
 * removal by confirming any read that returns all ones by checking the
 * status register value for all ones. This function avoids reading from
 * the hardware if a removal was previously detected in which case it
 * returns TXGBE_FAILED_READ_REG (all ones).
 */
u32 txgbe_read_reg(struct txgbe_hw *hw, u32 reg, bool quiet)
{
	u32 value;
	u8 __iomem *reg_addr;

	reg_addr = READ_ONCE(hw->hw_addr);
	if (TXGBE_REMOVED(reg_addr))
		return TXGBE_FAILED_READ_REG;
	value = txgbe_rd32(reg_addr + reg);
	if (unlikely(value == TXGBE_FAILED_READ_REG))
		txgbe_check_remove(hw, reg);
	if (unlikely(value == TXGBE_DEAD_READ_REG))
		value = txgbe_validate_register_read(hw, reg, quiet);
	return value;
}

static void txgbe_release_hw_control(struct txgbe_adapter *adapter)
{
	/* Let firmware take over control of hw */
	wr32m(&adapter->hw, TXGBE_CFG_PORT_CTL,
		TXGBE_CFG_PORT_CTL_DRV_LOAD, 0);
}

static void txgbe_get_hw_control(struct txgbe_adapter *adapter)
{
	/* Let firmware know the driver has taken over */
	wr32m(&adapter->hw, TXGBE_CFG_PORT_CTL,
		TXGBE_CFG_PORT_CTL_DRV_LOAD, TXGBE_CFG_PORT_CTL_DRV_LOAD);
}

/**
 * txgbe_set_ivar - set the IVAR registers, mapping interrupt causes to vectors
 * @adapter: pointer to adapter struct
 * @direction: 0 for Rx, 1 for Tx, -1 for other causes
 * @queue: queue to map the corresponding interrupt to
 * @msix_vector: the vector to map to the corresponding queue
 *
 **/
static void txgbe_set_ivar(struct txgbe_adapter *adapter, s8 direction,
	u16 queue, u16 msix_vector)
{
	u32 ivar, index;
	struct txgbe_hw *hw = &adapter->hw;

	if (direction == -1) {
		/* other causes */
		msix_vector |= TXGBE_PX_IVAR_ALLOC_VAL;
		index = 0;
		ivar = rd32(&adapter->hw, TXGBE_PX_MISC_IVAR);
		ivar &= ~(0xFF << index);
		ivar |= (msix_vector << index);
		wr32(&adapter->hw, TXGBE_PX_MISC_IVAR, ivar);
	} else {
		/* tx or rx causes */
		msix_vector |= TXGBE_PX_IVAR_ALLOC_VAL;
		index = ((16 * (queue & 1)) + (8 * direction));
		ivar = rd32(hw, TXGBE_PX_IVAR(queue >> 1));
		ivar &= ~(0xFF << index);
		ivar |= (msix_vector << index);
		wr32(hw, TXGBE_PX_IVAR(queue >> 1), ivar);
	}
}

void txgbe_unmap_and_free_tx_resource(struct txgbe_ring *ring,
				      struct txgbe_tx_buffer *tx_buffer)
{
	if (tx_buffer->skb) {
		dev_kfree_skb_any(tx_buffer->skb);
		if (dma_unmap_len(tx_buffer, len))
			dma_unmap_single(ring->dev,
					 dma_unmap_addr(tx_buffer, dma),
					 dma_unmap_len(tx_buffer, len),
					 DMA_TO_DEVICE);
	} else if (dma_unmap_len(tx_buffer, len)) {
		dma_unmap_page(ring->dev,
			       dma_unmap_addr(tx_buffer, dma),
			       dma_unmap_len(tx_buffer, len),
			       DMA_TO_DEVICE);
	}
	tx_buffer->next_to_watch = NULL;
	tx_buffer->skb = NULL;
	dma_unmap_len_set(tx_buffer, len, 0);
	/* tx_buffer must be completely set up in the transmit path */
}

static void txgbe_update_xoff_received(struct txgbe_adapter *adapter)
{
	struct txgbe_hw *hw = &adapter->hw;
	struct txgbe_hw_stats *hwstats = &adapter->stats;
	u32 xoff[8] = {0};
	int tc;
	int i;

	/* update stats for each tc, only valid with PFC enabled */
	for (i = 0; i < MAX_TX_PACKET_BUFFERS; i++) {
		u32 pxoffrxc;
		wr32m(hw, TXGBE_MMC_CONTROL, TXGBE_MMC_CONTROL_UP, i<<16);
		pxoffrxc = rd32(hw, TXGBE_MAC_PXOFFRXC);
		hwstats->pxoffrxc[i] += pxoffrxc;
		/* Get the TC for given UP */
		tc = netdev_get_prio_tc_map(adapter->netdev, i);
		xoff[tc] += pxoffrxc;
	}

	/* disarm tx queues that have received xoff frames */
	for (i = 0; i < adapter->num_tx_queues; i++) {
		struct txgbe_ring *tx_ring = adapter->tx_ring[i];

		tc = tx_ring->dcb_tc;
		if ((tc <= 7) && (xoff[tc]))
			clear_bit(__TXGBE_HANG_CHECK_ARMED, &tx_ring->state);
	}
}

static u64 txgbe_get_tx_completed(struct txgbe_ring *ring)
{
	return ring->stats.packets;
}

static u64 txgbe_get_tx_pending(struct txgbe_ring *ring)
{
	struct txgbe_adapter *adapter;
	struct txgbe_hw *hw;
	u32 head, tail;

	if (ring->accel)
		adapter = ring->accel->adapter;
	else
		adapter = ring->q_vector->adapter;

	hw = &adapter->hw;
	head = rd32(hw, TXGBE_PX_TR_RP(ring->reg_idx));
	tail = rd32(hw, TXGBE_PX_TR_WP(ring->reg_idx));

	return ((head <= tail) ? tail : tail + ring->count) - head;
}

static inline bool txgbe_check_tx_hang(struct txgbe_ring *tx_ring)
{
	u64 tx_done = txgbe_get_tx_completed(tx_ring);
	u64 tx_done_old = tx_ring->tx_stats.tx_done_old;
	u64 tx_pending = txgbe_get_tx_pending(tx_ring);

	clear_check_for_tx_hang(tx_ring);

	/*
	 * Check for a hung queue, but be thorough. This verifies
	 * that a transmit has been completed since the previous
	 * check AND there is at least one packet pending. The
	 * ARMED bit is set to indicate a potential hang. The
	 * bit is cleared if a pause frame is received to remove
	 * false hang detection due to PFC or 802.3x frames. By
	 * requiring this to fail twice we avoid races with
	 * pfc clearing the ARMED bit and conditions where we
	 * run the check_tx_hang logic with a transmit completion
	 * pending but without time to complete it yet.
	 */
	if (tx_done_old == tx_done && tx_pending)
		/* make sure it is true for two checks in a row */
		return test_and_set_bit(__TXGBE_HANG_CHECK_ARMED,
					&tx_ring->state);
	/* update completed stats and continue */
	tx_ring->tx_stats.tx_done_old = tx_done;
	/* reset the countdown */
	clear_bit(__TXGBE_HANG_CHECK_ARMED, &tx_ring->state);

	return false;
}

/**
 * txgbe_tx_timeout - Respond to a Tx Hang
 * @netdev: network interface device structure
 **/
static void txgbe_tx_timeout(struct net_device *netdev)
{
	struct txgbe_adapter *adapter = netdev_priv(netdev);
	struct txgbe_hw *hw = &adapter->hw;
	bool real_tx_hang = false;
	int i;
	u16 value = 0;
	u32 value2 = 0, value3 = 0;
	u32 head, tail;

	for (i = 0; i < adapter->num_tx_queues; i++) {
		struct txgbe_ring *tx_ring = adapter->tx_ring[i];
		if (check_for_tx_hang(tx_ring) && txgbe_check_tx_hang(tx_ring))
			real_tx_hang = true;
	}

	pci_read_config_word(adapter->pdev, PCI_VENDOR_ID, &value);
	ERROR_REPORT1(TXGBE_ERROR_POLLING, "pci vendor id is 0x%x\n", value);

	pci_read_config_word(adapter->pdev, PCI_COMMAND, &value);
	ERROR_REPORT1(TXGBE_ERROR_POLLING, "pci command reg is 0x%x.\n", value);

	for (i = 0; i < adapter->num_tx_queues; i++) {
		head = rd32(&adapter->hw, TXGBE_PX_TR_RP(adapter->tx_ring[i]->reg_idx));
		tail = rd32(&adapter->hw, TXGBE_PX_TR_WP(adapter->tx_ring[i]->reg_idx));

		ERROR_REPORT1(TXGBE_ERROR_POLLING,
			"tx ring %d next_to_use is %d, next_to_clean is %d\n",
			i, adapter->tx_ring[i]->next_to_use, adapter->tx_ring[i]->next_to_clean);
		ERROR_REPORT1(TXGBE_ERROR_POLLING,
			"tx ring %d hw rp is 0x%x, wp is 0x%x\n", i, head, tail);
	}

	value2 = rd32(&adapter->hw, TXGBE_PX_IMS(0));
	value3 = rd32(&adapter->hw, TXGBE_PX_IMS(1));
	ERROR_REPORT1(TXGBE_ERROR_POLLING,
			"PX_IMS0 value is 0x%08x, PX_IMS1 value is 0x%08x\n", value2, value3);

	if (value2 || value3) {
		ERROR_REPORT1(TXGBE_ERROR_POLLING, "clear interrupt mask.\n");
		wr32(&adapter->hw, TXGBE_PX_ICS(0), value2);
		wr32(&adapter->hw, TXGBE_PX_IMC(0), value2);
		wr32(&adapter->hw, TXGBE_PX_ICS(1), value3);
		wr32(&adapter->hw, TXGBE_PX_IMC(1), value3);
	}

	if (adapter->hw.bus.lan_id == 0) {
		ERROR_REPORT1(TXGBE_ERROR_POLLING, "tx timeout. do pcie recovery.\n");
		adapter->flags2 |= TXGBE_FLAG2_PCIE_NEED_RECOVER;
		txgbe_service_event_schedule(adapter);
	} else
		wr32(&adapter->hw, TXGBE_MIS_PF_SM, 1);
}

#define TX_WAKE_THRESHOLD (DESC_NEEDED * 2)

/**
 * txgbe_clean_tx_irq - Reclaim resources after transmit completes
 * @q_vector: structure containing interrupt and ring information
 * @tx_ring: tx ring to clean
 **/
static bool txgbe_clean_tx_irq(struct txgbe_q_vector *q_vector,
			       struct txgbe_ring *tx_ring)
{
	struct txgbe_adapter *adapter = q_vector->adapter;
	struct txgbe_tx_buffer *tx_buffer;
	union txgbe_tx_desc *tx_desc;
	unsigned int total_bytes = 0, total_packets = 0;
	unsigned int budget = q_vector->tx.work_limit;
	unsigned int i = tx_ring->next_to_clean;

	if (test_bit(__TXGBE_DOWN, &adapter->state))
		return true;

	tx_buffer = &tx_ring->tx_buffer_info[i];
	tx_desc = TXGBE_TX_DESC(tx_ring, i);
	i -= tx_ring->count;

	do {
		union txgbe_tx_desc *eop_desc = tx_buffer->next_to_watch;

		/* if next_to_watch is not set then there is no work pending */
		if (!eop_desc)
			break;

		/* prevent any other reads prior to eop_desc */
		read_barrier_depends();

		/* if DD is not set pending work has not been completed */
		if (!(eop_desc->wb.status & cpu_to_le32(TXGBE_TXD_STAT_DD)))
			break;

		/* clear next_to_watch to prevent false hangs */
		tx_buffer->next_to_watch = NULL;

		/* update the statistics for this packet */
		total_bytes += tx_buffer->bytecount;
		total_packets += tx_buffer->gso_segs;

		/* free the skb */
		dev_consume_skb_any(tx_buffer->skb);

		/* unmap skb header data */
		dma_unmap_single(tx_ring->dev,
				 dma_unmap_addr(tx_buffer, dma),
				 dma_unmap_len(tx_buffer, len),
				 DMA_TO_DEVICE);

		/* clear tx_buffer data */
		tx_buffer->skb = NULL;
		dma_unmap_len_set(tx_buffer, len, 0);

		/* unmap remaining buffers */
		while (tx_desc != eop_desc) {
			tx_buffer++;
			tx_desc++;
			i++;
			if (unlikely(!i)) {
				i -= tx_ring->count;
				tx_buffer = tx_ring->tx_buffer_info;
				tx_desc = TXGBE_TX_DESC(tx_ring, 0);
			}

			/* unmap any remaining paged data */
			if (dma_unmap_len(tx_buffer, len)) {
				dma_unmap_page(tx_ring->dev,
					       dma_unmap_addr(tx_buffer, dma),
					       dma_unmap_len(tx_buffer, len),
					       DMA_TO_DEVICE);
				dma_unmap_len_set(tx_buffer, len, 0);
			}
		}

		/* move us one more past the eop_desc for start of next pkt */
		tx_buffer++;
		tx_desc++;
		i++;
		if (unlikely(!i)) {
			i -= tx_ring->count;
			tx_buffer = tx_ring->tx_buffer_info;
			tx_desc = TXGBE_TX_DESC(tx_ring, 0);
		}

		/* issue prefetch for next Tx descriptor */
		prefetch(tx_desc);

		/* update budget accounting */
		budget--;
	} while (likely(budget));

	i += tx_ring->count;
	tx_ring->next_to_clean = i;
	u64_stats_update_begin(&tx_ring->syncp);
	tx_ring->stats.bytes += total_bytes;
	tx_ring->stats.packets += total_packets;
	u64_stats_update_end(&tx_ring->syncp);
	q_vector->tx.total_bytes += total_bytes;
	q_vector->tx.total_packets += total_packets;

	if (check_for_tx_hang(tx_ring) && txgbe_check_tx_hang(tx_ring)) {
	/* schedule immediate reset if we believe we hung */
		struct txgbe_hw *hw = &adapter->hw;
		u16 value = 0;

		e_err(drv, "Detected Tx Unit Hang\n"
			"  Tx Queue             <%d>\n"
			"  TDH, TDT             <%x>, <%x>\n"
			"  next_to_use          <%x>\n"
			"  next_to_clean        <%x>\n"
			"tx_buffer_info[next_to_clean]\n"
			"  time_stamp           <%lx>\n"
			"  jiffies              <%lx>\n",
			tx_ring->queue_index,
			rd32(hw, TXGBE_PX_TR_RP(tx_ring->reg_idx)),
			rd32(hw, TXGBE_PX_TR_WP(tx_ring->reg_idx)),
			tx_ring->next_to_use, i,
			tx_ring->tx_buffer_info[i].time_stamp, jiffies);

		pci_read_config_word(adapter->pdev, PCI_VENDOR_ID, &value);
		if (value == TXGBE_FAILED_READ_CFG_WORD) {
			e_info(hw, "pcie link has been lost.\n");
		}

		netif_stop_subqueue(tx_ring->netdev, tx_ring->queue_index);

		e_info(probe,
		       "tx hang %d detected on queue %d, resetting adapter\n",
			adapter->tx_timeout_count + 1, tx_ring->queue_index);

		/* schedule immediate reset if we believe we hung */
		e_info(hw, "real tx hang. do pcie recovery.\n");
		adapter->flags2 |= TXGBE_FLAG2_PCIE_NEED_RECOVER;
		txgbe_service_event_schedule(adapter);

		/* the adapter is about to reset, no point in enabling stuff */
		return true;
	}

	netdev_tx_completed_queue(txring_txq(tx_ring),
				  total_packets, total_bytes);

	if (unlikely(total_packets && netif_carrier_ok(tx_ring->netdev) &&
		     (txgbe_desc_unused(tx_ring) >= TX_WAKE_THRESHOLD))) {
		/* Make sure that anybody stopping the queue after this
		 * sees the new next_to_clean.
		 */
		smp_mb();

		if (__netif_subqueue_stopped(tx_ring->netdev,
		    tx_ring->queue_index)
		    && !test_bit(__TXGBE_DOWN, &adapter->state)) {
			netif_wake_subqueue(tx_ring->netdev,
					    tx_ring->queue_index);
			++tx_ring->tx_stats.restart_queue;
		}
	}

	return !!budget;
}

#define TXGBE_RSS_L4_TYPES_MASK \
	((1ul << TXGBE_RXD_RSSTYPE_IPV4_TCP) | \
	 (1ul << TXGBE_RXD_RSSTYPE_IPV4_UDP) | \
	 (1ul << TXGBE_RXD_RSSTYPE_IPV4_SCTP) | \
	 (1ul << TXGBE_RXD_RSSTYPE_IPV6_TCP) | \
	 (1ul << TXGBE_RXD_RSSTYPE_IPV6_UDP) | \
	 (1ul << TXGBE_RXD_RSSTYPE_IPV6_SCTP))

static inline void txgbe_rx_hash(struct txgbe_ring *ring,
				 union txgbe_rx_desc *rx_desc,
				 struct sk_buff *skb)
{
	u16 rss_type;

	if (!(ring->netdev->features & NETIF_F_RXHASH))
		return;

	rss_type = le16_to_cpu(rx_desc->wb.lower.lo_dword.hs_rss.pkt_info) &
		   TXGBE_RXD_RSSTYPE_MASK;

	if (!rss_type)
		return;

	skb_set_hash(skb, le32_to_cpu(rx_desc->wb.lower.hi_dword.rss),
		     (TXGBE_RSS_L4_TYPES_MASK & (1ul << rss_type)) ?
		     PKT_HASH_TYPE_L4 : PKT_HASH_TYPE_L3);
}

/**
 * txgbe_rx_checksum - indicate in skb if hw indicated a good cksum
 * @ring: structure containing ring specific data
 * @rx_desc: current Rx descriptor being processed
 * @skb: skb currently being received and modified
 **/
static inline void txgbe_rx_checksum(struct txgbe_ring *ring,
				     union txgbe_rx_desc *rx_desc,
				     struct sk_buff *skb)
{
	txgbe_dptype dptype = decode_rx_desc_ptype(rx_desc);

	skb->ip_summed = CHECKSUM_NONE;

	skb_checksum_none_assert(skb);

	/* Rx csum disabled */
	if (!(ring->netdev->features & NETIF_F_RXCSUM))
		return;

	/* if IPv4 header checksum error */
	if ((txgbe_test_staterr(rx_desc, TXGBE_RXD_STAT_IPCS) &&
		txgbe_test_staterr(rx_desc, TXGBE_RXD_ERR_IPE)) ||
		(txgbe_test_staterr(rx_desc, TXGBE_RXD_STAT_OUTERIPCS) &&
		txgbe_test_staterr(rx_desc, TXGBE_RXD_ERR_OUTERIPER))) {
		ring->rx_stats.csum_err++;
		return;
	}

	/* L4 checksum offload flag must set for the below code to work */
	if (!txgbe_test_staterr(rx_desc, TXGBE_RXD_STAT_L4CS))
		return;

	/*likely incorrect csum if IPv6 Dest Header found */
	if (dptype.prot != TXGBE_DEC_PTYPE_PROT_SCTP && TXGBE_RXD_IPV6EX(rx_desc))
		return;

	/* if L4 checksum error */
	if (txgbe_test_staterr(rx_desc, TXGBE_RXD_ERR_TCPE)) {
		ring->rx_stats.csum_err++;
		return;
	}
	/* If there is an outer header present that might contain a checksum
	 * we need to bump the checksum level by 1 to reflect the fact that
	 * we are indicating we validated the inner checksum.
	*/
	if (dptype.etype >= TXGBE_DEC_PTYPE_ETYPE_IG) {
		skb->csum_level = 1;
		/* FIXME :does skb->csum_level skb->encapsulation can both set ? */
		skb->encapsulation = 1;
	}

	/* It must be a TCP or UDP or SCTP packet with a valid checksum */
	skb->ip_summed = CHECKSUM_UNNECESSARY;
	ring->rx_stats.csum_good_cnt++;
}

static bool txgbe_alloc_mapped_skb(struct txgbe_ring *rx_ring,
			struct txgbe_rx_buffer *bi)
{
	struct sk_buff *skb = bi->skb;
	dma_addr_t dma = bi->dma;

	if (unlikely(dma))
		return true;

	if (likely(!skb)) {
		skb = netdev_alloc_skb_ip_align(rx_ring->netdev,
						rx_ring->rx_buf_len);
		if (unlikely(!skb)) {
			rx_ring->rx_stats.alloc_rx_buff_failed++;
			return false;
		}

		bi->skb = skb;

	}

	dma = dma_map_single(rx_ring->dev, skb->data,
			     rx_ring->rx_buf_len, DMA_FROM_DEVICE);

	/*
	 * if mapping failed free memory back to system since
	 * there isn't much point in holding memory we can't use
	 */
	if (dma_mapping_error(rx_ring->dev, dma)) {
		dev_kfree_skb_any(skb);
		bi->skb = NULL;

		rx_ring->rx_stats.alloc_rx_buff_failed++;
		return false;
	}

	bi->dma = dma;
	return true;
}

static bool txgbe_alloc_mapped_page(struct txgbe_ring *rx_ring,
				    struct txgbe_rx_buffer *bi)
{
	struct page *page = bi->page;
	dma_addr_t dma;

	/* since we are recycling buffers we should seldom need to alloc */
	if (likely(page))
		return true;

	/* alloc new page for storage */
	page = dev_alloc_pages(txgbe_rx_pg_order(rx_ring));
	if (unlikely(!page)) {
		rx_ring->rx_stats.alloc_rx_page_failed++;
		return false;
	}

	/* map page for use */
	dma = dma_map_page(rx_ring->dev, page, 0,
			   txgbe_rx_pg_size(rx_ring), DMA_FROM_DEVICE);

	/*
	 * if mapping failed free memory back to system since
	 * there isn't much point in holding memory we can't use
	 */
	if (dma_mapping_error(rx_ring->dev, dma)) {
		__free_pages(page, txgbe_rx_pg_order(rx_ring));

		rx_ring->rx_stats.alloc_rx_page_failed++;
		return false;
	}

	bi->page_dma = dma;
	bi->page = page;
	bi->page_offset = 0;

	return true;
}

/**
 * txgbe_alloc_rx_buffers - Replace used receive buffers
 * @rx_ring: ring to place buffers on
 * @cleaned_count: number of buffers to replace
 **/
void txgbe_alloc_rx_buffers(struct txgbe_ring *rx_ring, u16 cleaned_count)
{
	union txgbe_rx_desc *rx_desc;
	struct txgbe_rx_buffer *bi;
	u16 i = rx_ring->next_to_use;

	/* nothing to do */
	if (!cleaned_count)
		return;

	rx_desc = TXGBE_RX_DESC(rx_ring, i);
	bi = &rx_ring->rx_buffer_info[i];
	i -= rx_ring->count;

	do {
		if (ring_is_hs_enabled(rx_ring)) {
			if (!txgbe_alloc_mapped_skb(rx_ring, bi))
				break;
			rx_desc->read.hdr_addr = cpu_to_le64(bi->dma);
		}

		if (!txgbe_alloc_mapped_page(rx_ring, bi))
			break;
		rx_desc->read.pkt_addr =
			cpu_to_le64(bi->page_dma + bi->page_offset);

		rx_desc++;
		bi++;
		i++;
		if (unlikely(!i)) {
			rx_desc = TXGBE_RX_DESC(rx_ring, 0);
			bi = rx_ring->rx_buffer_info;
			i -= rx_ring->count;
		}

		/* clear the status bits for the next_to_use descriptor */
		rx_desc->wb.upper.status_error = 0;

		cleaned_count--;
	} while (cleaned_count);

	i += rx_ring->count;

	if (rx_ring->next_to_use != i) {
		rx_ring->next_to_use = i;
		/* update next to alloc since we have filled the ring */
		rx_ring->next_to_alloc = i;

		/* Force memory writes to complete before letting h/w
		 * know there are new descriptors to fetch.  (Only
		 * applicable for weak-ordered memory model archs,
		 * such as IA-64).
		 */
		wmb();
		writel(i, rx_ring->tail);
	}
}

static inline u16 txgbe_get_hlen(struct txgbe_ring *rx_ring,
				 union txgbe_rx_desc *rx_desc)
{
	__le16 hdr_info = rx_desc->wb.lower.lo_dword.hs_rss.hdr_info;
	u16 hlen = le16_to_cpu(hdr_info) & TXGBE_RXD_HDRBUFLEN_MASK;

	UNREFERENCED_PARAMETER(rx_ring);

	if (hlen > (TXGBE_RX_HDR_SIZE << TXGBE_RXD_HDRBUFLEN_SHIFT))
		hlen = 0;
	else
		hlen >>= TXGBE_RXD_HDRBUFLEN_SHIFT;

	return hlen;
}

static void txgbe_set_rsc_gso_size(struct txgbe_ring __maybe_unused *ring,
				   struct sk_buff *skb)
{
	u16 hdr_len = eth_get_headlen(skb->data, skb_headlen(skb));

	/* set gso_size to avoid messing up TCP MSS */
	skb_shinfo(skb)->gso_size = DIV_ROUND_UP((skb->len - hdr_len),
						 TXGBE_CB(skb)->append_cnt);
	skb_shinfo(skb)->gso_type = SKB_GSO_TCPV4;
}

static void txgbe_update_rsc_stats(struct txgbe_ring *rx_ring,
				   struct sk_buff *skb)
{
	/* if append_cnt is 0 then frame is not RSC */
	if (!TXGBE_CB(skb)->append_cnt)
		return;

	rx_ring->rx_stats.rsc_count += TXGBE_CB(skb)->append_cnt;
	rx_ring->rx_stats.rsc_flush++;

	txgbe_set_rsc_gso_size(rx_ring, skb);

	/* gso_size is computed using append_cnt so always clear it last */
	TXGBE_CB(skb)->append_cnt = 0;
}

static void txgbe_rx_vlan(struct txgbe_ring *ring,
			  union txgbe_rx_desc *rx_desc,
			  struct sk_buff *skb)
{
	u8 idx = 0;
	u16 ethertype;

	if ((ring->netdev->features & NETIF_F_HW_VLAN_CTAG_RX) &&
			txgbe_test_staterr(rx_desc, TXGBE_RXD_STAT_VP)) {
		idx = (le16_to_cpu(rx_desc->wb.lower.lo_dword.hs_rss.pkt_info) &
			TXGBE_RXD_TPID_MASK) >> TXGBE_RXD_TPID_SHIFT;
		ethertype = ring->q_vector->adapter->hw.tpid[idx];
		__vlan_hwaccel_put_tag(skb,
				       htons(ethertype),
				       le16_to_cpu(rx_desc->wb.upper.vlan));
	}
}

/**
 * txgbe_process_skb_fields - Populate skb header fields from Rx descriptor
 * @rx_ring: rx descriptor ring packet is being transacted on
 * @rx_desc: pointer to the EOP Rx descriptor
 * @skb: pointer to current skb being populated
 *
 * This function checks the ring, descriptor, and packet information in
 * order to populate the hash, checksum, VLAN, timestamp, protocol, and
 * other fields within the skb.
 **/
static void txgbe_process_skb_fields(struct txgbe_ring *rx_ring,
				     union txgbe_rx_desc *rx_desc,
				     struct sk_buff *skb)
{
	u32 flags = rx_ring->q_vector->adapter->flags;

	txgbe_update_rsc_stats(rx_ring, skb);
	txgbe_rx_hash(rx_ring, rx_desc, skb);
	txgbe_rx_checksum(rx_ring, rx_desc, skb);

	if (unlikely(flags & TXGBE_FLAG_RX_HWTSTAMP_ENABLED) &&
		unlikely(txgbe_test_staterr(rx_desc, TXGBE_RXD_STAT_TS))) {
		txgbe_ptp_rx_hwtstamp(rx_ring->q_vector->adapter, skb);
		rx_ring->last_rx_timestamp = jiffies;
	}

	txgbe_rx_vlan(rx_ring, rx_desc, skb);

	skb_record_rx_queue(skb, rx_ring->queue_index);

	skb->protocol = eth_type_trans(skb, rx_ring->netdev);
}

static void txgbe_rx_skb(struct txgbe_q_vector *q_vector,
			 struct sk_buff *skb)
{
	napi_gro_receive(&q_vector->napi, skb);
}

/**
 * txgbe_is_non_eop - process handling of non-EOP buffers
 * @rx_ring: Rx ring being processed
 * @rx_desc: Rx descriptor for current buffer
 * @skb: Current socket buffer containing buffer in progress
 *
 * This function updates next to clean.  If the buffer is an EOP buffer
 * this function exits returning false, otherwise it will place the
 * sk_buff in the next buffer to be chained and return true indicating
 * that this is in fact a non-EOP buffer.
 **/
static bool txgbe_is_non_eop(struct txgbe_ring *rx_ring,
			     union txgbe_rx_desc *rx_desc,
			     struct sk_buff *skb)
{
	struct txgbe_rx_buffer *rx_buffer =
			&rx_ring->rx_buffer_info[rx_ring->next_to_clean];
	u32 ntc = rx_ring->next_to_clean + 1;

	/* fetch, update, and store next to clean */
	ntc = (ntc < rx_ring->count) ? ntc : 0;
	rx_ring->next_to_clean = ntc;

	prefetch(TXGBE_RX_DESC(rx_ring, ntc));

	/* update RSC append count if present */
	if (ring_is_rsc_enabled(rx_ring)) {
		__le32 rsc_enabled = rx_desc->wb.lower.lo_dword.data &
				     cpu_to_le32(TXGBE_RXD_RSCCNT_MASK);

		if (unlikely(rsc_enabled)) {
			u32 rsc_cnt = le32_to_cpu(rsc_enabled);

			rsc_cnt >>= TXGBE_RXD_RSCCNT_SHIFT;
			TXGBE_CB(skb)->append_cnt += rsc_cnt - 1;

			/* update ntc based on RSC value */
			ntc = le32_to_cpu(rx_desc->wb.upper.status_error);
			ntc &= TXGBE_RXD_NEXTP_MASK;
			ntc >>= TXGBE_RXD_NEXTP_SHIFT;
		}
	}

	/* if we are the last buffer then there is nothing else to do */
	if (likely(txgbe_test_staterr(rx_desc, TXGBE_RXD_STAT_EOP)))
		return false;

	/* place skb in next buffer to be received */
	if (ring_is_hs_enabled(rx_ring)) {
		rx_buffer->skb = rx_ring->rx_buffer_info[ntc].skb;
		rx_buffer->dma = rx_ring->rx_buffer_info[ntc].dma;
		rx_ring->rx_buffer_info[ntc].dma = 0;
	}
	rx_ring->rx_buffer_info[ntc].skb = skb;
	rx_ring->rx_stats.non_eop_descs++;

	return true;
}

/**
 * txgbe_pull_tail - txgbe specific version of skb_pull_tail
 * @skb: pointer to current skb being adjusted
 *
 * This function is an txgbe specific version of __pskb_pull_tail.  The
 * main difference between this version and the original function is that
 * this function can make several assumptions about the state of things
 * that allow for significant optimizations versus the standard function.
 * As a result we can do things like drop a frag and maintain an accurate
 * truesize for the skb.
 */
static void txgbe_pull_tail(struct sk_buff *skb)
{
	skb_frag_t *frag = &skb_shinfo(skb)->frags[0];
	unsigned char *va;
	unsigned int pull_len;

	/*
	 * it is valid to use page_address instead of kmap since we are
	 * working with pages allocated out of the lomem pool per
	 * alloc_page(GFP_ATOMIC)
	 */
	va = skb_frag_address(frag);

	/*
	 * we need the header to contain the greater of either ETH_HLEN or
	 * 60 bytes if the skb->len is less than 60 for skb_pad.
	 */
	pull_len = eth_get_headlen(va, TXGBE_RX_HDR_SIZE);

	/* align pull length to size of long to optimize memcpy performance */
	skb_copy_to_linear_data(skb, va, ALIGN(pull_len, sizeof(long)));

	/* update all of the pointers */
	skb_frag_size_sub(frag, pull_len);
	frag->page_offset += pull_len;
	skb->data_len -= pull_len;
	skb->tail += pull_len;
}

/**
 * txgbe_dma_sync_frag - perform DMA sync for first frag of SKB
 * @rx_ring: rx descriptor ring packet is being transacted on
 * @skb: pointer to current skb being updated
 *
 * This function provides a basic DMA sync up for the first fragment of an
 * skb.  The reason for doing this is that the first fragment cannot be
 * unmapped until we have reached the end of packet descriptor for a buffer
 * chain.
 */
static void txgbe_dma_sync_frag(struct txgbe_ring *rx_ring,
				struct sk_buff *skb)
{
	if (ring_uses_build_skb(rx_ring)) {
		unsigned long offset = (unsigned long)(skb->data) & ~PAGE_MASK;
		dma_sync_single_range_for_cpu(rx_ring->dev,
					      TXGBE_CB(skb)->dma,
					      offset,
					      skb_headlen(skb),
					      DMA_FROM_DEVICE);
	} else {
		skb_frag_t *frag = &skb_shinfo(skb)->frags[0];
		dma_sync_single_range_for_cpu(rx_ring->dev,
					      TXGBE_CB(skb)->dma,
					      frag->page_offset,
					      skb_frag_size(frag),
					      DMA_FROM_DEVICE);
	}

	/* If the page was released, just unmap it. */
	if (unlikely(TXGBE_CB(skb)->page_released)) {
		dma_unmap_page_attrs(rx_ring->dev, TXGBE_CB(skb)->dma,
				     txgbe_rx_pg_size(rx_ring),
				     DMA_FROM_DEVICE,
				     TXGBE_RX_DMA_ATTR);
	}
}

/**
 * txgbe_cleanup_headers - Correct corrupted or empty headers
 * @rx_ring: rx descriptor ring packet is being transacted on
 * @rx_desc: pointer to the EOP Rx descriptor
 * @skb: pointer to current skb being fixed
 *
 * Check for corrupted packet headers caused by senders on the local L2
 * embedded NIC switch not setting up their Tx Descriptors right.  These
 * should be very rare.
 *
 * Also address the case where we are pulling data in on pages only
 * and as such no data is present in the skb header.
 *
 * In addition if skb is not at least 60 bytes we need to pad it so that
 * it is large enough to qualify as a valid Ethernet frame.
 *
 * Returns true if an error was encountered and skb was freed.
 **/
static bool txgbe_cleanup_headers(struct txgbe_ring *rx_ring,
				  union txgbe_rx_desc *rx_desc,
				  struct sk_buff *skb)
{
	struct net_device *netdev = rx_ring->netdev;

	/* verify that the packet does not have any known errors */
	if (unlikely(txgbe_test_staterr(rx_desc,
		TXGBE_RXD_ERR_FRAME_ERR_MASK) &&
		!(netdev->features & NETIF_F_RXALL))) {
		dev_kfree_skb_any(skb);
		return true;
	}

	/* place header in linear portion of buffer */
	if (skb_is_nonlinear(skb)  && !skb_headlen(skb))
		txgbe_pull_tail(skb);

	/* if eth_skb_pad returns an error the skb was freed */
	if (eth_skb_pad(skb))
		return true;

	return false;
}

/**
 * txgbe_reuse_rx_page - page flip buffer and store it back on the ring
 * @rx_ring: rx descriptor ring to store buffers on
 * @old_buff: donor buffer to have page reused
 *
 * Synchronizes page for reuse by the adapter
 **/
static void txgbe_reuse_rx_page(struct txgbe_ring *rx_ring,
				struct txgbe_rx_buffer *old_buff)
{
	struct txgbe_rx_buffer *new_buff;
	u16 nta = rx_ring->next_to_alloc;

	new_buff = &rx_ring->rx_buffer_info[nta];

	/* update, and store next to alloc */
	nta++;
	rx_ring->next_to_alloc = (nta < rx_ring->count) ? nta : 0;

	/* transfer page from old buffer to new buffer */
	new_buff->page_dma = old_buff->page_dma;
	new_buff->page = old_buff->page;
	new_buff->page_offset = old_buff->page_offset;

	/* sync the buffer for use by the device */
	dma_sync_single_range_for_device(rx_ring->dev, new_buff->page_dma,
					 new_buff->page_offset,
					 txgbe_rx_bufsz(rx_ring),
					 DMA_FROM_DEVICE);
}

static inline bool txgbe_page_is_reserved(struct page *page)
{
	return (page_to_nid(page) != numa_mem_id()) || page_is_pfmemalloc(page);
}

/**
 * txgbe_add_rx_frag - Add contents of Rx buffer to sk_buff
 * @rx_ring: rx descriptor ring to transact packets on
 * @rx_buffer: buffer containing page to add
 * @rx_desc: descriptor containing length of buffer written by hardware
 * @skb: sk_buff to place the data into
 *
 * This function will add the data contained in rx_buffer->page to the skb.
 * This is done either through a direct copy if the data in the buffer is
 * less than the skb header size, otherwise it will just attach the page as
 * a frag to the skb.
 *
 * The function will then update the page offset if necessary and return
 * true if the buffer can be reused by the adapter.
 **/
static bool txgbe_add_rx_frag(struct txgbe_ring *rx_ring,
			      struct txgbe_rx_buffer *rx_buffer,
			      union txgbe_rx_desc *rx_desc,
			      struct sk_buff *skb)
{
	struct page *page = rx_buffer->page;
	unsigned int size = le16_to_cpu(rx_desc->wb.upper.length);
#if (PAGE_SIZE < 8192)
	unsigned int truesize = txgbe_rx_bufsz(rx_ring);
#else
	unsigned int truesize = ALIGN(size, L1_CACHE_BYTES);
	unsigned int last_offset = txgbe_rx_pg_size(rx_ring) -
				   txgbe_rx_bufsz(rx_ring);
#endif

	if ((size <= TXGBE_RX_HDR_SIZE) && !skb_is_nonlinear(skb) &&
	    !ring_is_hs_enabled(rx_ring)) {
		unsigned char *va = page_address(page) + rx_buffer->page_offset;

		memcpy(__skb_put(skb, size), va, ALIGN(size, sizeof(long)));

		/* page is not reserved, we can reuse buffer as-is */
		if (likely(!txgbe_page_is_reserved(page)))
			return true;

		/* this page cannot be reused so discard it */
		__free_pages(page, txgbe_rx_pg_order(rx_ring));
		return false;
	}

	skb_add_rx_frag(skb, skb_shinfo(skb)->nr_frags, page,
			rx_buffer->page_offset, size, truesize);

	/* avoid re-using remote pages */
	if (unlikely(txgbe_page_is_reserved(page)))
		return false;

#if (PAGE_SIZE < 8192)
	/* if we are only owner of page we can reuse it */
	if (unlikely(page_count(page) != 1))
		return false;

	/* flip page offset to other buffer */
	rx_buffer->page_offset ^= truesize;
#else
	/* move offset up to the next cache line */
	rx_buffer->page_offset += truesize;

	if (rx_buffer->page_offset > last_offset)
		return false;
#endif

	/* Even if we own the page, we are not allowed to use atomic_set()
	 * This would break get_page_unless_zero() users.
	 */
	page_ref_inc(page);

	return true;
}

static struct sk_buff *txgbe_fetch_rx_buffer(struct txgbe_ring *rx_ring,
					     union txgbe_rx_desc *rx_desc)
{
	struct txgbe_rx_buffer *rx_buffer;
	struct sk_buff *skb;
	struct page *page;

	rx_buffer = &rx_ring->rx_buffer_info[rx_ring->next_to_clean];
	page = rx_buffer->page;
	prefetchw(page);

	skb = rx_buffer->skb;

	if (likely(!skb)) {
		void *page_addr = page_address(page) +
				  rx_buffer->page_offset;

		/* prefetch first cache line of first page */
		prefetch(page_addr);
#if L1_CACHE_BYTES < 128
		prefetch(page_addr + L1_CACHE_BYTES);
#endif

		/* allocate a skb to store the frags */
		skb = netdev_alloc_skb_ip_align(rx_ring->netdev,
						TXGBE_RX_HDR_SIZE);
		if (unlikely(!skb)) {
			rx_ring->rx_stats.alloc_rx_buff_failed++;
			return NULL;
		}

		/*
		 * we will be copying header into skb->data in
		 * pskb_may_pull so it is in our interest to prefetch
		 * it now to avoid a possible cache miss
		 */
		prefetchw(skb->data);

		/*
		 * Delay unmapping of the first packet. It carries the
		 * header information, HW may still access the header
		 * after the writeback.  Only unmap it when EOP is
		 * reached
		 */
		if (likely(txgbe_test_staterr(rx_desc, TXGBE_RXD_STAT_EOP)))
			goto dma_sync;

		TXGBE_CB(skb)->dma = rx_buffer->page_dma;
	} else {
		if (txgbe_test_staterr(rx_desc, TXGBE_RXD_STAT_EOP))
			txgbe_dma_sync_frag(rx_ring, skb);

dma_sync:
		/* we are reusing so sync this buffer for CPU use */
		dma_sync_single_range_for_cpu(rx_ring->dev,
					      rx_buffer->page_dma,
					      rx_buffer->page_offset,
					      txgbe_rx_bufsz(rx_ring),
					      DMA_FROM_DEVICE);

		rx_buffer->skb = NULL;
	}

	/* pull page into skb */
	if (txgbe_add_rx_frag(rx_ring, rx_buffer, rx_desc, skb)) {
		/* hand second half of page back to the ring */
		txgbe_reuse_rx_page(rx_ring, rx_buffer);
	} else if (TXGBE_CB(skb)->dma == rx_buffer->page_dma) {
		/* the page has been released from the ring */
		TXGBE_CB(skb)->page_released = true;
	} else {
		/* we are not reusing the buffer so unmap it */
		dma_unmap_page(rx_ring->dev, rx_buffer->page_dma,
			       txgbe_rx_pg_size(rx_ring),
			       DMA_FROM_DEVICE);
	}

	/* clear contents of buffer_info */
	rx_buffer->page = NULL;

	return skb;
}

static struct sk_buff *txgbe_fetch_rx_buffer_hs(struct txgbe_ring *rx_ring,
					     union txgbe_rx_desc *rx_desc)
{
	struct txgbe_rx_buffer *rx_buffer;
	struct sk_buff *skb;
	struct page *page;
	int hdr_len = 0;

	rx_buffer = &rx_ring->rx_buffer_info[rx_ring->next_to_clean];
	page = rx_buffer->page;
	prefetchw(page);

	skb = rx_buffer->skb;
	rx_buffer->skb = NULL;
	prefetchw(skb->data);

	if (!skb_is_nonlinear(skb)) {
		hdr_len = txgbe_get_hlen(rx_ring, rx_desc);
		if (hdr_len > 0) {
			__skb_put(skb, hdr_len);
			TXGBE_CB(skb)->dma_released = true;
			TXGBE_CB(skb)->dma = rx_buffer->dma;
			rx_buffer->dma = 0;
		} else {
			dma_unmap_single(rx_ring->dev,
					 rx_buffer->dma,
					 rx_ring->rx_buf_len,
					 DMA_FROM_DEVICE);
			rx_buffer->dma = 0;
			if (likely(txgbe_test_staterr(rx_desc,
			    TXGBE_RXD_STAT_EOP)))
				goto dma_sync;
			TXGBE_CB(skb)->dma = rx_buffer->page_dma;
			goto add_frag;
		}
	}

	if (txgbe_test_staterr(rx_desc, TXGBE_RXD_STAT_EOP)) {
		if (skb_headlen(skb)) {
			if (TXGBE_CB(skb)->dma_released == true) {
				dma_unmap_single(rx_ring->dev,
						 TXGBE_CB(skb)->dma,
						 rx_ring->rx_buf_len,
						 DMA_FROM_DEVICE);
				TXGBE_CB(skb)->dma = 0;
				TXGBE_CB(skb)->dma_released = false;
			}
		} else
			txgbe_dma_sync_frag(rx_ring, skb);
	}

dma_sync:
	/* we are reusing so sync this buffer for CPU use */
	dma_sync_single_range_for_cpu(rx_ring->dev,
				      rx_buffer->page_dma,
				      rx_buffer->page_offset,
				      txgbe_rx_bufsz(rx_ring),
				      DMA_FROM_DEVICE);
add_frag:
	/* pull page into skb */
	if (txgbe_add_rx_frag(rx_ring, rx_buffer, rx_desc, skb)) {
		/* hand second half of page back to the ring */
		txgbe_reuse_rx_page(rx_ring, rx_buffer);
	} else if (TXGBE_CB(skb)->dma == rx_buffer->page_dma) {
		/* the page has been released from the ring */
		TXGBE_CB(skb)->page_released = true;
	} else {
		/* we are not reusing the buffer so unmap it */
		dma_unmap_page(rx_ring->dev, rx_buffer->page_dma,
			       txgbe_rx_pg_size(rx_ring),
			       DMA_FROM_DEVICE);
	}

	/* clear contents of buffer_info */
	rx_buffer->page = NULL;

	return skb;
}

/**
 * txgbe_clean_rx_irq - Clean completed descriptors from Rx ring - bounce buf
 * @q_vector: structure containing interrupt and ring information
 * @rx_ring: rx descriptor ring to transact packets on
 * @budget: Total limit on number of packets to process
 *
 * This function provides a "bounce buffer" approach to Rx interrupt
 * processing.  The advantage to this is that on systems that have
 * expensive overhead for IOMMU access this provides a means of avoiding
 * it by maintaining the mapping of the page to the syste.
 *
 * Returns amount of work completed.
 **/
static int txgbe_clean_rx_irq(struct txgbe_q_vector *q_vector,
			       struct txgbe_ring *rx_ring,
			       int budget)
{
	unsigned int total_rx_bytes = 0, total_rx_packets = 0;
	u16 cleaned_count = txgbe_desc_unused(rx_ring);

	do {
		union txgbe_rx_desc *rx_desc;
		struct sk_buff *skb;

		/* return some buffers to hardware, one at a time is too slow */
		if (cleaned_count >= TXGBE_RX_BUFFER_WRITE) {
			txgbe_alloc_rx_buffers(rx_ring, cleaned_count);
			cleaned_count = 0;
		}

		rx_desc = TXGBE_RX_DESC(rx_ring, rx_ring->next_to_clean);

		if (!txgbe_test_staterr(rx_desc, TXGBE_RXD_STAT_DD))
			break;

		/* This memory barrier is needed to keep us from reading
		 * any other fields out of the rx_desc until we know the
		 * descriptor has been written back
		 */
		dma_rmb();

		/* retrieve a buffer from the ring */
		if (ring_is_hs_enabled(rx_ring))
			skb = txgbe_fetch_rx_buffer_hs(rx_ring, rx_desc);
		else
			skb = txgbe_fetch_rx_buffer(rx_ring, rx_desc);

		/* exit if we failed to retrieve a buffer */
		if (!skb)
			break;

		cleaned_count++;

		/* place incomplete frames back on ring for completion */
		if (txgbe_is_non_eop(rx_ring, rx_desc, skb))
			continue;

		/* verify the packet layout is correct */
		if (txgbe_cleanup_headers(rx_ring, rx_desc, skb))
			continue;

		/* probably a little skewed due to removing CRC */
		total_rx_bytes += skb->len;

		/* populate checksum, timestamp, VLAN, and protocol */
		txgbe_process_skb_fields(rx_ring, rx_desc, skb);

		txgbe_rx_skb(q_vector, skb);

		/* update budget accounting */
		total_rx_packets++;
	} while (likely(total_rx_packets < budget));

	u64_stats_update_begin(&rx_ring->syncp);
	rx_ring->stats.packets += total_rx_packets;
	rx_ring->stats.bytes += total_rx_bytes;
	u64_stats_update_end(&rx_ring->syncp);
	q_vector->rx.total_packets += total_rx_packets;
	q_vector->rx.total_bytes += total_rx_bytes;

	return total_rx_packets;
}

/**
 * txgbe_configure_msix - Configure MSI-X hardware
 * @adapter: board private structure
 *
 * txgbe_configure_msix sets up the hardware to properly generate MSI-X
 * interrupts.
 **/
static void txgbe_configure_msix(struct txgbe_adapter *adapter)
{
	u16 v_idx;

	/* Populate MSIX to EITR Select */
	if (adapter->num_vfs >= 32) {
		u32 eitrsel = (1 << (adapter->num_vfs - 32)) - 1;
		wr32(&adapter->hw, TXGBE_PX_ITRSEL, eitrsel);
	} else {
		wr32(&adapter->hw, TXGBE_PX_ITRSEL, 0);
	}

	/*
	 * Populate the IVAR table and set the ITR values to the
	 * corresponding register.
	 */
	for (v_idx = 0; v_idx < adapter->num_q_vectors; v_idx++) {
		struct txgbe_q_vector *q_vector = adapter->q_vector[v_idx];
		struct txgbe_ring *ring;

		txgbe_for_each_ring(ring, q_vector->rx)
			txgbe_set_ivar(adapter, 0, ring->reg_idx, v_idx);

		txgbe_for_each_ring(ring, q_vector->tx)
			txgbe_set_ivar(adapter, 1, ring->reg_idx, v_idx);

		txgbe_write_eitr(q_vector);
	}

	txgbe_set_ivar(adapter, -1, 0, v_idx);

	wr32(&adapter->hw, TXGBE_PX_ITR(v_idx), 1950);
}

enum latency_range {
	lowest_latency = 0,
	low_latency = 1,
	bulk_latency = 2,
	latency_invalid = 255
};

/**
 * txgbe_update_itr - update the dynamic ITR value based on statistics
 * @q_vector: structure containing interrupt and ring information
 * @ring_container: structure containing ring performance data
 *
 *      Stores a new ITR value based on packets and byte
 *      counts during the last interrupt.  The advantage of per interrupt
 *      computation is faster updates and more accurate ITR for the current
 *      traffic pattern.  Constants in this function were computed
 *      based on theoretical maximum wire speed and thresholds were set based
 *      on testing data as well as attempting to minimize response time
 *      while increasing bulk throughput.
 *      this functionality is controlled by the InterruptThrottleRate module
 *      parameter (see txgbe_param.c)
 **/
static void txgbe_update_itr(struct txgbe_q_vector *q_vector,
			     struct txgbe_ring_container *ring_container)
{
	int bytes = ring_container->total_bytes;
	int packets = ring_container->total_packets;
	u32 timepassed_us;
	u64 bytes_perint;
	u8 itr_setting = ring_container->itr;

	if (packets == 0)
		return;

	/* simple throttlerate management
	 *   0-10MB/s   lowest (100000 ints/s)
	 *  10-20MB/s   low    (20000 ints/s)
	 *  20-1249MB/s bulk   (12000 ints/s)
	 */
	/* what was last interrupt timeslice? */
	timepassed_us = q_vector->itr >> 2;
	if (timepassed_us == 0)
		return;
	bytes_perint = bytes / timepassed_us; /* bytes/usec */

	switch (itr_setting) {
	case lowest_latency:
		if (bytes_perint > 10) {
			itr_setting = low_latency;
		}
		break;
	case low_latency:
		if (bytes_perint > 20) {
			itr_setting = bulk_latency;
		} else if (bytes_perint <= 10) {
			itr_setting = lowest_latency;
		}
		break;
	case bulk_latency:
		if (bytes_perint <= 20) {
			itr_setting = low_latency;
		}
		break;
	}

	/* clear work counters since we have the values we need */
	ring_container->total_bytes = 0;
	ring_container->total_packets = 0;

	/* write updated itr to ring container */
	ring_container->itr = itr_setting;
}

/**
 * txgbe_write_eitr - write EITR register in hardware specific way
 * @q_vector: structure containing interrupt and ring information
 *
 * This function is made to be called by ethtool and by the driver
 * when it needs to update EITR registers at runtime.  Hardware
 * specific quirks/differences are taken care of here.
 */
void txgbe_write_eitr(struct txgbe_q_vector *q_vector)
{
	struct txgbe_adapter *adapter = q_vector->adapter;
	struct txgbe_hw *hw = &adapter->hw;
	int v_idx = q_vector->v_idx;
	u32 itr_reg = q_vector->itr & TXGBE_MAX_EITR;

	itr_reg |= TXGBE_PX_ITR_CNT_WDIS;

	wr32(hw, TXGBE_PX_ITR(v_idx), itr_reg);
}

static void txgbe_set_itr(struct txgbe_q_vector *q_vector)
{
	u16 new_itr = q_vector->itr;
	u8 current_itr;

	txgbe_update_itr(q_vector, &q_vector->tx);
	txgbe_update_itr(q_vector, &q_vector->rx);

	current_itr = max(q_vector->rx.itr, q_vector->tx.itr);

	switch (current_itr) {
	/* counts and packets in update_itr are dependent on these numbers */
	case lowest_latency:
		new_itr = TXGBE_100K_ITR;
		break;
	case low_latency:
		new_itr = TXGBE_20K_ITR;
		break;
	case bulk_latency:
		new_itr = TXGBE_12K_ITR;
		break;
	default:
		break;
	}

	if (new_itr != q_vector->itr) {
		/* do an exponential smoothing */
		new_itr = (10 * new_itr * q_vector->itr) /
			  ((9 * new_itr) + q_vector->itr);

		/* save the algorithm value here */
		q_vector->itr = new_itr;

		txgbe_write_eitr(q_vector);
	}
}

/**
 * txgbe_check_overtemp_subtask - check for over temperature
 * @adapter: pointer to adapter
 **/
static void txgbe_check_overtemp_subtask(struct txgbe_adapter *adapter)
{
	struct txgbe_hw *hw = &adapter->hw;
	u32 eicr = adapter->interrupt_event;
	s32 temp_state;

	if (test_bit(__TXGBE_DOWN, &adapter->state))
		return;
	if (!(adapter->flags2 & TXGBE_FLAG2_TEMP_SENSOR_CAPABLE))
		return;
	if (!(adapter->flags2 & TXGBE_FLAG2_TEMP_SENSOR_EVENT))
		return;

	adapter->flags2 &= ~TXGBE_FLAG2_TEMP_SENSOR_EVENT;

	/*
	 * Since the warning interrupt is for both ports
	 * we don't have to check if:
	 *  - This interrupt wasn't for our port.
	 *  - We may have missed the interrupt so always have to
	 *    check if we  got a LSC
	 */
	if (!(eicr & TXGBE_PX_MISC_IC_OVER_HEAT))
		return;

	temp_state = TCALL(hw, phy.ops.check_overtemp);
	if (!temp_state || temp_state == TXGBE_NOT_IMPLEMENTED)
		return;

	if (temp_state == TXGBE_ERR_UNDERTEMP &&
		test_bit(__TXGBE_HANGING, &adapter->state)) {
		e_crit(drv, "%s\n", txgbe_underheat_msg);
		wr32m(&adapter->hw, TXGBE_RDB_PB_CTL,
				TXGBE_RDB_PB_CTL_RXEN, TXGBE_RDB_PB_CTL_RXEN);
		netif_carrier_on(adapter->netdev);

		clear_bit(__TXGBE_HANGING, &adapter->state);
	} else if (temp_state == TXGBE_ERR_OVERTEMP &&
		!test_and_set_bit(__TXGBE_HANGING, &adapter->state)) {
		e_crit(drv, "%s\n", txgbe_overheat_msg);
		netif_carrier_off(adapter->netdev);

		wr32m(&adapter->hw, TXGBE_RDB_PB_CTL,
					TXGBE_RDB_PB_CTL_RXEN, 0);
	}

	adapter->interrupt_event = 0;
}

static void txgbe_check_overtemp_event(struct txgbe_adapter *adapter, u32 eicr)
{
	if (!(adapter->flags2 & TXGBE_FLAG2_TEMP_SENSOR_CAPABLE))
		return;

	if (!(eicr & TXGBE_PX_MISC_IC_OVER_HEAT))
		return;

	if (!test_bit(__TXGBE_DOWN, &adapter->state)) {
		adapter->interrupt_event = eicr;
		adapter->flags2 |= TXGBE_FLAG2_TEMP_SENSOR_EVENT;
		txgbe_service_event_schedule(adapter);
	}
}

static void txgbe_check_sfp_event(struct txgbe_adapter *adapter, u32 eicr)
{
	struct txgbe_hw *hw = &adapter->hw;
	u32 eicr_mask = TXGBE_PX_MISC_IC_GPIO;
	u32 reg;

	if (eicr & eicr_mask) {
		if (!test_bit(__TXGBE_DOWN, &adapter->state)) {
			wr32(hw, TXGBE_GPIO_INTMASK, 0xFF);
			reg = rd32(hw, TXGBE_GPIO_INTSTATUS);
			if (reg & TXGBE_GPIO_INTSTATUS_2) {
				adapter->flags2 |= TXGBE_FLAG2_SFP_NEEDS_RESET;
				wr32(hw, TXGBE_GPIO_EOI,
						TXGBE_GPIO_EOI_2);
				adapter->sfp_poll_time = 0;
				txgbe_service_event_schedule(adapter);
			}
			if (reg & TXGBE_GPIO_INTSTATUS_3) {
				adapter->flags |= TXGBE_FLAG_NEED_LINK_CONFIG;
				wr32(hw, TXGBE_GPIO_EOI,
						TXGBE_GPIO_EOI_3);
				txgbe_service_event_schedule(adapter);
			}

			if (reg & TXGBE_GPIO_INTSTATUS_6) {
				wr32(hw, TXGBE_GPIO_EOI,
						TXGBE_GPIO_EOI_6);
				adapter->flags |=
					TXGBE_FLAG_NEED_LINK_CONFIG;
				txgbe_service_event_schedule(adapter);
			}
			wr32(hw, TXGBE_GPIO_INTMASK, 0x0);
		}
	}
}

static void txgbe_check_lsc(struct txgbe_adapter *adapter)
{
	adapter->lsc_int++;
	adapter->flags |= TXGBE_FLAG_NEED_LINK_UPDATE;
	adapter->link_check_timeout = jiffies;
	if (!test_bit(__TXGBE_DOWN, &adapter->state)) {
		txgbe_service_event_schedule(adapter);
	}
}

/**
 * txgbe_irq_enable - Enable default interrupt generation settings
 * @adapter: board private structure
 **/
void txgbe_irq_enable(struct txgbe_adapter *adapter, bool queues, bool flush)
{
	u32 mask = 0;
	struct txgbe_hw *hw = &adapter->hw;
	u8 device_type = hw->subsystem_id & 0xF0;

	/* enable gpio interrupt */
	if (device_type != TXGBE_ID_MAC_XAUI &&
		device_type != TXGBE_ID_MAC_SGMII) {
		mask |= TXGBE_GPIO_INTEN_2;
		mask |= TXGBE_GPIO_INTEN_3;
		mask |= TXGBE_GPIO_INTEN_6;
	}
	wr32(&adapter->hw, TXGBE_GPIO_INTEN, mask);

	if (device_type != TXGBE_ID_MAC_XAUI &&
		device_type != TXGBE_ID_MAC_SGMII) {
		mask = TXGBE_GPIO_INTTYPE_LEVEL_2 | TXGBE_GPIO_INTTYPE_LEVEL_3 |
				TXGBE_GPIO_INTTYPE_LEVEL_6;
	}
	wr32(&adapter->hw, TXGBE_GPIO_INTTYPE_LEVEL, mask);

	/* enable misc interrupt */
	mask = TXGBE_PX_MISC_IEN_MASK;

	if (adapter->flags2 & TXGBE_FLAG2_TEMP_SENSOR_CAPABLE)
		mask |= TXGBE_PX_MISC_IEN_OVER_HEAT;

	if ((adapter->flags & TXGBE_FLAG_FDIR_HASH_CAPABLE) &&
			!(adapter->flags2 & TXGBE_FLAG2_FDIR_REQUIRES_REINIT))
		mask |= TXGBE_PX_MISC_IEN_FLOW_DIR;

	mask |= TXGBE_PX_MISC_IEN_TIMESYNC;

	wr32(&adapter->hw, TXGBE_PX_MISC_IEN, mask);

	/* unmask interrupt */
	txgbe_intr_enable(&adapter->hw, TXGBE_INTR_MISC(adapter));
	if (queues)
		txgbe_intr_enable(&adapter->hw, TXGBE_INTR_QALL(adapter));

	/* flush configuration */
	if (flush)
		TXGBE_WRITE_FLUSH(&adapter->hw);
}

static irqreturn_t txgbe_msix_other(int __always_unused irq, void *data)
{
	struct txgbe_adapter *adapter = data;
	struct txgbe_hw *hw = &adapter->hw;
	u32 eicr;
	u32 ecc;
	u32 value = 0;
	u16 pci_val = 0;

	eicr = txgbe_misc_isb(adapter, TXGBE_ISB_MISC);

	if (BOND_CHECK_LINK_MODE == 1) {
		if (eicr & (TXGBE_PX_MISC_IC_ETH_LKDN)) {
			value = rd32(hw, 0x14404);
			value = value & 0x1;
			if (value == 0) {
				adapter->link_up = false;
				adapter->flags2 |= TXGBE_FLAG2_LINK_DOWN;
				txgbe_service_event_schedule(adapter);
			}
		}
	} else {
		if (eicr & (TXGBE_PX_MISC_IC_ETH_LK | TXGBE_PX_MISC_IC_ETH_LKDN))
			txgbe_check_lsc(adapter);
	}
	if (eicr & TXGBE_PX_MISC_IC_ETH_AN) {
		if (adapter->backplane_an == 1 && (KR_POLLING == 0)) {
			value = txgbe_rd32_epcs(hw, 0x78002);
			value = value & 0x4;
			if (value == 0x4) {
				txgbe_kr_intr_handle(adapter);
				adapter->flags2 |= TXGBE_FLAG2_KR_TRAINING;
				txgbe_service_event_schedule(adapter);
			}
		}
	}

	if (eicr & TXGBE_PX_MISC_IC_PCIE_REQ_ERR) {
		ERROR_REPORT1(TXGBE_ERROR_POLLING,
			"lan id %d,PCIe request error founded.\n", hw->bus.lan_id);

		pci_read_config_word(adapter->pdev, PCI_VENDOR_ID, &pci_val);
		ERROR_REPORT1(TXGBE_ERROR_POLLING, "pci vendor id is 0x%x\n", pci_val);

		pci_read_config_word(adapter->pdev, PCI_COMMAND, &pci_val);
		ERROR_REPORT1(TXGBE_ERROR_POLLING, "pci command reg is 0x%x.\n", pci_val);

		if (hw->bus.lan_id == 0) {
			adapter->flags2 |= TXGBE_FLAG2_PCIE_NEED_RECOVER;
			txgbe_service_event_schedule(adapter);
		} else
			wr32(&adapter->hw, TXGBE_MIS_PF_SM, 1);
	}

	if (eicr & TXGBE_PX_MISC_IC_INT_ERR) {
		e_info(link, "Received unrecoverable ECC Err,"
		       "initiating reset.\n");
		ecc = rd32(hw, TXGBE_MIS_ST);
		if (((ecc & TXGBE_MIS_ST_LAN0_ECC) && (hw->bus.lan_id == 0)) ||
		    ((ecc & TXGBE_MIS_ST_LAN1_ECC) && (hw->bus.lan_id == 1)))
			adapter->flags2 |= TXGBE_FLAG2_PF_RESET_REQUESTED;

		txgbe_service_event_schedule(adapter);
	}
	if (eicr & TXGBE_PX_MISC_IC_DEV_RST) {
		adapter->flags2 |= TXGBE_FLAG2_RESET_INTR_RECEIVED;
		txgbe_service_event_schedule(adapter);
	}
	if ((eicr & TXGBE_PX_MISC_IC_STALL) ||
		(eicr & TXGBE_PX_MISC_IC_ETH_EVENT)) {
		adapter->flags2 |= TXGBE_FLAG2_PF_RESET_REQUESTED;
		txgbe_service_event_schedule(adapter);
	}

	/* Handle Flow Director Full threshold interrupt */
	if (eicr & TXGBE_PX_MISC_IC_FLOW_DIR) {
		int reinit_count = 0;
		int i;
		for (i = 0; i < adapter->num_tx_queues; i++) {
			struct txgbe_ring *ring = adapter->tx_ring[i];
			if (test_and_clear_bit(__TXGBE_TX_FDIR_INIT_DONE,
					      &ring->state))
				reinit_count++;
		}
		if (reinit_count) {
			/* no more flow director interrupts until after init */
			wr32m(hw, TXGBE_PX_MISC_IEN,
				TXGBE_PX_MISC_IEN_FLOW_DIR, 0);
			adapter->flags2 |=
				TXGBE_FLAG2_FDIR_REQUIRES_REINIT;
			txgbe_service_event_schedule(adapter);
		}
	}

	txgbe_check_sfp_event(adapter, eicr);
	txgbe_check_overtemp_event(adapter, eicr);

	if (unlikely(eicr & TXGBE_PX_MISC_IC_TIMESYNC))
		txgbe_ptp_check_pps_event(adapter);

	/* re-enable the original interrupt state, no lsc, no queues */
	if (!test_bit(__TXGBE_DOWN, &adapter->state))
		txgbe_irq_enable(adapter, false, false);

	return IRQ_HANDLED;
}

static irqreturn_t txgbe_msix_clean_rings(int __always_unused irq, void *data)
{
	struct txgbe_q_vector *q_vector = data;

	/* EIAM disabled interrupts (on this vector) for us */

	if (q_vector->rx.ring || q_vector->tx.ring)
		napi_schedule_irqoff(&q_vector->napi);

	return IRQ_HANDLED;
}

/**
 * txgbe_poll - NAPI polling RX/TX cleanup routine
 * @napi: napi struct with our devices info in it
 * @budget: amount of work driver is allowed to do this pass, in packets
 *
 * This function will clean all queues associated with a q_vector.
 **/
int txgbe_poll(struct napi_struct *napi, int budget)
{
	struct txgbe_q_vector *q_vector =
			       container_of(napi, struct txgbe_q_vector, napi);
	struct txgbe_adapter *adapter = q_vector->adapter;
	struct txgbe_ring *ring;
	int per_ring_budget;
	bool clean_complete = true;

	txgbe_for_each_ring(ring, q_vector->tx) {
		if (!txgbe_clean_tx_irq(q_vector, ring))
			clean_complete = false;
	}

	/* Exit if we are called by netpoll */
	if (budget <= 0)
		return budget;

	/* attempt to distribute budget to each queue fairly, but don't allow
	 * the budget to go below 1 because we'll exit polling */
	if (q_vector->rx.count > 1)
		per_ring_budget = max(budget/q_vector->rx.count, 1);
	else
		per_ring_budget = budget;

	txgbe_for_each_ring(ring, q_vector->rx) {
		int cleaned = txgbe_clean_rx_irq(q_vector, ring,
						 per_ring_budget);

		if (cleaned >= per_ring_budget)
			clean_complete = false;
	}

	/* If all work not completed, return budget and keep polling */
	if (!clean_complete)
		return budget;

	/* all work done, exit the polling mode */
	napi_complete(napi);
	if (adapter->rx_itr_setting == 1)
		txgbe_set_itr(q_vector);
	if (!test_bit(__TXGBE_DOWN, &adapter->state))
		txgbe_intr_enable(&adapter->hw,
			TXGBE_INTR_Q(q_vector->v_idx));

	return 0;
}

/**
 * txgbe_request_msix_irqs - Initialize MSI-X interrupts
 * @adapter: board private structure
 *
 * txgbe_request_msix_irqs allocates MSI-X vectors and requests
 * interrupts from the kernel.
 **/
static int txgbe_request_msix_irqs(struct txgbe_adapter *adapter)
{
	struct net_device *netdev = adapter->netdev;
	int vector, err;
	int ri = 0, ti = 0;

	for (vector = 0; vector < adapter->num_q_vectors; vector++) {
		struct txgbe_q_vector *q_vector = adapter->q_vector[vector];
		struct msix_entry *entry = &adapter->msix_entries[vector];

		if (q_vector->tx.ring && q_vector->rx.ring) {
			snprintf(q_vector->name, sizeof(q_vector->name) - 1,
				 "%s-TxRx-%d", netdev->name, ri++);
			ti++;
		} else if (q_vector->rx.ring) {
			snprintf(q_vector->name, sizeof(q_vector->name) - 1,
				 "%s-rx-%d", netdev->name, ri++);
		} else if (q_vector->tx.ring) {
			snprintf(q_vector->name, sizeof(q_vector->name) - 1,
				 "%s-tx-%d", netdev->name, ti++);
		} else {
			/* skip this unused q_vector */
			continue;
		}
		err = request_irq(entry->vector, &txgbe_msix_clean_rings, 0,
				  q_vector->name, q_vector);
		if (err) {
			e_err(probe, "request_irq failed for MSIX interrupt"
			      " '%s' Error: %d\n", q_vector->name, err);
			goto free_queue_irqs;
		}

		/* If Flow Director is enabled, set interrupt affinity */
		if (adapter->flags & TXGBE_FLAG_FDIR_HASH_CAPABLE) {
			/* assign the mask for this irq */
			irq_set_affinity_hint(entry->vector,
					      &q_vector->affinity_mask);
		}
	}

	err = request_irq(adapter->msix_entries[vector].vector,
			  txgbe_msix_other, 0, netdev->name, adapter);
	if (err) {
		e_err(probe, "request_irq for msix_other failed: %d\n", err);
		goto free_queue_irqs;
	}

	return 0;

free_queue_irqs:
	while (vector) {
		vector--;
		irq_set_affinity_hint(adapter->msix_entries[vector].vector,
				      NULL);
		free_irq(adapter->msix_entries[vector].vector,
			 adapter->q_vector[vector]);
	}
	adapter->flags &= ~TXGBE_FLAG_MSIX_ENABLED;
	pci_disable_msix(adapter->pdev);
	kfree(adapter->msix_entries);
	adapter->msix_entries = NULL;
	return err;
}

/**
 * txgbe_intr - legacy mode Interrupt Handler
 * @irq: interrupt number
 * @data: pointer to a network interface device structure
 **/
static irqreturn_t txgbe_intr(int __always_unused irq, void *data)
{
	struct txgbe_adapter *adapter = data;
	struct txgbe_q_vector *q_vector = adapter->q_vector[0];
	struct txgbe_hw *hw = &adapter->hw;
	u32 eicr;
	u32 eicr_misc;
	u32 value ;

	eicr = txgbe_misc_isb(adapter, TXGBE_ISB_VEC0);
	if (!eicr) {
		/*
		 * shared interrupt alert!
		 * the interrupt that we masked before the EICR read.
		 */
		if (!test_bit(__TXGBE_DOWN, &adapter->state))
			txgbe_irq_enable(adapter, true, true);
		return IRQ_NONE;        /* Not our interrupt */
	}
	adapter->isb_mem[TXGBE_ISB_VEC0] = 0;
	if (!(adapter->flags & TXGBE_FLAG_MSI_ENABLED))
		wr32(&(adapter->hw), TXGBE_PX_INTA, 1);

	eicr_misc = txgbe_misc_isb(adapter, TXGBE_ISB_MISC);
	if (eicr_misc & (TXGBE_PX_MISC_IC_ETH_LK | TXGBE_PX_MISC_IC_ETH_LKDN))
		txgbe_check_lsc(adapter);

	if (eicr_misc & TXGBE_PX_MISC_IC_ETH_AN) {
		if (adapter->backplane_an == 1 && (KR_POLLING == 0)) {
			value = txgbe_rd32_epcs(hw, 0x78002);
			value = value & 0x4;
			if (value == 0x4) {
				txgbe_kr_intr_handle(adapter);
				adapter->flags2 |= TXGBE_FLAG2_KR_TRAINING;
				txgbe_service_event_schedule(adapter);
			}
		}
	}

	if (eicr_misc & TXGBE_PX_MISC_IC_INT_ERR) {
		e_info(link, "Received unrecoverable ECC Err,"
			"initiating reset.\n");
		adapter->flags2 |= TXGBE_FLAG2_GLOBAL_RESET_REQUESTED;
		txgbe_service_event_schedule(adapter);
	}

	if (eicr_misc & TXGBE_PX_MISC_IC_DEV_RST) {
		adapter->flags2 |= TXGBE_FLAG2_RESET_INTR_RECEIVED;
		txgbe_service_event_schedule(adapter);
	}
	txgbe_check_sfp_event(adapter, eicr_misc);
	txgbe_check_overtemp_event(adapter, eicr_misc);

	if (unlikely(eicr_misc & TXGBE_PX_MISC_IC_TIMESYNC))
		txgbe_ptp_check_pps_event(adapter);

	adapter->isb_mem[TXGBE_ISB_MISC] = 0;
	/* would disable interrupts here but it is auto disabled */
	napi_schedule_irqoff(&q_vector->napi);

	/*
	 * re-enable link(maybe) and non-queue interrupts, no flush.
	 * txgbe_poll will re-enable the queue interrupts
	 */
	if (!test_bit(__TXGBE_DOWN, &adapter->state))
		txgbe_irq_enable(adapter, false, false);

	return IRQ_HANDLED;
}

/**
 * txgbe_request_irq - initialize interrupts
 * @adapter: board private structure
 *
 * Attempts to configure interrupts using the best available
 * capabilities of the hardware and kernel.
 **/
static int txgbe_request_irq(struct txgbe_adapter *adapter)
{
	struct net_device *netdev = adapter->netdev;
	int err;

	if (adapter->flags & TXGBE_FLAG_MSIX_ENABLED)
		err = txgbe_request_msix_irqs(adapter);
	else if (adapter->flags & TXGBE_FLAG_MSI_ENABLED)
		err = request_irq(adapter->pdev->irq, &txgbe_intr, 0,
				  netdev->name, adapter);
	else
		err = request_irq(adapter->pdev->irq, &txgbe_intr, IRQF_SHARED,
				  netdev->name, adapter);

	if (err)
		e_err(probe, "request_irq failed, Error %d\n", err);

	return err;
}

static void txgbe_free_irq(struct txgbe_adapter *adapter)
{
	int vector;

	if (!(adapter->flags & TXGBE_FLAG_MSIX_ENABLED)) {
		free_irq(adapter->pdev->irq, adapter);
		return;
	}

	for (vector = 0; vector < adapter->num_q_vectors; vector++) {
		struct txgbe_q_vector *q_vector = adapter->q_vector[vector];
		struct msix_entry *entry = &adapter->msix_entries[vector];

		/* free only the irqs that were actually requested */
		if (!q_vector->rx.ring && !q_vector->tx.ring)
			continue;

		/* clear the affinity_mask in the IRQ descriptor */
		irq_set_affinity_hint(entry->vector, NULL);
		free_irq(entry->vector, q_vector);
	}

	free_irq(adapter->msix_entries[vector++].vector, adapter);
}

/**
 * txgbe_irq_disable - Mask off interrupt generation on the NIC
 * @adapter: board private structure
 **/
void txgbe_irq_disable(struct txgbe_adapter *adapter)
{
	wr32(&adapter->hw, TXGBE_PX_MISC_IEN, 0);
	txgbe_intr_disable(&adapter->hw, TXGBE_INTR_ALL);

	TXGBE_WRITE_FLUSH(&adapter->hw);
	if (adapter->flags & TXGBE_FLAG_MSIX_ENABLED) {
		int vector;

		for (vector = 0; vector < adapter->num_q_vectors; vector++)
			synchronize_irq(adapter->msix_entries[vector].vector);

		synchronize_irq(adapter->msix_entries[vector++].vector);
	} else {
		synchronize_irq(adapter->pdev->irq);
	}
}

/**
 * txgbe_configure_msi_and_legacy - Initialize PIN (INTA...) and MSI interrupts
 *
 **/
static void txgbe_configure_msi_and_legacy(struct txgbe_adapter *adapter)
{
	struct txgbe_q_vector *q_vector = adapter->q_vector[0];
	struct txgbe_ring *ring;

	txgbe_write_eitr(q_vector);

	txgbe_for_each_ring(ring, q_vector->rx)
		txgbe_set_ivar(adapter, 0, ring->reg_idx, 0);

	txgbe_for_each_ring(ring, q_vector->tx)
		txgbe_set_ivar(adapter, 1, ring->reg_idx, 0);

	txgbe_set_ivar(adapter, -1, 0, 1);

	e_info(hw, "Legacy interrupt IVAR setup done\n");
}

/**
 * txgbe_configure_tx_ring - Configure Tx ring after Reset
 * @adapter: board private structure
 * @ring: structure containing ring specific data
 *
 * Configure the Tx descriptor ring after a reset.
 **/
void txgbe_configure_tx_ring(struct txgbe_adapter *adapter,
			     struct txgbe_ring *ring)
{
	struct txgbe_hw *hw = &adapter->hw;
	u64 tdba = ring->dma;
	int wait_loop = 10;
	u32 txdctl = TXGBE_PX_TR_CFG_ENABLE;
	u8 reg_idx = ring->reg_idx;

	/* disable queue to avoid issues while updating state */
	wr32(hw, TXGBE_PX_TR_CFG(reg_idx), TXGBE_PX_TR_CFG_SWFLSH);
	TXGBE_WRITE_FLUSH(hw);

	wr32(hw, TXGBE_PX_TR_BAL(reg_idx), tdba & DMA_BIT_MASK(32));
	wr32(hw, TXGBE_PX_TR_BAH(reg_idx), tdba >> 32);

	/* reset head and tail pointers */
	wr32(hw, TXGBE_PX_TR_RP(reg_idx), 0);
	wr32(hw, TXGBE_PX_TR_WP(reg_idx), 0);
	ring->tail = adapter->io_addr + TXGBE_PX_TR_WP(reg_idx);

	/* reset ntu and ntc to place SW in sync with hardwdare */
	ring->next_to_clean = 0;
	ring->next_to_use = 0;

	txdctl |= TXGBE_RING_SIZE(ring) << TXGBE_PX_TR_CFG_TR_SIZE_SHIFT;

	/*
	 * set WTHRESH to encourage burst writeback, it should not be set
	 * higher than 1 when:
	 * - ITR is 0 as it could cause false TX hangs
	 * - ITR is set to > 100k int/sec and BQL is enabled
	 *
	 * In order to avoid issues WTHRESH + PTHRESH should always be equal
	 * to or less than the number of on chip descriptors, which is
	 * currently 40.
	 */

	txdctl |= 0x20 << TXGBE_PX_TR_CFG_WTHRESH_SHIFT;

	/* reinitialize flowdirector state */
	if (adapter->flags & TXGBE_FLAG_FDIR_HASH_CAPABLE) {
		ring->atr_sample_rate = adapter->atr_sample_rate;
		ring->atr_count = 0;
		set_bit(__TXGBE_TX_FDIR_INIT_DONE, &ring->state);
	} else {
		ring->atr_sample_rate = 0;
	}

	/* initialize XPS */
	if (!test_and_set_bit(__TXGBE_TX_XPS_INIT_DONE, &ring->state)) {
		struct txgbe_q_vector *q_vector = ring->q_vector;

		if (q_vector)
			netif_set_xps_queue(adapter->netdev,
					    &q_vector->affinity_mask,
					    ring->queue_index);
	}

	clear_bit(__TXGBE_HANG_CHECK_ARMED, &ring->state);

	/* enable queue */
	wr32(hw, TXGBE_PX_TR_CFG(reg_idx), txdctl);


	/* poll to verify queue is enabled */
	do {
		msleep(1);
		txdctl = rd32(hw, TXGBE_PX_TR_CFG(reg_idx));
	} while (--wait_loop && !(txdctl & TXGBE_PX_TR_CFG_ENABLE));
	if (!wait_loop)
		e_err(drv, "Could not enable Tx Queue %d\n", reg_idx);
}

/**
 * txgbe_configure_tx - Configure Transmit Unit after Reset
 * @adapter: board private structure
 *
 * Configure the Tx unit of the MAC after a reset.
 **/
static void txgbe_configure_tx(struct txgbe_adapter *adapter)
{
	struct txgbe_hw *hw = &adapter->hw;
	u32 i;

	/* TDM_CTL.TE must be before Tx queues are enabled */
	wr32m(hw, TXGBE_TDM_CTL,
		TXGBE_TDM_CTL_TE, TXGBE_TDM_CTL_TE);

	/* Setup the HW Tx Head and Tail descriptor pointers */
	for (i = 0; i < adapter->num_tx_queues; i++)
		txgbe_configure_tx_ring(adapter, adapter->tx_ring[i]);

	wr32m(hw, TXGBE_TSC_BUF_AE, 0x3FF, 0x10);
	/* enable mac transmitter */
	wr32m(hw, TXGBE_MAC_TX_CFG,
		TXGBE_MAC_TX_CFG_TE, TXGBE_MAC_TX_CFG_TE);
}

static void txgbe_enable_rx_drop(struct txgbe_adapter *adapter,
				 struct txgbe_ring *ring)
{
	struct txgbe_hw *hw = &adapter->hw;
	u16 reg_idx = ring->reg_idx;

	u32 srrctl = rd32(hw, TXGBE_PX_RR_CFG(reg_idx));

	srrctl |= TXGBE_PX_RR_CFG_DROP_EN;

	wr32(hw, TXGBE_PX_RR_CFG(reg_idx), srrctl);
}

static void txgbe_disable_rx_drop(struct txgbe_adapter *adapter,
				  struct txgbe_ring *ring)
{
	struct txgbe_hw *hw = &adapter->hw;
	u16 reg_idx = ring->reg_idx;

	u32 srrctl = rd32(hw, TXGBE_PX_RR_CFG(reg_idx));

	srrctl &= ~TXGBE_PX_RR_CFG_DROP_EN;

	wr32(hw, TXGBE_PX_RR_CFG(reg_idx), srrctl);
}

void txgbe_set_rx_drop_en(struct txgbe_adapter *adapter)
{
	int i;

	/*
	 * We should set the drop enable bit if:
	 *  SR-IOV is enabled
	 *   or
	 *  Number of Rx queues > 1 and flow control is disabled
	 *
	 *  This allows us to avoid head of line blocking for security
	 *  and performance reasons.
	 */
	if (adapter->num_vfs || (adapter->num_rx_queues > 1 &&
	  !(adapter->hw.fc.current_mode & txgbe_fc_tx_pause))) {
		for (i = 0; i < adapter->num_rx_queues; i++)
			txgbe_enable_rx_drop(adapter, adapter->rx_ring[i]);
	} else {
		for (i = 0; i < adapter->num_rx_queues; i++)
			txgbe_disable_rx_drop(adapter, adapter->rx_ring[i]);
	}
}

static void txgbe_configure_srrctl(struct txgbe_adapter *adapter,
	struct txgbe_ring *rx_ring)
{
	struct txgbe_hw *hw = &adapter->hw;
	u32 srrctl;
	u16 reg_idx = rx_ring->reg_idx;

	srrctl = rd32m(hw, TXGBE_PX_RR_CFG(reg_idx),
			~(TXGBE_PX_RR_CFG_RR_HDR_SZ |
			  TXGBE_PX_RR_CFG_RR_BUF_SZ |
			  TXGBE_PX_RR_CFG_SPLIT_MODE));
	/* configure header buffer length, needed for RSC */
	srrctl |= TXGBE_RX_HDR_SIZE << TXGBE_PX_RR_CFG_BSIZEHDRSIZE_SHIFT;

	/* configure the packet buffer length */
	srrctl |= txgbe_rx_bufsz(rx_ring) >> TXGBE_PX_RR_CFG_BSIZEPKT_SHIFT;
	if (ring_is_hs_enabled(rx_ring))
		srrctl |= TXGBE_PX_RR_CFG_SPLIT_MODE;

	wr32(hw, TXGBE_PX_RR_CFG(reg_idx), srrctl);
}

/**
 * Return a number of entries in the RSS indirection table
 *
 * @adapter: device handle
 *
 */
u32 txgbe_rss_indir_tbl_entries(struct txgbe_adapter *adapter)
{
	return 128;
}

/**
 * Write the RETA table to HW
 *
 * @adapter: device handle
 *
 * Write the RSS redirection table stored in adapter.rss_indir_tbl[] to HW.
 */
void txgbe_store_reta(struct txgbe_adapter *adapter)
{
	u32 i, reta_entries = txgbe_rss_indir_tbl_entries(adapter);
	struct txgbe_hw *hw = &adapter->hw;
	u32 reta = 0;
	u8 *indir_tbl = adapter->rss_indir_tbl;

	/* Fill out the redirection table as follows:
	 *  - 8 bit wide entries containing 4 bit RSS index
	 */

	/* Write redirection table to HW */
	for (i = 0; i < reta_entries; i++) {
		reta |= indir_tbl[i] << (i & 0x3) * 8;
		if ((i & 3) == 3) {
			wr32(hw, TXGBE_RDB_RSSTBL(i >> 2), reta);
			reta = 0;
		}
	}
}

/**
 * Write the RETA table to HW (for devices in SRIOV mode)
 *
 * @adapter: device handle
 *
 * Write the RSS redirection table stored in adapter.rss_indir_tbl[] to HW.
 */
//static void txgbe_store_vfreta(struct txgbe_adapter *adapter)

static void txgbe_setup_reta(struct txgbe_adapter *adapter)
{
	struct txgbe_hw *hw = &adapter->hw;
	u32 i, j;
	u32 reta_entries = txgbe_rss_indir_tbl_entries(adapter);
	u16 rss_i = adapter->ring_feature[RING_F_RSS].indices;

	/*
	 * Program table for at least 4 queues w/ SR-IOV so that VFs can
	 * make full use of any rings they may have.  We will use the
	 * PSRTYPE register to control how many rings we use within the PF.
	 */
	if ((adapter->flags & TXGBE_FLAG_SRIOV_ENABLED) && (rss_i < 2))
		rss_i = 2;

	/* Fill out hash function seeds */
	for (i = 0; i < 10; i++)
		wr32(hw, TXGBE_RDB_RSSRK(i), adapter->rss_key[i]);

	/* Fill out redirection table */
	memset(adapter->rss_indir_tbl, 0, sizeof(adapter->rss_indir_tbl));

	for (i = 0, j = 0; i < reta_entries; i++, j++) {
		if (j == rss_i)
			j = 0;

		adapter->rss_indir_tbl[i] = j;
	}

	txgbe_store_reta(adapter);
}

static void txgbe_setup_mrqc(struct txgbe_adapter *adapter)
{
	struct txgbe_hw *hw = &adapter->hw;
	u32 rss_field = 0;

	/* VT, DCB and RSS do not coexist at the same time */
	if (adapter->flags & TXGBE_FLAG_VMDQ_ENABLED &&
		adapter->flags & TXGBE_FLAG_DCB_ENABLED)
		return;

	/* Disable indicating checksum in descriptor, enables RSS hash */
	wr32m(hw, TXGBE_PSR_CTL,
		TXGBE_PSR_CTL_PCSD, TXGBE_PSR_CTL_PCSD);

	/* Perform hash on these packet types */
	rss_field = TXGBE_RDB_RA_CTL_RSS_IPV4 |
				TXGBE_RDB_RA_CTL_RSS_IPV4_TCP |
				TXGBE_RDB_RA_CTL_RSS_IPV6 |
				TXGBE_RDB_RA_CTL_RSS_IPV6_TCP;

	if (adapter->flags2 & TXGBE_FLAG2_RSS_FIELD_IPV4_UDP)
		rss_field |= TXGBE_RDB_RA_CTL_RSS_IPV4_UDP;
	if (adapter->flags2 & TXGBE_FLAG2_RSS_FIELD_IPV6_UDP)
		rss_field |= TXGBE_RDB_RA_CTL_RSS_IPV6_UDP;

	netdev_rss_key_fill(adapter->rss_key, sizeof(adapter->rss_key));

	if (adapter->flags & TXGBE_FLAG_SRIOV_ENABLED) {
		/*wait to fix txgbe_setup_vfreta(adapter);*/
		txgbe_setup_reta(adapter);
	} else {
		txgbe_setup_reta(adapter);
	}

	if (adapter->flags2 & TXGBE_FLAG2_RSS_ENABLED)
		rss_field |= TXGBE_RDB_RA_CTL_RSS_EN;

	wr32(hw, TXGBE_RDB_RA_CTL, rss_field);
}

/**
 * txgbe_clear_rscctl - disable RSC for the indicated ring
 * @adapter: address of board private structure
 * @ring: structure containing ring specific data
 **/
void txgbe_clear_rscctl(struct txgbe_adapter *adapter,
			struct txgbe_ring *ring)
{
	struct txgbe_hw *hw = &adapter->hw;
	u8 reg_idx = ring->reg_idx;

	wr32m(hw, TXGBE_PX_RR_CFG(reg_idx),
		TXGBE_PX_RR_CFG_RSC, 0);

	clear_ring_rsc_enabled(ring);
}

/**
 * txgbe_configure_rscctl - enable RSC for the indicated ring
 * @adapter:    address of board private structure
 * @ring: structure containing ring specific data
 **/
void txgbe_configure_rscctl(struct txgbe_adapter *adapter,
			    struct txgbe_ring *ring)
{
	struct txgbe_hw *hw = &adapter->hw;
	u32 rscctrl;
	u8 reg_idx = ring->reg_idx;

	if (!ring_is_rsc_enabled(ring))
		return;

	rscctrl = rd32(hw, TXGBE_PX_RR_CFG(reg_idx));
	rscctrl |= TXGBE_PX_RR_CFG_RSC;
	/*
	 * we must limit the number of descriptors so that the
	 * total size of max desc * buf_len is not greater
	 * than 65536
	 */
#if (MAX_SKB_FRAGS >= 16)
	rscctrl |= TXGBE_PX_RR_CFG_MAX_RSCBUF_16;
#elif (MAX_SKB_FRAGS >= 8)
	rscctrl |= TXGBE_PX_RR_CFG_MAX_RSCBUF_8;
#elif (MAX_SKB_FRAGS >= 4)
	rscctrl |= TXGBE_PX_RR_CFG_MAX_RSCBUF_4;
#else
	rscctrl |= TXGBE_PX_RR_CFG_MAX_RSCBUF_1;
#endif
	wr32(hw, TXGBE_PX_RR_CFG(reg_idx), rscctrl);
}

static void txgbe_rx_desc_queue_enable(struct txgbe_adapter *adapter,
				       struct txgbe_ring *ring)
{
	struct txgbe_hw *hw = &adapter->hw;
	int wait_loop = TXGBE_MAX_RX_DESC_POLL;
	u32 rxdctl;
	u8 reg_idx = ring->reg_idx;

	if (TXGBE_REMOVED(hw->hw_addr))
		return;

	do {
		msleep(1);
		rxdctl = rd32(hw, TXGBE_PX_RR_CFG(reg_idx));
	} while (--wait_loop && !(rxdctl & TXGBE_PX_RR_CFG_RR_EN));

	if (!wait_loop) {
		e_err(drv, "RXDCTL.ENABLE on Rx queue %d "
		      "not set within the polling period\n", reg_idx);
	}
}

/* disable the specified tx ring/queue */
void txgbe_disable_tx_queue(struct txgbe_adapter *adapter,
			    struct txgbe_ring *ring)
{
	struct txgbe_hw *hw = &adapter->hw;
	int wait_loop = TXGBE_MAX_RX_DESC_POLL;
	u32 rxdctl, reg_offset, enable_mask;
	u8 reg_idx = ring->reg_idx;

	if (TXGBE_REMOVED(hw->hw_addr))
		return;

	reg_offset = TXGBE_PX_TR_CFG(reg_idx);
	enable_mask = TXGBE_PX_TR_CFG_ENABLE;

	/* write value back with TDCFG.ENABLE bit cleared */
	wr32m(hw, reg_offset, enable_mask, 0);

	/* the hardware may take up to 100us to really disable the tx queue */
	do {
		udelay(10);
		rxdctl = rd32(hw, reg_offset);
	} while (--wait_loop && (rxdctl & enable_mask));

	if (!wait_loop) {
		e_err(drv, "TDCFG.ENABLE on Tx queue %d not cleared within "
			  "the polling period\n", reg_idx);
	}
}

/* disable the specified rx ring/queue */
void txgbe_disable_rx_queue(struct txgbe_adapter *adapter,
			    struct txgbe_ring *ring)
{
	struct txgbe_hw *hw = &adapter->hw;
	int wait_loop = TXGBE_MAX_RX_DESC_POLL;
	u32 rxdctl;
	u8 reg_idx = ring->reg_idx;

	if (TXGBE_REMOVED(hw->hw_addr))
		return;

	/* write value back with RXDCTL.ENABLE bit cleared */
	wr32m(hw, TXGBE_PX_RR_CFG(reg_idx),
		TXGBE_PX_RR_CFG_RR_EN, 0);

	/* the hardware may take up to 100us to really disable the rx queue */
	do {
		udelay(10);
		rxdctl = rd32(hw, TXGBE_PX_RR_CFG(reg_idx));
	} while (--wait_loop && (rxdctl & TXGBE_PX_RR_CFG_RR_EN));

	if (!wait_loop) {
		e_err(drv, "RXDCTL.ENABLE on Rx queue %d not cleared within "
		      "the polling period\n", reg_idx);
	}
}

void txgbe_configure_rx_ring(struct txgbe_adapter *adapter,
			     struct txgbe_ring *ring)
{
	struct txgbe_hw *hw = &adapter->hw;
	u64 rdba = ring->dma;
	u32 rxdctl;
	u16 reg_idx = ring->reg_idx;

	/* disable queue to avoid issues while updating state */
	rxdctl = rd32(hw, TXGBE_PX_RR_CFG(reg_idx));
	txgbe_disable_rx_queue(adapter, ring);

	wr32(hw, TXGBE_PX_RR_BAL(reg_idx), rdba & DMA_BIT_MASK(32));
	wr32(hw, TXGBE_PX_RR_BAH(reg_idx), rdba >> 32);

	if (ring->count == TXGBE_MAX_RXD)
		rxdctl |= 0 << TXGBE_PX_RR_CFG_RR_SIZE_SHIFT;
	else
		rxdctl |= (ring->count / 128) << TXGBE_PX_RR_CFG_RR_SIZE_SHIFT;

	rxdctl |= 0x1 << TXGBE_PX_RR_CFG_RR_THER_SHIFT;
	wr32(hw, TXGBE_PX_RR_CFG(reg_idx), rxdctl);

	/* reset head and tail pointers */
	wr32(hw, TXGBE_PX_RR_RP(reg_idx), 0);
	wr32(hw, TXGBE_PX_RR_WP(reg_idx), 0);
	ring->tail = adapter->io_addr + TXGBE_PX_RR_WP(reg_idx);

	/* reset ntu and ntc to place SW in sync with hardwdare */
	ring->next_to_clean = 0;
	ring->next_to_use = 0;
	ring->next_to_alloc = 0;

	txgbe_configure_srrctl(adapter, ring);
	/* In ESX, RSCCTL configuration is done by on demand */
	txgbe_configure_rscctl(adapter, ring);

	/* enable receive descriptor ring */
	wr32m(hw, TXGBE_PX_RR_CFG(reg_idx),
		TXGBE_PX_RR_CFG_RR_EN, TXGBE_PX_RR_CFG_RR_EN);

	txgbe_rx_desc_queue_enable(adapter, ring);
	txgbe_alloc_rx_buffers(ring, txgbe_desc_unused(ring));
}

static void txgbe_setup_psrtype(struct txgbe_adapter *adapter)
{
	struct txgbe_hw *hw = &adapter->hw;
	int rss_i = adapter->ring_feature[RING_F_RSS].indices;
	int pool;

	/* PSRTYPE must be initialized in adapters */
	u32 psrtype = TXGBE_RDB_PL_CFG_L4HDR |
				  TXGBE_RDB_PL_CFG_L3HDR |
				  TXGBE_RDB_PL_CFG_L2HDR |
				  TXGBE_RDB_PL_CFG_TUN_OUTER_L2HDR |
				  TXGBE_RDB_PL_CFG_TUN_TUNHDR;

	if (rss_i > 3)
		psrtype |= 2 << 29;
	else if (rss_i > 1)
		psrtype |= 1 << 29;

	for_each_set_bit(pool, &adapter->fwd_bitmask, TXGBE_MAX_MACVLANS)
		wr32(hw, TXGBE_RDB_PL_CFG(VMDQ_P(pool)), psrtype);
}

/**
 * txgbe_configure_bridge_mode - common settings for configuring bridge mode
 * @adapter - the private structure
 *
 * This function's purpose is to remove code duplication and configure some
 * settings require to switch bridge modes.
 **/
static void txgbe_configure_bridge_mode(struct txgbe_adapter *adapter)
{
	struct txgbe_hw *hw = &adapter->hw;
	unsigned int p;

	if (adapter->flags & TXGBE_FLAG_SRIOV_VEPA_BRIDGE_MODE) {
		/* disable Tx loopback, rely on switch hairpin mode */
		wr32m(hw, TXGBE_PSR_CTL,
			TXGBE_PSR_CTL_SW_EN, 0);

		/* enable Rx source address pruning. Note, this requires
		 * replication to be enabled or else it does nothing.
		 */
		for (p = 0; p < adapter->num_vfs; p++) {
			TCALL(hw, mac.ops.set_source_address_pruning, true, p);
		}

		for_each_set_bit(p, &adapter->fwd_bitmask, TXGBE_MAX_MACVLANS) {
			TCALL(hw, mac.ops.set_source_address_pruning, true, VMDQ_P(p));
		}
	} else {
		/* enable Tx loopback for internal VF/PF communication */
		wr32m(hw, TXGBE_PSR_CTL,
			TXGBE_PSR_CTL_SW_EN, TXGBE_PSR_CTL_SW_EN);

		/* disable Rx source address pruning, since we don't expect to
		 * be receiving external loopback of our transmitted frames.
		 */
		for (p = 0; p < adapter->num_vfs; p++) {
			TCALL(hw, mac.ops.set_source_address_pruning, false, p);
		}

		for_each_set_bit(p, &adapter->fwd_bitmask, TXGBE_MAX_MACVLANS) {
			TCALL(hw, mac.ops.set_source_address_pruning, false, VMDQ_P(p));
		}
	}
}

static void txgbe_configure_virtualization(struct txgbe_adapter *adapter)
{
	struct txgbe_hw *hw = &adapter->hw;
	u32 reg_offset, vf_shift;
	u32 i;

	if (!(adapter->flags & TXGBE_FLAG_VMDQ_ENABLED))
		return;

	wr32m(hw, TXGBE_PSR_VM_CTL,
		TXGBE_PSR_VM_CTL_POOL_MASK |
		TXGBE_PSR_VM_CTL_REPLEN,
		VMDQ_P(0) << TXGBE_PSR_VM_CTL_POOL_SHIFT |
		TXGBE_PSR_VM_CTL_REPLEN);

	for_each_set_bit(i, &adapter->fwd_bitmask, TXGBE_MAX_MACVLANS) {
		/* accept untagged packets until a vlan tag is
		 * specifically set for the VMDQ queue/pool
		 */
		wr32m(hw, TXGBE_PSR_VM_L2CTL(i),
			TXGBE_PSR_VM_L2CTL_AUPE, TXGBE_PSR_VM_L2CTL_AUPE);
	}

	vf_shift = VMDQ_P(0) % 32;
	reg_offset = (VMDQ_P(0) >= 32) ? 1 : 0;

	/* Enable only the PF pools for Tx/Rx */
	wr32(hw, TXGBE_RDM_VF_RE(reg_offset), (~0) << vf_shift);
	wr32(hw, TXGBE_RDM_VF_RE(reg_offset ^ 1), reg_offset - 1);
	wr32(hw, TXGBE_TDM_VF_TE(reg_offset), (~0) << vf_shift);
	wr32(hw, TXGBE_TDM_VF_TE(reg_offset ^ 1), reg_offset - 1);

	if (!(adapter->flags & TXGBE_FLAG_SRIOV_ENABLED))
		return;

	/* configure default bridge settings */
	txgbe_configure_bridge_mode(adapter);

	/* Ensure LLDP and FC is set for Ethertype Antispoofing if we will be
	 * calling set_ethertype_anti_spoofing for each VF in loop below.
	 */
	if (hw->mac.ops.set_ethertype_anti_spoofing) {
		wr32(hw,
			TXGBE_PSR_ETYPE_SWC(TXGBE_PSR_ETYPE_SWC_FILTER_LLDP),
			(TXGBE_PSR_ETYPE_SWC_FILTER_EN    | /* enable filter */
			 TXGBE_PSR_ETYPE_SWC_TX_ANTISPOOF |
			 TXGBE_ETH_P_LLDP));       /* LLDP eth procotol type */

		wr32(hw,
			TXGBE_PSR_ETYPE_SWC(TXGBE_PSR_ETYPE_SWC_FILTER_FC),
			(TXGBE_PSR_ETYPE_SWC_FILTER_EN    |
			 TXGBE_PSR_ETYPE_SWC_TX_ANTISPOOF |
			 ETH_P_PAUSE));
	}

	for (i = 0; i < adapter->num_vfs; i++) {
		/* enable ethertype anti spoofing if hw supports it */
		TCALL(hw, mac.ops.set_ethertype_anti_spoofing, true, i);
	}
}

static void txgbe_set_rx_buffer_len(struct txgbe_adapter *adapter)
{
	struct txgbe_hw *hw = &adapter->hw;
	struct net_device *netdev = adapter->netdev;
	u32 max_frame = netdev->mtu + ETH_HLEN + ETH_FCS_LEN;
	struct txgbe_ring *rx_ring;
	int i;
	u32 mhadd;

	/* adjust max frame to be at least the size of a standard frame */
	if (max_frame < (ETH_FRAME_LEN + ETH_FCS_LEN))
		max_frame = (ETH_FRAME_LEN + ETH_FCS_LEN);

	mhadd = rd32(hw, TXGBE_PSR_MAX_SZ);
	if (max_frame != mhadd) {
		wr32(hw, TXGBE_PSR_MAX_SZ, max_frame);
	}

	/*
	 * Setup the HW Rx Head and Tail Descriptor Pointers and
	 * the Base and Length of the Rx Descriptor Ring
	 */
	for (i = 0; i < adapter->num_rx_queues; i++) {
		rx_ring = adapter->rx_ring[i];

		if (adapter->flags & TXGBE_FLAG_RX_HS_ENABLED) {
			rx_ring->rx_buf_len = TXGBE_RX_HDR_SIZE;
			set_ring_hs_enabled(rx_ring);
		} else
			clear_ring_hs_enabled(rx_ring);

		if (adapter->flags2 & TXGBE_FLAG2_RSC_ENABLED)
			set_ring_rsc_enabled(rx_ring);
		else
			clear_ring_rsc_enabled(rx_ring);
	}
}

/**
 * txgbe_configure_rx - Configure Receive Unit after Reset
 * @adapter: board private structure
 *
 * Configure the Rx unit of the MAC after a reset.
 **/
static void txgbe_configure_rx(struct txgbe_adapter *adapter)
{
	struct txgbe_hw *hw = &adapter->hw;
	int i;
	u32 rxctrl, psrctl;

	/* disable receives while setting up the descriptors */
	TCALL(hw, mac.ops.disable_rx);

	txgbe_setup_psrtype(adapter);

	/* enable hw crc stripping */
	wr32m(hw, TXGBE_RSC_CTL,
		TXGBE_RSC_CTL_CRC_STRIP, TXGBE_RSC_CTL_CRC_STRIP);

	/* RSC Setup */
	psrctl = rd32m(hw, TXGBE_PSR_CTL, ~TXGBE_PSR_CTL_RSC_DIS);
	psrctl |= TXGBE_PSR_CTL_RSC_ACK; /* Disable RSC for ACK packets */
	if (!(adapter->flags2 & TXGBE_FLAG2_RSC_ENABLED))
		psrctl |= TXGBE_PSR_CTL_RSC_DIS;
	wr32(hw, TXGBE_PSR_CTL, psrctl);

	/* Program registers for the distribution of queues */
	txgbe_setup_mrqc(adapter);

	/* set_rx_buffer_len must be called before ring initialization */
	txgbe_set_rx_buffer_len(adapter);

	/*
	 * Setup the HW Rx Head and Tail Descriptor Pointers and
	 * the Base and Length of the Rx Descriptor Ring
	 */
	for (i = 0; i < adapter->num_rx_queues; i++)
		txgbe_configure_rx_ring(adapter, adapter->rx_ring[i]);

	rxctrl = rd32(hw, TXGBE_RDB_PB_CTL);

	/* enable all receives */
	rxctrl |= TXGBE_RDB_PB_CTL_RXEN;
	TCALL(hw, mac.ops.enable_rx_dma, rxctrl);
}

static int txgbe_vlan_rx_add_vid(struct net_device *netdev,
				 __always_unused __be16 proto, u16 vid)
{
	struct txgbe_adapter *adapter = netdev_priv(netdev);
	struct txgbe_hw *hw = &adapter->hw;
	int pool_ndx = VMDQ_P(0);

	/* add VID to filter table */
	if (hw->mac.ops.set_vfta) {
		if (vid < VLAN_N_VID)
			set_bit(vid, adapter->active_vlans);
		TCALL(hw, mac.ops.set_vfta, vid, pool_ndx, true);
		if (adapter->flags & TXGBE_FLAG_VMDQ_ENABLED) {
			int i;
			/* enable vlan id for all pools */
			for_each_set_bit(i, &adapter->fwd_bitmask,
					 TXGBE_MAX_MACVLANS)
				TCALL(hw, mac.ops.set_vfta, vid,
					   VMDQ_P(i), true);
		}
	}

	return 0;
}

static int txgbe_vlan_rx_kill_vid(struct net_device *netdev,
				  __always_unused __be16 proto, u16 vid)
{
	struct txgbe_adapter *adapter = netdev_priv(netdev);
	struct txgbe_hw *hw = &adapter->hw;
	int pool_ndx = VMDQ_P(0);

	/* User is not allowed to remove vlan ID 0 */
	if (!vid)
		return 0;

	/* remove VID from filter table */
	if (hw->mac.ops.set_vfta) {
		TCALL(hw, mac.ops.set_vfta, vid, pool_ndx, false);
		if (adapter->flags & TXGBE_FLAG_VMDQ_ENABLED) {
			int i;
			/* remove vlan id from all pools */
			for_each_set_bit(i, &adapter->fwd_bitmask,
					 TXGBE_MAX_MACVLANS)
				TCALL(hw, mac.ops.set_vfta, vid,
					   VMDQ_P(i), false);
		}
	}

	clear_bit(vid, adapter->active_vlans);

	return 0;
}

#ifdef HAVE_8021P_SUPPORT
/**
 * txgbe_vlan_strip_disable - helper to disable vlan tag stripping
 * @adapter: driver data
 */
void txgbe_vlan_strip_disable(struct txgbe_adapter *adapter)
{
	struct txgbe_hw *hw = &adapter->hw;
	int i, j;

	/* leave vlan tag stripping enabled for DCB */
	if (adapter->flags & TXGBE_FLAG_DCB_ENABLED)
		return;

	for (i = 0; i < adapter->num_rx_queues; i++) {
		struct txgbe_ring *ring = adapter->rx_ring[i];
		if (ring->accel)
			continue;
		j = ring->reg_idx;
		wr32m(hw, TXGBE_PX_RR_CFG(j),
			TXGBE_PX_RR_CFG_VLAN, 0);
	}
}

#endif
/**
 * txgbe_vlan_strip_enable - helper to enable vlan tag stripping
 * @adapter: driver data
 */
void txgbe_vlan_strip_enable(struct txgbe_adapter *adapter)
{
	struct txgbe_hw *hw = &adapter->hw;
	int i, j;

	for (i = 0; i < adapter->num_rx_queues; i++) {
		struct txgbe_ring *ring = adapter->rx_ring[i];
		if (ring->accel)
			continue;
		j = ring->reg_idx;
		wr32m(hw, TXGBE_PX_RR_CFG(j),
			TXGBE_PX_RR_CFG_VLAN, TXGBE_PX_RR_CFG_VLAN);
	}
}

void txgbe_vlan_mode(struct net_device *netdev, u32 features)
{
#if defined(HAVE_8021P_SUPPORT)
	struct txgbe_adapter *adapter = netdev_priv(netdev);
#endif
#ifdef HAVE_8021P_SUPPORT
	bool enable;
#endif

#ifdef HAVE_8021P_SUPPORT
	enable = !!(features & (NETIF_F_HW_VLAN_CTAG_RX));

	if (enable)
		/* enable VLAN tag insert/strip */
		txgbe_vlan_strip_enable(adapter);
	else
		/* disable VLAN tag insert/strip */
		txgbe_vlan_strip_disable(adapter);

#endif /* HAVE_8021P_SUPPORT */
}

static void txgbe_restore_vlan(struct txgbe_adapter *adapter)
{
	struct net_device *netdev = adapter->netdev;
	u16 vid;

	txgbe_vlan_mode(netdev, netdev->features);

	for_each_set_bit(vid, adapter->active_vlans, VLAN_N_VID)
		txgbe_vlan_rx_add_vid(netdev, htons(ETH_P_8021Q), vid);
}

static u8 *txgbe_addr_list_itr(struct txgbe_hw __maybe_unused *hw,
			       u8 **mc_addr_ptr, u32 *vmdq)
{
	struct netdev_hw_addr *mc_ptr;
	u8 *addr = *mc_addr_ptr;

	/* VMDQ_P implicitely uses the adapter struct when CONFIG_PCI_IOV is
	 * defined, so we have to wrap the pointer above correctly to prevent
	 * a warning.
	 */
	*vmdq = VMDQ_P(0);

	mc_ptr = container_of(addr, struct netdev_hw_addr, addr[0]);
	if (mc_ptr->list.next) {
		struct netdev_hw_addr *ha;
		ha = list_entry(mc_ptr->list.next, struct netdev_hw_addr, list);
		*mc_addr_ptr = ha->addr;
	} else
		*mc_addr_ptr = NULL;

	return addr;
}

/**
 * txgbe_write_mc_addr_list - write multicast addresses to MTA
 * @netdev: network interface device structure
 *
 * Writes multicast address list to the MTA hash table.
 * Returns: -ENOMEM on failure
 *                0 on no addresses written
 *                X on writing X addresses to MTA
 **/
int txgbe_write_mc_addr_list(struct net_device *netdev)
{
	struct txgbe_adapter *adapter = netdev_priv(netdev);
	struct txgbe_hw *hw = &adapter->hw;
	struct netdev_hw_addr *ha;
	u8  *addr_list = NULL;
	int addr_count = 0;

	if (!hw->mac.ops.update_mc_addr_list)
		return -ENOMEM;

	if (!netif_running(netdev))
		return 0;


	if (netdev_mc_empty(netdev)) {
		TCALL(hw, mac.ops.update_mc_addr_list, NULL, 0,
						txgbe_addr_list_itr, true);
	} else {
		ha = list_first_entry(&netdev->mc.list,
				      struct netdev_hw_addr, list);
		addr_list = ha->addr;
		addr_count = netdev_mc_count(netdev);

		TCALL(hw, mac.ops.update_mc_addr_list, addr_list, addr_count,
						txgbe_addr_list_itr, true);
	}

	return addr_count;
}


void txgbe_full_sync_mac_table(struct txgbe_adapter *adapter)
{
	struct txgbe_hw *hw = &adapter->hw;
	int i;
	for (i = 0; i < hw->mac.num_rar_entries; i++) {
		if (adapter->mac_table[i].state & TXGBE_MAC_STATE_IN_USE) {
			TCALL(hw, mac.ops.set_rar, i,
				adapter->mac_table[i].addr,
				adapter->mac_table[i].pools,
				TXGBE_PSR_MAC_SWC_AD_H_AV);
		} else {
			TCALL(hw, mac.ops.clear_rar, i);
		}
		adapter->mac_table[i].state &= ~(TXGBE_MAC_STATE_MODIFIED);
	}
}

static void txgbe_sync_mac_table(struct txgbe_adapter *adapter)
{
	struct txgbe_hw *hw = &adapter->hw;
	int i;
	for (i = 0; i < hw->mac.num_rar_entries; i++) {
		if (adapter->mac_table[i].state & TXGBE_MAC_STATE_MODIFIED) {
			if (adapter->mac_table[i].state &
					TXGBE_MAC_STATE_IN_USE) {
				TCALL(hw, mac.ops.set_rar, i,
						adapter->mac_table[i].addr,
						adapter->mac_table[i].pools,
						TXGBE_PSR_MAC_SWC_AD_H_AV);
			} else {
				TCALL(hw, mac.ops.clear_rar, i);
			}
			adapter->mac_table[i].state &=
				~(TXGBE_MAC_STATE_MODIFIED);
		}
	}
}

int txgbe_available_rars(struct txgbe_adapter *adapter)
{
	struct txgbe_hw *hw = &adapter->hw;
	u32 i, count = 0;

	for (i = 0; i < hw->mac.num_rar_entries; i++) {
		if (adapter->mac_table[i].state == 0)
			count++;
	}
	return count;
}

/* this function destroys the first RAR entry */
static void txgbe_mac_set_default_filter(struct txgbe_adapter *adapter,
					 u8 *addr)
{
	struct txgbe_hw *hw = &adapter->hw;

	memcpy(&adapter->mac_table[0].addr, addr, ETH_ALEN);
	adapter->mac_table[0].pools = 1ULL << VMDQ_P(0);
	adapter->mac_table[0].state = (TXGBE_MAC_STATE_DEFAULT |
				       TXGBE_MAC_STATE_IN_USE);
	TCALL(hw, mac.ops.set_rar, 0, adapter->mac_table[0].addr,
			    adapter->mac_table[0].pools,
			    TXGBE_PSR_MAC_SWC_AD_H_AV);
}

int txgbe_add_mac_filter(struct txgbe_adapter *adapter, u8 *addr, u16 pool)
{
	struct txgbe_hw *hw = &adapter->hw;
	u32 i;

	if (is_zero_ether_addr(addr))
		return -EINVAL;

	for (i = 0; i < hw->mac.num_rar_entries; i++) {
		if (adapter->mac_table[i].state & TXGBE_MAC_STATE_IN_USE) {
			continue;
		}
		adapter->mac_table[i].state |= (TXGBE_MAC_STATE_MODIFIED |
						TXGBE_MAC_STATE_IN_USE);
		memcpy(adapter->mac_table[i].addr, addr, ETH_ALEN);
		adapter->mac_table[i].pools = (1ULL << pool);
		txgbe_sync_mac_table(adapter);
		return i;
	}
	return -ENOMEM;
}

static void txgbe_flush_sw_mac_table(struct txgbe_adapter *adapter)
{
	u32 i;
	struct txgbe_hw *hw = &adapter->hw;

	for (i = 0; i < hw->mac.num_rar_entries; i++) {
		adapter->mac_table[i].state |= TXGBE_MAC_STATE_MODIFIED;
		adapter->mac_table[i].state &= ~TXGBE_MAC_STATE_IN_USE;
		memset(adapter->mac_table[i].addr, 0, ETH_ALEN);
		adapter->mac_table[i].pools = 0;
	}
	txgbe_sync_mac_table(adapter);
}

int txgbe_del_mac_filter(struct txgbe_adapter *adapter, u8 *addr, u16 pool)
{
	/* search table for addr, if found, set to 0 and sync */
	u32 i;
	struct txgbe_hw *hw = &adapter->hw;

	if (is_zero_ether_addr(addr))
		return -EINVAL;

	for (i = 0; i < hw->mac.num_rar_entries; i++) {
		if (ether_addr_equal(addr, adapter->mac_table[i].addr) &&
		    adapter->mac_table[i].pools | (1ULL << pool)) {
			adapter->mac_table[i].state |= TXGBE_MAC_STATE_MODIFIED;
			adapter->mac_table[i].state &= ~TXGBE_MAC_STATE_IN_USE;
			memset(adapter->mac_table[i].addr, 0, ETH_ALEN);
			adapter->mac_table[i].pools = 0;
			txgbe_sync_mac_table(adapter);
			return 0;
		}
	}
	return -ENOMEM;
}

/**
 * txgbe_write_uc_addr_list - write unicast addresses to RAR table
 * @netdev: network interface device structure
 *
 * Writes unicast address list to the RAR table.
 * Returns: -ENOMEM on failure/insufficient address space
 *                0 on no addresses written
 *                X on writing X addresses to the RAR table
 **/
int txgbe_write_uc_addr_list(struct net_device *netdev, int pool)
{
	struct txgbe_adapter *adapter = netdev_priv(netdev);
	int count = 0;

	/* return ENOMEM indicating insufficient memory for addresses */
	if (netdev_uc_count(netdev) > txgbe_available_rars(adapter))
		return -ENOMEM;

	if (!netdev_uc_empty(netdev)) {
		struct netdev_hw_addr *ha;

		netdev_for_each_uc_addr(ha, netdev) {
			txgbe_del_mac_filter(adapter, ha->addr, pool);
			txgbe_add_mac_filter(adapter, ha->addr, pool);
			count++;
		}
	}
	return count;
}

int txgbe_add_cloud_switcher(struct txgbe_adapter *adapter, u32 key, u16 pool)
{
	struct txgbe_hw *hw = &adapter->hw;

	UNREFERENCED_PARAMETER(pool);

	wr32(hw, TXGBE_PSR_CL_SWC_IDX, 0);
	wr32(hw, TXGBE_PSR_CL_SWC_KEY, key);
	wr32(hw, TXGBE_PSR_CL_SWC_CTL,
		TXGBE_PSR_CL_SWC_CTL_VLD | TXGBE_PSR_CL_SWC_CTL_DST_MSK);
	wr32(hw, TXGBE_PSR_CL_SWC_VM_L, 0x1);
	wr32(hw, TXGBE_PSR_CL_SWC_VM_H, 0x0);

	return 0;
}

int txgbe_del_cloud_switcher(struct txgbe_adapter *adapter, u32 key, u16 pool)
{
	/* search table for addr, if found, set to 0 and sync */
	struct txgbe_hw *hw = &adapter->hw;

	UNREFERENCED_PARAMETER(key);
	UNREFERENCED_PARAMETER(pool);

	wr32(hw, TXGBE_PSR_CL_SWC_IDX, 0);
	wr32(hw, TXGBE_PSR_CL_SWC_CTL, 0);

	return 0;
}

/**
 * txgbe_set_rx_mode - Unicast, Multicast and Promiscuous mode set
 * @netdev: network interface device structure
 *
 * The set_rx_method entry point is called whenever the unicast/multicast
 * address list or the network interface flags are updated.  This routine is
 * responsible for configuring the hardware for proper unicast, multicast and
 * promiscuous mode.
 **/
void txgbe_set_rx_mode(struct net_device *netdev)
{
	struct txgbe_adapter *adapter = netdev_priv(netdev);
	struct txgbe_hw *hw = &adapter->hw;
	u32 fctrl, vmolr, vlnctrl;
	int count;

	/* Check for Promiscuous and All Multicast modes */
	fctrl = rd32m(hw, TXGBE_PSR_CTL,
			~(TXGBE_PSR_CTL_UPE | TXGBE_PSR_CTL_MPE));
	vmolr = rd32m(hw, TXGBE_PSR_VM_L2CTL(VMDQ_P(0)),
			~(TXGBE_PSR_VM_L2CTL_UPE |
			  TXGBE_PSR_VM_L2CTL_MPE |
			  TXGBE_PSR_VM_L2CTL_ROPE |
			  TXGBE_PSR_VM_L2CTL_ROMPE));
	vlnctrl = rd32m(hw, TXGBE_PSR_VLAN_CTL,
			~(TXGBE_PSR_VLAN_CTL_VFE |
			  TXGBE_PSR_VLAN_CTL_CFIEN));

	/* set all bits that we expect to always be set */
	fctrl |= TXGBE_PSR_CTL_BAM | TXGBE_PSR_CTL_MFE;
	vmolr |= TXGBE_PSR_VM_L2CTL_BAM |
		 TXGBE_PSR_VM_L2CTL_AUPE |
		 TXGBE_PSR_VM_L2CTL_VACC;
	vlnctrl |= TXGBE_PSR_VLAN_CTL_VFE;

	hw->addr_ctrl.user_set_promisc = false;
	if (netdev->flags & IFF_PROMISC) {
		hw->addr_ctrl.user_set_promisc = true;
		fctrl |= (TXGBE_PSR_CTL_UPE | TXGBE_PSR_CTL_MPE);
		/* pf don't want packets routing to vf, so clear UPE */
		vmolr |= TXGBE_PSR_VM_L2CTL_MPE;
		vlnctrl &= ~TXGBE_PSR_VLAN_CTL_VFE;
	}

	if (netdev->flags & IFF_ALLMULTI) {
		fctrl |= TXGBE_PSR_CTL_MPE;
		vmolr |= TXGBE_PSR_VM_L2CTL_MPE;
	}

	/* This is useful for sniffing bad packets. */
	if (netdev->features & NETIF_F_RXALL) {
		vmolr |= (TXGBE_PSR_VM_L2CTL_UPE | TXGBE_PSR_VM_L2CTL_MPE);
		vlnctrl &= ~TXGBE_PSR_VLAN_CTL_VFE;
		/* receive bad packets */
		wr32m(hw, TXGBE_RSC_CTL,
			TXGBE_RSC_CTL_SAVE_MAC_ERR,
			TXGBE_RSC_CTL_SAVE_MAC_ERR);
	} else {
		vmolr |= TXGBE_PSR_VM_L2CTL_ROPE | TXGBE_PSR_VM_L2CTL_ROMPE;
	}

	/*
	 * Write addresses to available RAR registers, if there is not
	 * sufficient space to store all the addresses then enable
	 * unicast promiscuous mode
	 */
	count = txgbe_write_uc_addr_list(netdev, VMDQ_P(0));
	if (count < 0) {
		vmolr &= ~TXGBE_PSR_VM_L2CTL_ROPE;
		vmolr |= TXGBE_PSR_VM_L2CTL_UPE;
	}

	/*
	 * Write addresses to the MTA, if the attempt fails
	 * then we should just turn on promiscuous mode so
	 * that we can at least receive multicast traffic
	 */
	count = txgbe_write_mc_addr_list(netdev);
	if (count < 0) {
		vmolr &= ~TXGBE_PSR_VM_L2CTL_ROMPE;
		vmolr |= TXGBE_PSR_VM_L2CTL_MPE;
	}

	wr32(hw, TXGBE_PSR_VLAN_CTL, vlnctrl);
	wr32(hw, TXGBE_PSR_CTL, fctrl);
	wr32(hw, TXGBE_PSR_VM_L2CTL(VMDQ_P(0)), vmolr);

	if (netdev->features & NETIF_F_HW_VLAN_CTAG_RX)
		txgbe_vlan_strip_enable(adapter);
	else
		txgbe_vlan_strip_disable(adapter);

	/* enable cloud switch */
	if (adapter->flags2 & TXGBE_FLAG2_CLOUD_SWITCH_ENABLED) {
		txgbe_add_cloud_switcher(adapter, 0x10, 0);
	}
}

static void txgbe_napi_enable_all(struct txgbe_adapter *adapter)
{
	struct txgbe_q_vector *q_vector;
	int q_idx;

	for (q_idx = 0; q_idx < adapter->num_q_vectors; q_idx++) {
		q_vector = adapter->q_vector[q_idx];
		napi_enable(&q_vector->napi);
	}
}

static void txgbe_napi_disable_all(struct txgbe_adapter *adapter)
{
	struct txgbe_q_vector *q_vector;
	int q_idx;

	for (q_idx = 0; q_idx < adapter->num_q_vectors; q_idx++) {
		q_vector = adapter->q_vector[q_idx];
		napi_disable(&q_vector->napi);
	}
}

void txgbe_clear_vxlan_port(struct txgbe_adapter *adapter)
{
	adapter->vxlan_port = 0;
	if (!(adapter->flags & TXGBE_FLAG_VXLAN_OFFLOAD_CAPABLE))
		return;
	wr32(&adapter->hw, TXGBE_CFG_VXLAN, 0);
}

#define TXGBE_GSO_PARTIAL_FEATURES (NETIF_F_GSO_GRE | \
				    NETIF_F_GSO_GRE_CSUM | \
				    NETIF_F_GSO_IPXIP4 | \
				    NETIF_F_GSO_IPXIP6 | \
				    NETIF_F_GSO_UDP_TUNNEL | \
				    NETIF_F_GSO_UDP_TUNNEL_CSUM)

static inline unsigned long txgbe_tso_features(void)
{
	unsigned long features = 0;

	features |= NETIF_F_TSO;
	features |= NETIF_F_TSO6;
	features |= NETIF_F_GSO_PARTIAL | TXGBE_GSO_PARTIAL_FEATURES;

	return features;
}

static void txgbe_configure_lli(struct txgbe_adapter *adapter)
{
	/* lli should only be enabled with MSI-X and MSI */
	if (!(adapter->flags & TXGBE_FLAG_MSI_ENABLED) &&
	    !(adapter->flags & TXGBE_FLAG_MSIX_ENABLED))
		return;

	if (adapter->lli_etype) {
		wr32(&adapter->hw, TXGBE_RDB_5T_CTL1(0),
				(TXGBE_RDB_5T_CTL1_LLI |
				 TXGBE_RDB_5T_CTL1_SIZE_BP));
		wr32(&adapter->hw, TXGBE_RDB_ETYPE_CLS(0),
				TXGBE_RDB_ETYPE_CLS_LLI);
		wr32(&adapter->hw, TXGBE_PSR_ETYPE_SWC(0),
				(adapter->lli_etype |
				 TXGBE_PSR_ETYPE_SWC_FILTER_EN));
	}

	if (adapter->lli_port) {
		wr32(&adapter->hw, TXGBE_RDB_5T_CTL1(0),
				(TXGBE_RDB_5T_CTL1_LLI |
				 TXGBE_RDB_5T_CTL1_SIZE_BP));
		wr32(&adapter->hw, TXGBE_RDB_5T_CTL0(0),
				(TXGBE_RDB_5T_CTL0_POOL_MASK_EN |
				 (TXGBE_RDB_5T_CTL0_PRIORITY_MASK <<
				  TXGBE_RDB_5T_CTL0_PRIORITY_SHIFT) |
				 (TXGBE_RDB_5T_CTL0_DEST_PORT_MASK <<
				  TXGBE_RDB_5T_CTL0_5TUPLE_MASK_SHIFT)));

		wr32(&adapter->hw, TXGBE_RDB_5T_SDP(0),
			(adapter->lli_port << 16));
	}

	if (adapter->lli_size) {
		wr32(&adapter->hw, TXGBE_RDB_5T_CTL1(0),
				TXGBE_RDB_5T_CTL1_LLI);
		wr32m(&adapter->hw, TXGBE_RDB_LLI_THRE,
				TXGBE_RDB_LLI_THRE_SZ(~0), adapter->lli_size);
		wr32(&adapter->hw, TXGBE_RDB_5T_CTL0(0),
				(TXGBE_RDB_5T_CTL0_POOL_MASK_EN |
				 (TXGBE_RDB_5T_CTL0_PRIORITY_MASK <<
				  TXGBE_RDB_5T_CTL0_PRIORITY_SHIFT) |
				 (TXGBE_RDB_5T_CTL0_5TUPLE_MASK_MASK <<
				  TXGBE_RDB_5T_CTL0_5TUPLE_MASK_SHIFT)));
	}

	if (adapter->lli_vlan_pri) {
		wr32m(&adapter->hw, TXGBE_RDB_LLI_THRE,
			TXGBE_RDB_LLI_THRE_PRIORITY_EN |
			TXGBE_RDB_LLI_THRE_UP(~0),
			TXGBE_RDB_LLI_THRE_PRIORITY_EN |
			(adapter->lli_vlan_pri << TXGBE_RDB_LLI_THRE_UP_SHIFT));
	}
}

/* Additional bittime to account for TXGBE framing */
#define TXGBE_ETH_FRAMING 20

/*
 * txgbe_hpbthresh - calculate high water mark for flow control
 *
 * @adapter: board private structure to calculate for
 * @pb - packet buffer to calculate
 */
static int txgbe_hpbthresh(struct txgbe_adapter *adapter, int pb)
{
	struct txgbe_hw *hw = &adapter->hw;
	struct net_device *dev = adapter->netdev;
	int link, tc, kb, marker;
	u32 dv_id, rx_pba;

	/* Calculate max LAN frame size */
	tc = link = dev->mtu + ETH_HLEN + ETH_FCS_LEN + TXGBE_ETH_FRAMING;

	/* Calculate delay value for device */
	dv_id = TXGBE_DV(link, tc);

	/* Loopback switch introduces additional latency */
	if (adapter->flags & TXGBE_FLAG_SRIOV_ENABLED)
		dv_id += TXGBE_B2BT(tc);

	/* Delay value is calculated in bit times convert to KB */
	kb = TXGBE_BT2KB(dv_id);
	rx_pba = rd32(hw, TXGBE_RDB_PB_SZ(pb))
			>> TXGBE_RDB_PB_SZ_SHIFT;

	marker = rx_pba - kb;

	/* It is possible that the packet buffer is not large enough
	 * to provide required headroom. In this case throw an error
	 * to user and a do the best we can.
	 */
	if (marker < 0) {
		e_warn(drv, "Packet Buffer(%i) can not provide enough"
			    "headroom to suppport flow control."
			    "Decrease MTU or number of traffic classes\n", pb);
		marker = tc + 1;
	}

	return marker;
}

/*
 * txgbe_lpbthresh - calculate low water mark for for flow control
 *
 * @adapter: board private structure to calculate for
 * @pb - packet buffer to calculate
 */
static int txgbe_lpbthresh(struct txgbe_adapter *adapter, int __maybe_unused pb)
{
	struct net_device *dev = adapter->netdev;
	int tc;
	u32 dv_id;

	/* Calculate max LAN frame size */
	tc = dev->mtu + ETH_HLEN + ETH_FCS_LEN;

	/* Calculate delay value for device */
	dv_id = TXGBE_LOW_DV(tc);

	/* Delay value is calculated in bit times convert to KB */
	return TXGBE_BT2KB(dv_id);
}

/*
 * txgbe_pbthresh_setup - calculate and setup high low water marks
 */
static void txgbe_pbthresh_setup(struct txgbe_adapter *adapter)
{
	struct txgbe_hw *hw = &adapter->hw;
	int num_tc = netdev_get_num_tc(adapter->netdev);
	int i;

	if (!num_tc)
		num_tc = 1;


	for (i = 0; i < num_tc; i++) {
		hw->fc.high_water[i] = txgbe_hpbthresh(adapter, i);
		hw->fc.low_water[i] = txgbe_lpbthresh(adapter, i);

		/* Low water marks must not be larger than high water marks */
		if (hw->fc.low_water[i] > hw->fc.high_water[i])
			hw->fc.low_water[i] = 0;
	}

	for (; i < TXGBE_DCB_MAX_TRAFFIC_CLASS; i++)
		hw->fc.high_water[i] = 0;
}

static void txgbe_configure_pb(struct txgbe_adapter *adapter)
{
	struct txgbe_hw *hw = &adapter->hw;
	int hdrm;
	int tc = netdev_get_num_tc(adapter->netdev);

	if (adapter->flags & TXGBE_FLAG_FDIR_HASH_CAPABLE ||
	    adapter->flags & TXGBE_FLAG_FDIR_PERFECT_CAPABLE)
		hdrm = 32 << adapter->fdir_pballoc;
	else
		hdrm = 0;

	TCALL(hw, mac.ops.setup_rxpba, tc, hdrm, PBA_STRATEGY_EQUAL);
	txgbe_pbthresh_setup(adapter);
}

static void txgbe_fdir_filter_restore(struct txgbe_adapter *adapter)
{
	struct txgbe_hw *hw = &adapter->hw;
	struct hlist_node *node;
	struct txgbe_fdir_filter *filter;

	spin_lock(&adapter->fdir_perfect_lock);

	if (!hlist_empty(&adapter->fdir_filter_list))
		txgbe_fdir_set_input_mask(hw, &adapter->fdir_mask,
				adapter->cloud_mode);

	hlist_for_each_entry_safe(filter, node,
				  &adapter->fdir_filter_list, fdir_node) {
		txgbe_fdir_write_perfect_filter(hw,
				&filter->filter,
				filter->sw_idx,
				(filter->action == TXGBE_RDB_FDIR_DROP_QUEUE) ?
				TXGBE_RDB_FDIR_DROP_QUEUE :
				adapter->rx_ring[filter->action]->reg_idx,
				adapter->cloud_mode);
	}

	spin_unlock(&adapter->fdir_perfect_lock);
}

void txgbe_configure_isb(struct txgbe_adapter *adapter)
{
	/* set ISB Address */
	struct txgbe_hw *hw = &adapter->hw;

	wr32(hw, TXGBE_PX_ISB_ADDR_L,
			adapter->isb_dma & DMA_BIT_MASK(32));
	wr32(hw, TXGBE_PX_ISB_ADDR_H, adapter->isb_dma >> 32);
}

void txgbe_configure_port(struct txgbe_adapter *adapter)
{
	struct txgbe_hw *hw = &adapter->hw;
	u32 value, i;
	u8 tcs = netdev_get_num_tc(adapter->netdev);

	if (adapter->flags & TXGBE_FLAG_VMDQ_ENABLED) {
		if (tcs > 4)
			/* 8 TCs */
			value = TXGBE_CFG_PORT_CTL_NUM_TC_8 |
			      TXGBE_CFG_PORT_CTL_NUM_VT_16 |
			      TXGBE_CFG_PORT_CTL_DCB_EN;
		else if (tcs > 1)
			/* 4 TCs */
			value = TXGBE_CFG_PORT_CTL_NUM_TC_4 |
			      TXGBE_CFG_PORT_CTL_NUM_VT_32 |
			      TXGBE_CFG_PORT_CTL_DCB_EN;
		else if (adapter->ring_feature[RING_F_RSS].indices == 4)
			value = TXGBE_CFG_PORT_CTL_NUM_VT_32;
		else /* adapter->ring_feature[RING_F_RSS].indices <= 2 */
			value = TXGBE_CFG_PORT_CTL_NUM_VT_64;
	} else {
		if (tcs > 4)
			value = TXGBE_CFG_PORT_CTL_NUM_TC_8 |
			      TXGBE_CFG_PORT_CTL_DCB_EN;
		else if (tcs > 1)
			value = TXGBE_CFG_PORT_CTL_NUM_TC_4 |
			      TXGBE_CFG_PORT_CTL_DCB_EN;
		else
			value = 0;
	}

	value |= TXGBE_CFG_PORT_CTL_D_VLAN | TXGBE_CFG_PORT_CTL_QINQ;
	wr32m(hw, TXGBE_CFG_PORT_CTL,
		TXGBE_CFG_PORT_CTL_NUM_TC_MASK |
		TXGBE_CFG_PORT_CTL_NUM_VT_MASK |
		TXGBE_CFG_PORT_CTL_DCB_EN |
		TXGBE_CFG_PORT_CTL_D_VLAN |
		TXGBE_CFG_PORT_CTL_QINQ,
		value);

	wr32(hw, TXGBE_CFG_TAG_TPID(0),
			ETH_P_8021Q | ETH_P_8021AD << 16);
	adapter->hw.tpid[0] = ETH_P_8021Q;
	adapter->hw.tpid[1] = ETH_P_8021AD;
	for (i = 1; i < 4; i++)
		wr32(hw, TXGBE_CFG_TAG_TPID(i),
			ETH_P_8021Q | ETH_P_8021Q << 16);
	for (i = 2; i < 8; i++)
		adapter->hw.tpid[i] = ETH_P_8021Q;
}

static void txgbe_configure(struct txgbe_adapter *adapter)
{
	struct txgbe_hw *hw = &adapter->hw;

	txgbe_configure_pb(adapter);

	/*
	 * We must restore virtualization before VLANs or else
	 * the VLVF registers will not be populated
	 */
	txgbe_configure_virtualization(adapter);
	txgbe_configure_port(adapter);

	txgbe_set_rx_mode(adapter->netdev);
	txgbe_restore_vlan(adapter);

	TCALL(hw, mac.ops.disable_sec_rx_path);

	if (adapter->flags & TXGBE_FLAG_FDIR_HASH_CAPABLE) {
		txgbe_init_fdir_signature(&adapter->hw,
						   adapter->fdir_pballoc);
	} else if (adapter->flags & TXGBE_FLAG_FDIR_PERFECT_CAPABLE) {
		txgbe_init_fdir_perfect(&adapter->hw,
								adapter->fdir_pballoc,
								adapter->cloud_mode);
		txgbe_fdir_filter_restore(adapter);
	}

	TCALL(hw, mac.ops.enable_sec_rx_path);

	TCALL(hw, mac.ops.setup_eee,
		(adapter->flags2 & TXGBE_FLAG2_EEE_CAPABLE) &&
		(adapter->flags2 & TXGBE_FLAG2_EEE_ENABLED));

	txgbe_configure_tx(adapter);
	txgbe_configure_rx(adapter);
	txgbe_configure_isb(adapter);
}

static bool txgbe_is_sfp(struct txgbe_hw *hw)
{
	switch (TCALL(hw, mac.ops.get_media_type)) {
	case txgbe_media_type_fiber:
		return true;
	default:
		return false;
	}
}

static bool txgbe_is_backplane(struct txgbe_hw *hw)
{
	switch (TCALL(hw, mac.ops.get_media_type)) {
	case txgbe_media_type_backplane:
		return true;
	default:
		return false;
	}
}

/**
 * txgbe_sfp_link_config - set up SFP+ link
 * @adapter: pointer to private adapter struct
 **/
static void txgbe_sfp_link_config(struct txgbe_adapter *adapter)
{
	/*
	 * We are assuming the worst case scenerio here, and that
	 * is that an SFP was inserted/removed after the reset
	 * but before SFP detection was enabled.  As such the best
	 * solution is to just start searching as soon as we start
	 */

	adapter->flags2 |= TXGBE_FLAG2_SFP_NEEDS_RESET;
	adapter->sfp_poll_time = 0;
}

/**
 * txgbe_non_sfp_link_config - set up non-SFP+ link
 * @hw: pointer to private hardware struct
 *
 * Returns 0 on success, negative on failure
 **/
static int txgbe_non_sfp_link_config(struct txgbe_hw *hw)
{
	u32 speed;
	bool autoneg, link_up = false;
	u32 ret = TXGBE_ERR_LINK_SETUP;

	ret = TCALL(hw, mac.ops.check_link, &speed, &link_up, false);

	if (ret)
		goto link_cfg_out;

	if (link_up)
		return 0;

	if ((hw->subsystem_id & 0xF0) != TXGBE_ID_SFI_XAUI) {
		/* setup external PHY Mac Interface */
		mtdSetMacInterfaceControl(&hw->phy_dev, hw->phy.addr, MTD_MAC_TYPE_XAUI,
			MTD_FALSE, MTD_MAC_SNOOP_OFF,
			0, MTD_MAC_SPEED_1000_MBPS,
			MTD_MAX_MAC_SPEED_LEAVE_UNCHANGED,
			MTD_TRUE, MTD_TRUE);

		speed = hw->phy.autoneg_advertised;
		if (!speed)
			ret = TCALL(hw, mac.ops.get_link_capabilities, &speed,
								&autoneg);
		if (ret)
			goto link_cfg_out;
	} else {
		speed = TXGBE_LINK_SPEED_10GB_FULL;
		autoneg = false;
	}

	ret = TCALL(hw, mac.ops.setup_link, speed, autoneg);

link_cfg_out:
	return ret;
}

/**
 * txgbe_clear_vf_stats_counters - Clear out VF stats after reset
 * @adapter: board private structure
 *
 * On a reset we need to clear out the VF stats or accounting gets
 * messed up because they're not clear on read.
 **/
static void txgbe_clear_vf_stats_counters(struct txgbe_adapter *adapter)
{
	struct txgbe_hw *hw = &adapter->hw;
	int i;

	for (i = 0; i < adapter->num_vfs; i++) {
		adapter->vfinfo[i].last_vfstats.gprc =
			rd32(hw, TXGBE_VX_GPRC(i));
		adapter->vfinfo[i].saved_rst_vfstats.gprc +=
			adapter->vfinfo[i].vfstats.gprc;
		adapter->vfinfo[i].vfstats.gprc = 0;
		adapter->vfinfo[i].last_vfstats.gptc =
			rd32(hw, TXGBE_VX_GPTC(i));
		adapter->vfinfo[i].saved_rst_vfstats.gptc +=
			adapter->vfinfo[i].vfstats.gptc;
		adapter->vfinfo[i].vfstats.gptc = 0;
		adapter->vfinfo[i].last_vfstats.gorc =
			rd32(hw, TXGBE_VX_GORC_LSB(i));
		adapter->vfinfo[i].saved_rst_vfstats.gorc +=
			adapter->vfinfo[i].vfstats.gorc;
		adapter->vfinfo[i].vfstats.gorc = 0;
		adapter->vfinfo[i].last_vfstats.gotc =
			rd32(hw, TXGBE_VX_GOTC_LSB(i));
		adapter->vfinfo[i].saved_rst_vfstats.gotc +=
			adapter->vfinfo[i].vfstats.gotc;
		adapter->vfinfo[i].vfstats.gotc = 0;
		adapter->vfinfo[i].last_vfstats.mprc =
			rd32(hw, TXGBE_VX_MPRC(i));
		adapter->vfinfo[i].saved_rst_vfstats.mprc +=
			adapter->vfinfo[i].vfstats.mprc;
		adapter->vfinfo[i].vfstats.mprc = 0;
	}
}

static void txgbe_setup_gpie(struct txgbe_adapter *adapter)
{
	struct txgbe_hw *hw = &adapter->hw;
	u32 gpie = 0;

	if (adapter->flags & TXGBE_FLAG_MSIX_ENABLED) {
		gpie = TXGBE_PX_GPIE_MODEL;
		/*
		 * use EIAM to auto-mask when MSI-X interrupt is asserted
		 * this saves a register write for every interrupt
		 */
	} else {
		/* legacy interrupts, use EIAM to auto-mask when reading EICR,
		 * specifically only auto mask tx and rx interrupts */
	}

	wr32(hw, TXGBE_PX_GPIE, gpie);
}

static void txgbe_up_complete(struct txgbe_adapter *adapter)
{
	struct txgbe_hw *hw = &adapter->hw;
	int err;
	u32 links_reg;
	u16 value;

	txgbe_get_hw_control(adapter);
	txgbe_setup_gpie(adapter);

	if (adapter->flags & TXGBE_FLAG_MSIX_ENABLED)
		txgbe_configure_msix(adapter);
	else
		txgbe_configure_msi_and_legacy(adapter);

	/* enable the optics for SFP+ fiber */
	TCALL(hw, mac.ops.enable_tx_laser);

	smp_mb__before_atomic();
	clear_bit(__TXGBE_DOWN, &adapter->state);
	txgbe_napi_enable_all(adapter);

	txgbe_configure_lli(adapter);

	if (txgbe_is_sfp(hw)) {
		txgbe_sfp_link_config(adapter);
	} else if (txgbe_is_backplane(hw)) {
		adapter->flags |= TXGBE_FLAG_NEED_LINK_CONFIG;
		txgbe_service_event_schedule(adapter);
	} else {
		err = txgbe_non_sfp_link_config(hw);
		if (err)
			e_err(probe, "link_config FAILED %d\n", err);
	}

	links_reg = rd32(hw, TXGBE_CFG_PORT_ST);
	if (links_reg & TXGBE_CFG_PORT_ST_LINK_UP) {
		if (links_reg & TXGBE_CFG_PORT_ST_LINK_10G) {
			wr32(hw, TXGBE_MAC_TX_CFG,
				(rd32(hw, TXGBE_MAC_TX_CFG) &
				~TXGBE_MAC_TX_CFG_SPEED_MASK) |
				TXGBE_MAC_TX_CFG_SPEED_10G);
		} else if (links_reg & (TXGBE_CFG_PORT_ST_LINK_1G | TXGBE_CFG_PORT_ST_LINK_100M)) {
			wr32(hw, TXGBE_MAC_TX_CFG,
				(rd32(hw, TXGBE_MAC_TX_CFG) &
				~TXGBE_MAC_TX_CFG_SPEED_MASK) |
				TXGBE_MAC_TX_CFG_SPEED_1G);
		}
	}

	/* clear any pending interrupts, may auto mask */
	rd32(hw, TXGBE_PX_IC(0));
	rd32(hw, TXGBE_PX_IC(1));
	rd32(hw, TXGBE_PX_MISC_IC);
	txgbe_irq_enable(adapter, true, true);

	/* enable external PHY interrupt */
	if ((hw->subsystem_id & 0xF0) == TXGBE_ID_XAUI) {
		txgbe_read_mdio(&hw->phy_dev, hw->phy.addr, 0x03, 0x8011, &value);
		/* only enable T unit int */
		txgbe_write_mdio(&hw->phy_dev, hw->phy.addr, 31, 0xf043, 0x1);
		/* active high */
		txgbe_write_mdio(&hw->phy_dev, hw->phy.addr, 31, 0xf041, 0x0);
		/* enable AN complete and link status change int */
		txgbe_write_mdio(&hw->phy_dev, hw->phy.addr, 0x03, 0x8010, 0xc00);
	}

	/* enable transmits */
	netif_tx_start_all_queues(adapter->netdev);

	/* bring the link up in the watchdog, this could race with our first
	 * link up interrupt but shouldn't be a problem */
	adapter->flags |= TXGBE_FLAG_NEED_LINK_UPDATE;
	adapter->link_check_timeout = jiffies;

	mod_timer(&adapter->service_timer, jiffies);
	txgbe_clear_vf_stats_counters(adapter);

	/* Set PF Reset Done bit so PF/VF Mail Ops can work */
	wr32m(hw, TXGBE_CFG_PORT_CTL,
		TXGBE_CFG_PORT_CTL_PFRSTD, TXGBE_CFG_PORT_CTL_PFRSTD);
}

void txgbe_reinit_locked(struct txgbe_adapter *adapter)
{
	WARN_ON(in_interrupt());
	/* put off any impending NetWatchDogTimeout */
	netif_trans_update(adapter->netdev);

	while (test_and_set_bit(__TXGBE_RESETTING, &adapter->state))
		usleep_range(1000, 2000);
	txgbe_down(adapter);
	/*
	 * If SR-IOV enabled then wait a bit before bringing the adapter
	 * back up to give the VFs time to respond to the reset.  The
	 * two second wait is based upon the watchdog timer cycle in
	 * the VF driver.
	 */
	if (adapter->flags & TXGBE_FLAG_SRIOV_ENABLED)
		msleep(2000);
	txgbe_up(adapter);
	clear_bit(__TXGBE_RESETTING, &adapter->state);
}

void txgbe_up(struct txgbe_adapter *adapter)
{
	/* hardware has been reset, we need to reload some things */
	txgbe_configure(adapter);

	txgbe_up_complete(adapter);
}

void txgbe_reset(struct txgbe_adapter *adapter)
{
	struct txgbe_hw *hw = &adapter->hw;
	struct net_device *netdev = adapter->netdev;
	int err;
	u8 old_addr[ETH_ALEN];

	if (TXGBE_REMOVED(hw->hw_addr))
		return;
	/* lock SFP init bit to prevent race conditions with the watchdog */
	while (test_and_set_bit(__TXGBE_IN_SFP_INIT, &adapter->state))
		usleep_range(1000, 2000);

	/* clear all SFP and link config related flags while holding SFP_INIT */
	adapter->flags2 &= ~(TXGBE_FLAG2_SEARCH_FOR_SFP |
			     TXGBE_FLAG2_SFP_NEEDS_RESET);
	adapter->flags &= ~TXGBE_FLAG_NEED_LINK_CONFIG;

	err = TCALL(hw, mac.ops.init_hw);
	switch (err) {
	case 0:
	case TXGBE_ERR_SFP_NOT_PRESENT:
	case TXGBE_ERR_SFP_NOT_SUPPORTED:
		break;
	case TXGBE_ERR_MASTER_REQUESTS_PENDING:
		e_dev_err("master disable timed out\n");
		break;
	case TXGBE_ERR_EEPROM_VERSION:
		/* We are running on a pre-production device, log a warning */
		e_dev_warn("This device is a pre-production adapter/LOM. "
			   "Please be aware there may be issues associated "
			   "with your hardware.  If you are experiencing "
			   "problems please contact your hardware "
			   "representative who provided you with this "
			   "hardware.\n");
		break;
	default:
		e_dev_err("Hardware Error: %d\n", err);
	}

	clear_bit(__TXGBE_IN_SFP_INIT, &adapter->state);
	/* do not flush user set addresses */
	memcpy(old_addr, &adapter->mac_table[0].addr, netdev->addr_len);
	txgbe_flush_sw_mac_table(adapter);
	txgbe_mac_set_default_filter(adapter, old_addr);

	/* update SAN MAC vmdq pool selection */
	TCALL(hw, mac.ops.set_vmdq_san_mac, VMDQ_P(0));

	/* Clear saved DMA coalescing values except for watchdog_timer */
	hw->mac.dmac_config.fcoe_en = false;
	hw->mac.dmac_config.link_speed = 0;
	hw->mac.dmac_config.fcoe_tc = 0;
	hw->mac.dmac_config.num_tcs = 0;

	if (test_bit(__TXGBE_PTP_RUNNING, &adapter->state))
		txgbe_ptp_reset(adapter);
}

/**
 * txgbe_clean_rx_ring - Free Rx Buffers per Queue
 * @rx_ring: ring to free buffers from
 **/
static void txgbe_clean_rx_ring(struct txgbe_ring *rx_ring)
{
	struct device *dev = rx_ring->dev;
	unsigned long size;
	u16 i;

	/* ring already cleared, nothing to do */
	if (!rx_ring->rx_buffer_info)
		return;

	/* Free all the Rx ring sk_buffs */
	for (i = 0; i < rx_ring->count; i++) {
		struct txgbe_rx_buffer *rx_buffer = &rx_ring->rx_buffer_info[i];
		if (rx_buffer->dma) {
			dma_unmap_single(dev,
					 rx_buffer->dma,
					 rx_ring->rx_buf_len,
					 DMA_FROM_DEVICE);
			rx_buffer->dma = 0;
		}

		if (rx_buffer->skb) {
			struct sk_buff *skb = rx_buffer->skb;
			if (TXGBE_CB(skb)->dma_released) {
				dma_unmap_single(dev,
						TXGBE_CB(skb)->dma,
						rx_ring->rx_buf_len,
						DMA_FROM_DEVICE);
				TXGBE_CB(skb)->dma = 0;
				TXGBE_CB(skb)->dma_released = false;
			}

			if (TXGBE_CB(skb)->page_released)
				dma_unmap_page(dev,
					       TXGBE_CB(skb)->dma,
					       txgbe_rx_bufsz(rx_ring),
					       DMA_FROM_DEVICE);
			dev_kfree_skb(skb);
			rx_buffer->skb = NULL;
		}

		if (!rx_buffer->page)
			continue;

		dma_unmap_page(dev, rx_buffer->page_dma,
			       txgbe_rx_pg_size(rx_ring),
			       DMA_FROM_DEVICE);

		__free_pages(rx_buffer->page,
				     txgbe_rx_pg_order(rx_ring));
		rx_buffer->page = NULL;
	}

	size = sizeof(struct txgbe_rx_buffer) * rx_ring->count;
	memset(rx_ring->rx_buffer_info, 0, size);

	/* Zero out the descriptor ring */
	memset(rx_ring->desc, 0, rx_ring->size);

	rx_ring->next_to_alloc = 0;
	rx_ring->next_to_clean = 0;
	rx_ring->next_to_use = 0;
}

/**
 * txgbe_clean_tx_ring - Free Tx Buffers
 * @tx_ring: ring to be cleaned
 **/
static void txgbe_clean_tx_ring(struct txgbe_ring *tx_ring)
{
	struct txgbe_tx_buffer *tx_buffer_info;
	unsigned long size;
	u16 i;

	/* ring already cleared, nothing to do */
	if (!tx_ring->tx_buffer_info)
		return;

	/* Free all the Tx ring sk_buffs */
	for (i = 0; i < tx_ring->count; i++) {
		tx_buffer_info = &tx_ring->tx_buffer_info[i];
		txgbe_unmap_and_free_tx_resource(tx_ring, tx_buffer_info);
	}

	netdev_tx_reset_queue(txring_txq(tx_ring));

	size = sizeof(struct txgbe_tx_buffer) * tx_ring->count;
	memset(tx_ring->tx_buffer_info, 0, size);

	/* Zero out the descriptor ring */
	memset(tx_ring->desc, 0, tx_ring->size);
}

/**
 * txgbe_clean_all_rx_rings - Free Rx Buffers for all queues
 * @adapter: board private structure
 **/
static void txgbe_clean_all_rx_rings(struct txgbe_adapter *adapter)
{
	int i;

	for (i = 0; i < adapter->num_rx_queues; i++)
		txgbe_clean_rx_ring(adapter->rx_ring[i]);
}

/**
 * txgbe_clean_all_tx_rings - Free Tx Buffers for all queues
 * @adapter: board private structure
 **/
static void txgbe_clean_all_tx_rings(struct txgbe_adapter *adapter)
{
	int i;

	for (i = 0; i < adapter->num_tx_queues; i++)
		txgbe_clean_tx_ring(adapter->tx_ring[i]);
}

static void txgbe_fdir_filter_exit(struct txgbe_adapter *adapter)
{
	struct hlist_node *node;
	struct txgbe_fdir_filter *filter;

	spin_lock(&adapter->fdir_perfect_lock);

	hlist_for_each_entry_safe(filter, node,
				  &adapter->fdir_filter_list, fdir_node) {
		hlist_del(&filter->fdir_node);
		kfree(filter);
	}
	adapter->fdir_filter_count = 0;

	spin_unlock(&adapter->fdir_perfect_lock);
}

void txgbe_disable_device(struct txgbe_adapter *adapter)
{
	struct net_device *netdev = adapter->netdev;
	struct txgbe_hw *hw = &adapter->hw;

	u32 i;

	/* signal that we are down to the interrupt handler */
	if (test_and_set_bit(__TXGBE_DOWN, &adapter->state))
		return; /* do nothing if already down */

	txgbe_disable_pcie_master(hw);
	/* disable receives */
	TCALL(hw, mac.ops.disable_rx);

	/* disable all enabled rx queues */
	for (i = 0; i < adapter->num_rx_queues; i++)
		/* this call also flushes the previous write */
		txgbe_disable_rx_queue(adapter, adapter->rx_ring[i]);

	netif_tx_stop_all_queues(netdev);

	/* call carrier off first to avoid false dev_watchdog timeouts */
	netif_carrier_off(netdev);
	netif_tx_disable(netdev);

	txgbe_irq_disable(adapter);

	txgbe_napi_disable_all(adapter);

	adapter->flags2 &= ~(TXGBE_FLAG2_FDIR_REQUIRES_REINIT |
			     TXGBE_FLAG2_PF_RESET_REQUESTED |
			     TXGBE_FLAG2_DEV_RESET_REQUESTED |
			     TXGBE_FLAG2_GLOBAL_RESET_REQUESTED);
	adapter->flags &= ~TXGBE_FLAG_NEED_LINK_UPDATE;

	del_timer_sync(&adapter->service_timer);

	if (adapter->num_vfs) {
		/* Clear EITR Select mapping */
		wr32(&adapter->hw, TXGBE_PX_ITRSEL, 0);

		/* Mark all the VFs as inactive */
		for (i = 0 ; i < adapter->num_vfs; i++)
			adapter->vfinfo[i].clear_to_send = 0;

		/* ping all the active vfs to let them know we are going down */

		/* Disable all VFTE/VFRE TX/RX */
	}

	if (!(((hw->subsystem_device_id & TXGBE_NCSI_MASK) == TXGBE_NCSI_SUP) ||
		((hw->subsystem_device_id & TXGBE_WOL_MASK) == TXGBE_WOL_SUP))) {
		/* disable mac transmiter */
		wr32m(hw, TXGBE_MAC_TX_CFG,
			TXGBE_MAC_TX_CFG_TE, 0);
	}
	/* disable transmits in the hardware now that interrupts are off */
	for (i = 0; i < adapter->num_tx_queues; i++) {
		u8 reg_idx = adapter->tx_ring[i]->reg_idx;
		wr32(hw, TXGBE_PX_TR_CFG(reg_idx),
				TXGBE_PX_TR_CFG_SWFLSH);
	}

	/* Disable the Tx DMA engine */
	wr32m(hw, TXGBE_TDM_CTL, TXGBE_TDM_CTL_TE, 0);
}


void txgbe_down(struct txgbe_adapter *adapter)
{
	struct txgbe_hw *hw = &adapter->hw;
	txgbe_disable_device(adapter);

	txgbe_reset(adapter);

	if (!(((hw->subsystem_device_id & TXGBE_NCSI_MASK) == TXGBE_NCSI_SUP)))
		/* power down the optics for SFP+ fiber */
		TCALL(&adapter->hw, mac.ops.disable_tx_laser);

	txgbe_clean_all_tx_rings(adapter);
	txgbe_clean_all_rx_rings(adapter);
}

/**
 *  txgbe_init_shared_code - Initialize the shared code
 *  @hw: pointer to hardware structure
 *
 *  This will assign function pointers and assign the MAC type and PHY code.
 *  Does not touch the hardware. This function must be called prior to any
 *  other function in the shared code. The txgbe_hw structure should be
 *  memset to 0 prior to calling this function.  The following fields in
 *  hw structure should be filled in prior to calling this function:
 *  hw_addr, back, device_id, vendor_id, subsystem_device_id,
 *  subsystem_vendor_id, and revision_id
 **/
s32 txgbe_init_shared_code(struct txgbe_hw *hw)
{
	s32 status;

	DEBUGFUNC("\n");

	status = txgbe_init_ops(hw);
	return status;
}

/**
 * txgbe_sw_init - Initialize general software structures (struct txgbe_adapter)
 * @adapter: board private structure to initialize
 *
 * txgbe_sw_init initializes the Adapter private data structure.
 * Fields are initialized based on PCI device information and
 * OS network device settings (MTU size).
 **/
static const u32 def_rss_key[10] = {
	0xE291D73D, 0x1805EC6C, 0x2A94B30D,
	0xA54F2BEC, 0xEA49AF7C, 0xE214AD3D, 0xB855AABE,
	0x6A3E67EA, 0x14364D17, 0x3BED200D
};

static int txgbe_sw_init(struct txgbe_adapter *adapter)
{
	struct txgbe_hw *hw = &adapter->hw;
	struct pci_dev *pdev = adapter->pdev;
	int err;
	unsigned int fdir;

	/* PCI config space info */
	hw->vendor_id = pdev->vendor;
	hw->device_id = pdev->device;
	pci_read_config_byte(pdev, PCI_REVISION_ID, &hw->revision_id);
	if (hw->revision_id == TXGBE_FAILED_READ_CFG_BYTE &&
	    txgbe_check_cfg_remove(hw, pdev)) {
		e_err(probe, "read of revision id failed\n");
		err = -ENODEV;
		goto out;
	}
	hw->subsystem_vendor_id = pdev->subsystem_vendor;
	hw->subsystem_device_id = pdev->subsystem_device;

	pci_read_config_word(pdev, PCI_SUBSYSTEM_ID, &hw->subsystem_id);
	if (hw->subsystem_id == TXGBE_FAILED_READ_CFG_WORD) {
		e_err(probe, "read of subsystem id failed\n");
		err = -ENODEV;
		goto out;
	}

	err = txgbe_init_shared_code(hw);
	if (err) {
		e_err(probe, "init_shared_code failed: %d\n", err);
		goto out;
	}
	adapter->mac_table = kzalloc(sizeof(struct txgbe_mac_addr) *
				     hw->mac.num_rar_entries,
				     GFP_ATOMIC);
	if (!adapter->mac_table) {
		err = TXGBE_ERR_OUT_OF_MEM;
		e_err(probe, "mac_table allocation failed: %d\n", err);
		goto out;
	}

	memcpy(adapter->rss_key, def_rss_key, sizeof(def_rss_key));

	/* Set common capability flags and settings */
	adapter->flags2 |= TXGBE_FLAG2_RSC_CAPABLE;
	fdir = min_t(int, TXGBE_MAX_FDIR_INDICES, num_online_cpus());
	adapter->ring_feature[RING_F_FDIR].limit = fdir;
	adapter->max_q_vectors = TXGBE_MAX_MSIX_Q_VECTORS_SAPPHIRE;

	/* Set MAC specific capability flags and exceptions */
	adapter->flags |= TXGBE_FLAGS_SP_INIT;
	adapter->flags2 |= TXGBE_FLAG2_TEMP_SENSOR_CAPABLE;
	hw->phy.smart_speed = txgbe_smart_speed_off;
	adapter->flags2 |= TXGBE_FLAG2_EEE_CAPABLE;

	/* n-tuple support exists, always init our spinlock */
	spin_lock_init(&adapter->fdir_perfect_lock);

	TCALL(hw, mbx.ops.init_params);

	/* default flow control settings */
	hw->fc.requested_mode = txgbe_fc_full;
	hw->fc.current_mode = txgbe_fc_full;    /* init for ethtool output */

	adapter->last_lfc_mode = hw->fc.current_mode;
	hw->fc.pause_time = TXGBE_DEFAULT_FCPAUSE;
	hw->fc.send_xon = true;
	hw->fc.disable_fc_autoneg = false;

	/* set default ring sizes */
	adapter->tx_ring_count = TXGBE_DEFAULT_TXD;
	adapter->rx_ring_count = TXGBE_DEFAULT_RXD;

	/* set default work limits */
	adapter->tx_work_limit = TXGBE_DEFAULT_TX_WORK;
	adapter->rx_work_limit = TXGBE_DEFAULT_RX_WORK;

	adapter->tx_timeout_recovery_level = 0;

	/* PF holds first pool slot */
	adapter->num_vmdqs = 1;
	set_bit(0, &adapter->fwd_bitmask);
	set_bit(__TXGBE_DOWN, &adapter->state);
out:
	return err;
}

/**
 * txgbe_setup_tx_resources - allocate Tx resources (Descriptors)
 * @tx_ring:    tx descriptor ring (for a specific queue) to setup
 *
 * Return 0 on success, negative on failure
 **/
int txgbe_setup_tx_resources(struct txgbe_ring *tx_ring)
{
	struct device *dev = tx_ring->dev;
	int orig_node = dev_to_node(dev);
	int numa_node = -1;
	int size;

	size = sizeof(struct txgbe_tx_buffer) * tx_ring->count;

	if (tx_ring->q_vector)
		numa_node = tx_ring->q_vector->numa_node;

	tx_ring->tx_buffer_info = vzalloc_node(size, numa_node);
	if (!tx_ring->tx_buffer_info)
		tx_ring->tx_buffer_info = vzalloc(size);
	if (!tx_ring->tx_buffer_info)
		goto err;

	/* round up to nearest 4K */
	tx_ring->size = tx_ring->count * sizeof(union txgbe_tx_desc);
	tx_ring->size = ALIGN(tx_ring->size, 4096);

	set_dev_node(dev, numa_node);
	tx_ring->desc = dma_alloc_coherent(dev,
					   tx_ring->size,
					   &tx_ring->dma,
					   GFP_KERNEL);
	set_dev_node(dev, orig_node);
	if (!tx_ring->desc)
		tx_ring->desc = dma_alloc_coherent(dev, tx_ring->size,
						   &tx_ring->dma, GFP_KERNEL);
	if (!tx_ring->desc)
		goto err;

	return 0;

err:
	vfree(tx_ring->tx_buffer_info);
	tx_ring->tx_buffer_info = NULL;
	dev_err(dev, "Unable to allocate memory for the Tx descriptor ring\n");
	return -ENOMEM;
}

/**
 * txgbe_setup_all_tx_resources - allocate all queues Tx resources
 * @adapter: board private structure
 *
 * If this function returns with an error, then it's possible one or
 * more of the rings is populated (while the rest are not).  It is the
 * callers duty to clean those orphaned rings.
 *
 * Return 0 on success, negative on failure
 **/
static int txgbe_setup_all_tx_resources(struct txgbe_adapter *adapter)
{
	int i, err = 0;

	for (i = 0; i < adapter->num_tx_queues; i++) {
		err = txgbe_setup_tx_resources(adapter->tx_ring[i]);
		if (!err)
			continue;

		e_err(probe, "Allocation for Tx Queue %u failed\n", i);
		goto err_setup_tx;
	}

	return 0;
err_setup_tx:
	/* rewind the index freeing the rings as we go */
	while (i--)
		txgbe_free_tx_resources(adapter->tx_ring[i]);
	return err;
}

/**
 * txgbe_setup_rx_resources - allocate Rx resources (Descriptors)
 * @rx_ring:    rx descriptor ring (for a specific queue) to setup
 *
 * Returns 0 on success, negative on failure
 **/
int txgbe_setup_rx_resources(struct txgbe_ring *rx_ring)
{
	struct device *dev = rx_ring->dev;
	int orig_node = dev_to_node(dev);
	int numa_node = -1;
	int size;

	size = sizeof(struct txgbe_rx_buffer) * rx_ring->count;

	if (rx_ring->q_vector)
		numa_node = rx_ring->q_vector->numa_node;

	rx_ring->rx_buffer_info = vzalloc_node(size, numa_node);
	if (!rx_ring->rx_buffer_info)
		rx_ring->rx_buffer_info = vzalloc(size);
	if (!rx_ring->rx_buffer_info)
		goto err;

	/* Round up to nearest 4K */
	rx_ring->size = rx_ring->count * sizeof(union txgbe_rx_desc);
	rx_ring->size = ALIGN(rx_ring->size, 4096);

	set_dev_node(dev, numa_node);
	rx_ring->desc = dma_alloc_coherent(dev,
					   rx_ring->size,
					   &rx_ring->dma,
					   GFP_KERNEL);
	set_dev_node(dev, orig_node);
	if (!rx_ring->desc)
		rx_ring->desc = dma_alloc_coherent(dev, rx_ring->size,
						   &rx_ring->dma, GFP_KERNEL);
	if (!rx_ring->desc)
		goto err;

	return 0;
err:
	vfree(rx_ring->rx_buffer_info);
	rx_ring->rx_buffer_info = NULL;
	dev_err(dev, "Unable to allocate memory for the Rx descriptor ring\n");
	return -ENOMEM;
}

/**
 * txgbe_setup_all_rx_resources - allocate all queues Rx resources
 * @adapter: board private structure
 *
 * If this function returns with an error, then it's possible one or
 * more of the rings is populated (while the rest are not).  It is the
 * callers duty to clean those orphaned rings.
 *
 * Return 0 on success, negative on failure
 **/
static int txgbe_setup_all_rx_resources(struct txgbe_adapter *adapter)
{
	int i, err = 0;

	for (i = 0; i < adapter->num_rx_queues; i++) {
		err = txgbe_setup_rx_resources(adapter->rx_ring[i]);
		if (!err) {
			continue;
		}

		e_err(probe, "Allocation for Rx Queue %u failed\n", i);
		goto err_setup_rx;
	}

		return 0;
err_setup_rx:
	/* rewind the index freeing the rings as we go */
	while (i--)
		txgbe_free_rx_resources(adapter->rx_ring[i]);
	return err;
}

/**
 * txgbe_setup_isb_resources - allocate interrupt status resources
 * @adapter: board private structure
 *
 * Return 0 on success, negative on failure
 **/
static int txgbe_setup_isb_resources(struct txgbe_adapter *adapter)
{
	struct device *dev = pci_dev_to_dev(adapter->pdev);

	adapter->isb_mem = dma_alloc_coherent(dev,
					   sizeof(u32) * TXGBE_ISB_MAX,
					   &adapter->isb_dma,
					   GFP_KERNEL);
	if (!adapter->isb_mem)
		return -ENOMEM;
	memset(adapter->isb_mem, 0, sizeof(u32) * TXGBE_ISB_MAX);
	return 0;
}

/**
 * txgbe_free_isb_resources - allocate all queues Rx resources
 * @adapter: board private structure
 *
 * Return 0 on success, negative on failure
 **/
static void txgbe_free_isb_resources(struct txgbe_adapter *adapter)
{
	struct device *dev = pci_dev_to_dev(adapter->pdev);

	dma_free_coherent(dev, sizeof(u32) * TXGBE_ISB_MAX,
			  adapter->isb_mem, adapter->isb_dma);
	adapter->isb_mem = NULL;
}

/**
 * txgbe_free_tx_resources - Free Tx Resources per Queue
 * @tx_ring: Tx descriptor ring for a specific queue
 *
 * Free all transmit software resources
 **/
void txgbe_free_tx_resources(struct txgbe_ring *tx_ring)
{
	txgbe_clean_tx_ring(tx_ring);

	vfree(tx_ring->tx_buffer_info);
	tx_ring->tx_buffer_info = NULL;

	/* if not set, then don't free */
	if (!tx_ring->desc)
		return;

	dma_free_coherent(tx_ring->dev, tx_ring->size,
			  tx_ring->desc, tx_ring->dma);
	tx_ring->desc = NULL;
}

/**
 * txgbe_free_all_tx_resources - Free Tx Resources for All Queues
 * @adapter: board private structure
 *
 * Free all transmit software resources
 **/
static void txgbe_free_all_tx_resources(struct txgbe_adapter *adapter)
{
	int i;

	for (i = 0; i < adapter->num_tx_queues; i++)
		txgbe_free_tx_resources(adapter->tx_ring[i]);
}

/**
 * txgbe_free_rx_resources - Free Rx Resources
 * @rx_ring: ring to clean the resources from
 *
 * Free all receive software resources
 **/
void txgbe_free_rx_resources(struct txgbe_ring *rx_ring)
{
	txgbe_clean_rx_ring(rx_ring);

	vfree(rx_ring->rx_buffer_info);
	rx_ring->rx_buffer_info = NULL;

	/* if not set, then don't free */
	if (!rx_ring->desc)
		return;

	dma_free_coherent(rx_ring->dev, rx_ring->size,
			  rx_ring->desc, rx_ring->dma);

	rx_ring->desc = NULL;
}

/**
 * txgbe_free_all_rx_resources - Free Rx Resources for All Queues
 * @adapter: board private structure
 *
 * Free all receive software resources
 **/
static void txgbe_free_all_rx_resources(struct txgbe_adapter *adapter)
{
	int i;

	for (i = 0; i < adapter->num_rx_queues; i++)
		txgbe_free_rx_resources(adapter->rx_ring[i]);
}

/**
 * txgbe_change_mtu - Change the Maximum Transfer Unit
 * @netdev: network interface device structure
 * @new_mtu: new value for maximum frame size
 *
 * Returns 0 on success, negative on failure
 **/
static int txgbe_change_mtu(struct net_device *netdev, int new_mtu)
{
	struct txgbe_adapter *adapter = netdev_priv(netdev);

	if ((new_mtu < 68) || (new_mtu > 9414))
		return -EINVAL;

	/*
	 * we cannot allow legacy VFs to enable their receive
	 * paths when MTU greater than 1500 is configured.  So display a
	 * warning that legacy VFs will be disabled.
	 */
	if ((adapter->flags & TXGBE_FLAG_SRIOV_ENABLED) &&
		(new_mtu > ETH_DATA_LEN))
		e_warn(probe, "Setting MTU > 1500 will disable legacy VFs\n");

	e_info(probe, "changing MTU from %d to %d\n", netdev->mtu, new_mtu);

	/* must set new MTU before calling down or up */
	netdev->mtu = new_mtu;

	if (netif_running(netdev))
		txgbe_reinit_locked(adapter);

	return 0;
}

/**
 * txgbe_open - Called when a network interface is made active
 * @netdev: network interface device structure
 *
 * Returns 0 on success, negative value on failure
 *
 * The open entry point is called when a network interface is made
 * active by the system (IFF_UP).  At this point all resources needed
 * for transmit and receive operations are allocated, the interrupt
 * handler is registered with the OS, the watchdog timer is started,
 * and the stack is notified that the interface is ready.
 **/
int txgbe_open(struct net_device *netdev)
{
	struct txgbe_adapter *adapter = netdev_priv(netdev);
	int err;

	/*special for backplane flow*/
	adapter->flags2 &= ~TXGBE_FLAG2_KR_PRO_DOWN;

	/* disallow open during test */
	if (test_bit(__TXGBE_TESTING, &adapter->state))
		return -EBUSY;

	netif_carrier_off(netdev);

	/* allocate transmit descriptors */
	err = txgbe_setup_all_tx_resources(adapter);
	if (err)
		goto err_setup_tx;

	/* allocate receive descriptors */
	err = txgbe_setup_all_rx_resources(adapter);
	if (err)
		goto err_setup_rx;

	err = txgbe_setup_isb_resources(adapter);
	if (err)
		goto err_req_isb;

	txgbe_configure(adapter);

	err = txgbe_request_irq(adapter);
	if (err)
		goto err_req_irq;

	/* Notify the stack of the actual queue counts. */
	err = netif_set_real_num_tx_queues(netdev, adapter->num_vmdqs > 1
					   ? adapter->queues_per_pool
					   : adapter->num_tx_queues);
	if (err)
		goto err_set_queues;

	err = netif_set_real_num_rx_queues(netdev, adapter->num_vmdqs > 1
					   ? adapter->queues_per_pool
					   : adapter->num_rx_queues);
	if (err)
		goto err_set_queues;

	txgbe_ptp_init(adapter);

	txgbe_up_complete(adapter);

	txgbe_clear_vxlan_port(adapter);
	udp_tunnel_get_rx_info(netdev);

	return 0;

err_set_queues:
	txgbe_free_irq(adapter);
err_req_irq:
	txgbe_free_isb_resources(adapter);
err_req_isb:
	txgbe_free_all_rx_resources(adapter);

err_setup_rx:
	txgbe_free_all_tx_resources(adapter);
err_setup_tx:
	txgbe_reset(adapter);

	return err;
}

/**
 * txgbe_close_suspend - actions necessary to both suspend and close flows
 * @adapter: the private adapter struct
 *
 * This function should contain the necessary work common to both suspending
 * and closing of the device.
 */
static void txgbe_close_suspend(struct txgbe_adapter *adapter)
{
	struct txgbe_hw *hw = &adapter->hw;

	txgbe_ptp_suspend(adapter);

	txgbe_disable_device(adapter);
	if (!((hw->subsystem_device_id & TXGBE_NCSI_MASK) == TXGBE_NCSI_SUP))
		TCALL(hw, mac.ops.disable_tx_laser);
	txgbe_clean_all_tx_rings(adapter);
	txgbe_clean_all_rx_rings(adapter);

	txgbe_free_irq(adapter);

	txgbe_free_isb_resources(adapter);
	txgbe_free_all_rx_resources(adapter);
	txgbe_free_all_tx_resources(adapter);
}

/**
 * txgbe_close - Disables a network interface
 * @netdev: network interface device structure
 *
 * Returns 0, this is not allowed to fail
 *
 * The close entry point is called when an interface is de-activated
 * by the OS.  The hardware is still under the drivers control, but
 * needs to be disabled.  A global MAC reset is issued to stop the
 * hardware, and all transmit and receive resources are freed.
 **/
int txgbe_close(struct net_device *netdev)
{
	struct txgbe_adapter *adapter = netdev_priv(netdev);
	struct txgbe_hw *hw = &adapter->hw;

	if (hw->subsystem_device_id == TXGBE_ID_WX1820_KR_KX_KX4 ||
		hw->subsystem_device_id == TXGBE_ID_SP1000_KR_KX_KX4) {
		txgbe_bp_close_protect(adapter);
	}

	txgbe_ptp_stop(adapter);

	txgbe_down(adapter);
	txgbe_free_irq(adapter);

	txgbe_free_isb_resources(adapter);
	txgbe_free_all_rx_resources(adapter);
	txgbe_free_all_tx_resources(adapter);

	txgbe_fdir_filter_exit(adapter);

	txgbe_release_hw_control(adapter);

	return 0;
}

#ifdef CONFIG_PM
static int txgbe_resume(struct pci_dev *pdev)
{
	struct txgbe_adapter *adapter;
	struct net_device *netdev;
	u32 err;

	adapter = pci_get_drvdata(pdev);
	netdev = adapter->netdev;
	adapter->hw.hw_addr = adapter->io_addr;
	pci_set_power_state(pdev, PCI_D0);
	pci_restore_state(pdev);
	/*
	 * pci_restore_state clears dev->state_saved so call
	 * pci_save_state to restore it.
	 */
	pci_save_state(pdev);

	err = pci_enable_device_mem(pdev);
	if (err) {
		e_dev_err("Cannot enable PCI device from suspend\n");
		return err;
	}
	smp_mb__before_atomic();
	clear_bit(__TXGBE_DISABLED, &adapter->state);
	pci_set_master(pdev);

	pci_wake_from_d3(pdev, false);

	txgbe_reset(adapter);

	rtnl_lock();

	err = txgbe_init_interrupt_scheme(adapter);
	if (!err && netif_running(netdev))
		err = txgbe_open(netdev);

	rtnl_unlock();

	if (err)
		return err;

	netif_device_attach(netdev);

	return 0;
}
#endif /* CONFIG_PM */
/*
 * __txgbe_shutdown is not used when power management
 * is disabled on older kernels (<2.6.12). causes a compile
 * warning/error, because it is defined and not used.
 */
static int __txgbe_shutdown(struct pci_dev *pdev, bool *enable_wake)
{
	struct txgbe_adapter *adapter = pci_get_drvdata(pdev);
	struct net_device *netdev = adapter->netdev;
	struct txgbe_hw *hw = &adapter->hw;
	u32 wufc = adapter->wol;
#ifdef CONFIG_PM
	int retval = 0;
#endif

	netif_device_detach(netdev);

	rtnl_lock();
	if (netif_running(netdev))
		txgbe_close_suspend(adapter);
	rtnl_unlock();

	txgbe_clear_interrupt_scheme(adapter);

#ifdef CONFIG_PM
	retval = pci_save_state(pdev);
	if (retval)
		return retval;
#endif

	/* this won't stop link of managebility or WoL is enabled */
	txgbe_stop_mac_link_on_d3(hw);

	if (wufc) {
		txgbe_set_rx_mode(netdev);
		txgbe_configure_rx(adapter);
		/* enable the optics for SFP+ fiber as we can WoL */
		TCALL(hw, mac.ops.enable_tx_laser);

		/* turn on all-multi mode if wake on multicast is enabled */
		if (wufc & TXGBE_PSR_WKUP_CTL_MC) {
			wr32m(hw, TXGBE_PSR_CTL,
				TXGBE_PSR_CTL_MPE, TXGBE_PSR_CTL_MPE);
		}

		pci_clear_master(adapter->pdev);
		wr32(hw, TXGBE_PSR_WKUP_CTL, wufc);
	} else {
		wr32(hw, TXGBE_PSR_WKUP_CTL, 0);
	}

	pci_wake_from_d3(pdev, !!wufc);

	*enable_wake = !!wufc;
	txgbe_release_hw_control(adapter);

	if (!test_and_set_bit(__TXGBE_DISABLED, &adapter->state))
		pci_disable_device(pdev);

	return 0;
}

#ifdef CONFIG_PM
static int txgbe_suspend(struct pci_dev *pdev,
			 pm_message_t __always_unused state)
{
	int retval;
	bool wake;

	retval = __txgbe_shutdown(pdev, &wake);
	if (retval)
		return retval;

	if (wake) {
		pci_prepare_to_sleep(pdev);
	} else {
		pci_wake_from_d3(pdev, false);
		pci_set_power_state(pdev, PCI_D3hot);
	}

	return 0;
}
#endif /* CONFIG_PM */

static void txgbe_shutdown(struct pci_dev *pdev)
{
	bool wake;

	__txgbe_shutdown(pdev, &wake);

	if (system_state == SYSTEM_POWER_OFF) {
		pci_wake_from_d3(pdev, wake);
		pci_set_power_state(pdev, PCI_D3hot);
	}
}

/**
 * txgbe_get_stats64 - Get System Network Statistics
 * @netdev: network interface device structure
 * @stats: storage space for 64bit statistics
 *
 * Returns 64bit statistics, for use in the ndo_get_stats64 callback. This
 * function replaces txgbe_get_stats for kernels which support it.
 */
static void txgbe_get_stats64(struct net_device *netdev,
			      struct rtnl_link_stats64 *stats)
{
	struct txgbe_adapter *adapter = netdev_priv(netdev);
	int i;

	rcu_read_lock();
	for (i = 0; i < adapter->num_rx_queues; i++) {
		struct txgbe_ring *ring = READ_ONCE(adapter->rx_ring[i]);
		u64 bytes, packets;
		unsigned int start;

		if (ring) {
			do {
				start = u64_stats_fetch_begin_irq(&ring->syncp);
				packets = ring->stats.packets;
				bytes   = ring->stats.bytes;
			} while (u64_stats_fetch_retry_irq(&ring->syncp,
				 start));
			stats->rx_packets += packets;
			stats->rx_bytes   += bytes;
		}
	}

	for (i = 0; i < adapter->num_tx_queues; i++) {
		struct txgbe_ring *ring = READ_ONCE(adapter->tx_ring[i]);
		u64 bytes, packets;
		unsigned int start;

		if (ring) {
			do {
				start = u64_stats_fetch_begin_irq(&ring->syncp);
				packets = ring->stats.packets;
				bytes   = ring->stats.bytes;
			} while (u64_stats_fetch_retry_irq(&ring->syncp,
				 start));
			stats->tx_packets += packets;
			stats->tx_bytes   += bytes;
		}
	}
	rcu_read_unlock();
	/* following stats updated by txgbe_watchdog_task() */
	stats->multicast        = netdev->stats.multicast;
	stats->rx_errors        = netdev->stats.rx_errors;
	stats->rx_length_errors = netdev->stats.rx_length_errors;
	stats->rx_crc_errors    = netdev->stats.rx_crc_errors;
	stats->rx_missed_errors = netdev->stats.rx_missed_errors;
}

/**
 * txgbe_update_stats - Update the board statistics counters.
 * @adapter: board private structure
 **/
void txgbe_update_stats(struct txgbe_adapter *adapter)
{
	struct net_device_stats *net_stats = &adapter->netdev->stats;
	struct txgbe_hw *hw = &adapter->hw;
	struct txgbe_hw_stats *hwstats = &adapter->stats;
	u64 total_mpc = 0;
	u32 i, missed_rx = 0, mpc, bprc, lxon, lxoff;
	u64 non_eop_descs = 0, restart_queue = 0, tx_busy = 0;
	u64 alloc_rx_page_failed = 0, alloc_rx_buff_failed = 0;
	u64 bytes = 0, packets = 0, hw_csum_rx_error = 0;
	u64 hw_csum_rx_good = 0;

	if (test_bit(__TXGBE_DOWN, &adapter->state) ||
	    test_bit(__TXGBE_RESETTING, &adapter->state))
		return;

	if (adapter->flags2 & TXGBE_FLAG2_RSC_ENABLED) {
		u64 rsc_count = 0;
		u64 rsc_flush = 0;
		for (i = 0; i < adapter->num_rx_queues; i++) {
			rsc_count += adapter->rx_ring[i]->rx_stats.rsc_count;
			rsc_flush += adapter->rx_ring[i]->rx_stats.rsc_flush;
		}
		adapter->rsc_total_count = rsc_count;
		adapter->rsc_total_flush = rsc_flush;
	}

	for (i = 0; i < adapter->num_rx_queues; i++) {
		struct txgbe_ring *rx_ring = adapter->rx_ring[i];
		non_eop_descs += rx_ring->rx_stats.non_eop_descs;
		alloc_rx_page_failed += rx_ring->rx_stats.alloc_rx_page_failed;
		alloc_rx_buff_failed += rx_ring->rx_stats.alloc_rx_buff_failed;
		hw_csum_rx_error += rx_ring->rx_stats.csum_err;
		hw_csum_rx_good += rx_ring->rx_stats.csum_good_cnt;
		bytes += rx_ring->stats.bytes;
		packets += rx_ring->stats.packets;

	}
	adapter->non_eop_descs = non_eop_descs;
	adapter->alloc_rx_page_failed = alloc_rx_page_failed;
	adapter->alloc_rx_buff_failed = alloc_rx_buff_failed;
	adapter->hw_csum_rx_error = hw_csum_rx_error;
	adapter->hw_csum_rx_good = hw_csum_rx_good;
	net_stats->rx_bytes = bytes;
	net_stats->rx_packets = packets;

	bytes = 0;
	packets = 0;
	/* gather some stats to the adapter struct that are per queue */
	for (i = 0; i < adapter->num_tx_queues; i++) {
		struct txgbe_ring *tx_ring = adapter->tx_ring[i];
		restart_queue += tx_ring->tx_stats.restart_queue;
		tx_busy += tx_ring->tx_stats.tx_busy;
		bytes += tx_ring->stats.bytes;
		packets += tx_ring->stats.packets;
	}
	adapter->restart_queue = restart_queue;
	adapter->tx_busy = tx_busy;
	net_stats->tx_bytes = bytes;
	net_stats->tx_packets = packets;

	hwstats->crcerrs += rd32(hw, TXGBE_RX_CRC_ERROR_FRAMES_LOW);

	/* 8 register reads */
	for (i = 0; i < 8; i++) {
		/* for packet buffers not used, the register should read 0 */
		mpc = rd32(hw, TXGBE_RDB_MPCNT(i));
		missed_rx += mpc;
		hwstats->mpc[i] += mpc;
		total_mpc += hwstats->mpc[i];
		hwstats->pxontxc[i] += rd32(hw, TXGBE_RDB_PXONTXC(i));
		hwstats->pxofftxc[i] +=
				rd32(hw, TXGBE_RDB_PXOFFTXC(i));
		hwstats->pxonrxc[i] += rd32(hw, TXGBE_MAC_PXONRXC(i));
	}

	hwstats->gprc += rd32(hw, TXGBE_PX_GPRC);

	txgbe_update_xoff_received(adapter);

	hwstats->o2bgptc += rd32(hw, TXGBE_TDM_OS2BMC_CNT);
	if (txgbe_check_mng_access(&adapter->hw)) {
		hwstats->o2bspc += rd32(hw, TXGBE_MNG_OS2BMC_CNT);
		hwstats->b2ospc += rd32(hw, TXGBE_MNG_BMC2OS_CNT);
	}
	hwstats->b2ogprc += rd32(hw, TXGBE_RDM_BMC2OS_CNT);
	hwstats->gorc += rd32(hw, TXGBE_PX_GORC_LSB);
	hwstats->gorc += (u64)rd32(hw, TXGBE_PX_GORC_MSB) << 32;

	hwstats->gotc += rd32(hw, TXGBE_PX_GOTC_LSB);
	hwstats->gotc += (u64)rd32(hw, TXGBE_PX_GOTC_MSB) << 32;


	adapter->hw_rx_no_dma_resources +=
				     rd32(hw, TXGBE_RDM_DRP_PKT);
	hwstats->lxonrxc += rd32(hw, TXGBE_MAC_LXONRXC);

	hwstats->fdirmatch += rd32(hw, TXGBE_RDB_FDIR_MATCH);
	hwstats->fdirmiss += rd32(hw, TXGBE_RDB_FDIR_MISS);

	bprc = rd32(hw, TXGBE_RX_BC_FRAMES_GOOD_LOW);
	hwstats->bprc += bprc;
	hwstats->mprc = 0;

	for (i = 0; i < 128; i++)
		hwstats->mprc += rd32(hw, TXGBE_PX_MPRC(i));


	hwstats->roc += rd32(hw, TXGBE_RX_OVERSIZE_FRAMES_GOOD);
	hwstats->rlec += rd32(hw, TXGBE_RX_LEN_ERROR_FRAMES_LOW);
	lxon = rd32(hw, TXGBE_RDB_LXONTXC);
	hwstats->lxontxc += lxon;
	lxoff = rd32(hw, TXGBE_RDB_LXOFFTXC);
	hwstats->lxofftxc += lxoff;

	hwstats->gptc += rd32(hw, TXGBE_PX_GPTC);
	hwstats->mptc += rd32(hw, TXGBE_TX_MC_FRAMES_GOOD_LOW);
	hwstats->ruc += rd32(hw, TXGBE_RX_UNDERSIZE_FRAMES_GOOD);
	hwstats->tpr += rd32(hw, TXGBE_RX_FRAME_CNT_GOOD_BAD_LOW);
	hwstats->bptc += rd32(hw, TXGBE_TX_BC_FRAMES_GOOD_LOW);
	/* Fill out the OS statistics structure */
	net_stats->multicast = hwstats->mprc;

	/* Rx Errors */
	net_stats->rx_errors = hwstats->crcerrs +
				       hwstats->rlec;
	net_stats->rx_dropped = 0;
	net_stats->rx_length_errors = hwstats->rlec;
	net_stats->rx_crc_errors = hwstats->crcerrs;
	net_stats->rx_missed_errors = total_mpc;

	/*
	 * VF Stats Collection - skip while resetting because these
	 * are not clear on read and otherwise you'll sometimes get
	 * crazy values.
	 */
	if (!test_bit(__TXGBE_RESETTING, &adapter->state)) {
		for (i = 0; i < adapter->num_vfs; i++) {
			UPDATE_VF_COUNTER_32bit(TXGBE_VX_GPRC(i),             \
					adapter->vfinfo[i].last_vfstats.gprc, \
					adapter->vfinfo[i].vfstats.gprc);
			UPDATE_VF_COUNTER_32bit(TXGBE_VX_GPTC(i),             \
					adapter->vfinfo[i].last_vfstats.gptc, \
					adapter->vfinfo[i].vfstats.gptc);
			UPDATE_VF_COUNTER_36bit(TXGBE_VX_GORC_LSB(i),         \
					TXGBE_VX_GORC_MSB(i),                 \
					adapter->vfinfo[i].last_vfstats.gorc, \
					adapter->vfinfo[i].vfstats.gorc);
			UPDATE_VF_COUNTER_36bit(TXGBE_VX_GOTC_LSB(i),         \
					TXGBE_VX_GOTC_MSB(i),                 \
					adapter->vfinfo[i].last_vfstats.gotc, \
					adapter->vfinfo[i].vfstats.gotc);
			UPDATE_VF_COUNTER_32bit(TXGBE_VX_MPRC(i),             \
					adapter->vfinfo[i].last_vfstats.mprc, \
					adapter->vfinfo[i].vfstats.mprc);
		}
	}
}

/**
 * txgbe_fdir_reinit_subtask - worker thread to reinit FDIR filter table
 * @adapter - pointer to the device adapter structure
 **/
static void txgbe_fdir_reinit_subtask(struct txgbe_adapter *adapter)
{
	struct txgbe_hw *hw = &adapter->hw;
	int i;

	if (!(adapter->flags2 & TXGBE_FLAG2_FDIR_REQUIRES_REINIT))
		return;

	adapter->flags2 &= ~TXGBE_FLAG2_FDIR_REQUIRES_REINIT;

	/* if interface is down do nothing */
	if (test_bit(__TXGBE_DOWN, &adapter->state))
		return;

	/* do nothing if we are not using signature filters */
	if (!(adapter->flags & TXGBE_FLAG_FDIR_HASH_CAPABLE))
		return;

	adapter->fdir_overflow++;

	if (txgbe_reinit_fdir_tables(hw) == 0) {
		for (i = 0; i < adapter->num_tx_queues; i++)
			set_bit(__TXGBE_TX_FDIR_INIT_DONE,
				&(adapter->tx_ring[i]->state));
		/* re-enable flow director interrupts */
		wr32m(hw, TXGBE_PX_MISC_IEN,
			TXGBE_PX_MISC_IEN_FLOW_DIR, TXGBE_PX_MISC_IEN_FLOW_DIR);
	} else {
		e_err(probe, "failed to finish FDIR re-initialization, "
		      "ignored adding FDIR ATR filters\n");
	}
}

/**
 * txgbe_check_hang_subtask - check for hung queues and dropped interrupts
 * @adapter - pointer to the device adapter structure
 *
 * This function serves two purposes.  First it strobes the interrupt lines
 * in order to make certain interrupts are occurring.  Secondly it sets the
 * bits needed to check for TX hangs.  As a result we should immediately
 * determine if a hang has occurred.
 */
static void txgbe_check_hang_subtask(struct txgbe_adapter *adapter)
{
	int i;

	/* If we're down or resetting, just bail */
	if (test_bit(__TXGBE_DOWN, &adapter->state) ||
	    test_bit(__TXGBE_REMOVING, &adapter->state) ||
	    test_bit(__TXGBE_RESETTING, &adapter->state))
		return;

	/* Force detection of hung controller */
	if (netif_carrier_ok(adapter->netdev)) {
		for (i = 0; i < adapter->num_tx_queues; i++)
			set_check_for_tx_hang(adapter->tx_ring[i]);
	}

}

/**
 * txgbe_watchdog_update_link - update the link status
 * @adapter - pointer to the device adapter structure
 * @link_speed - pointer to a u32 to store the link_speed
 **/
static void txgbe_watchdog_update_link(struct txgbe_adapter *adapter)
{
	struct txgbe_hw *hw = &adapter->hw;
	u32 link_speed = adapter->link_speed;
	bool link_up = adapter->link_up;
//	bool pfc_en = adapter->dcb_cfg.pfc_mode_enable;
	u32 reg;
	u32 i = 1;

	if (!(adapter->flags & TXGBE_FLAG_NEED_LINK_UPDATE))
		return;

	link_speed = TXGBE_LINK_SPEED_10GB_FULL;
	link_up = true;
	TCALL(hw, mac.ops.check_link, &link_speed, &link_up, false);

	if (link_up || time_after(jiffies, (adapter->link_check_timeout +
		TXGBE_TRY_LINK_TIMEOUT))) {
		adapter->flags &= ~TXGBE_FLAG_NEED_LINK_UPDATE;
	}

	for (i = 0; i < 3; i++) {
		TCALL(hw, mac.ops.check_link, &link_speed, &link_up, false);
		msleep(1);
	}

	if (link_up && !((adapter->flags & TXGBE_FLAG_DCB_ENABLED))) {
		TCALL(hw, mac.ops.fc_enable);
		txgbe_set_rx_drop_en(adapter);
	}

	if (link_up) {
		adapter->last_rx_ptp_check = jiffies;

		if (test_bit(__TXGBE_PTP_RUNNING, &adapter->state))
			txgbe_ptp_start_cyclecounter(adapter);

		if (link_speed & TXGBE_LINK_SPEED_10GB_FULL) {
			wr32(hw, TXGBE_MAC_TX_CFG,
				(rd32(hw, TXGBE_MAC_TX_CFG) &
				~TXGBE_MAC_TX_CFG_SPEED_MASK) | TXGBE_MAC_TX_CFG_TE |
				TXGBE_MAC_TX_CFG_SPEED_10G);
		} else if (link_speed & (TXGBE_LINK_SPEED_1GB_FULL |
				TXGBE_LINK_SPEED_100_FULL | TXGBE_LINK_SPEED_10_FULL)) {
			wr32(hw, TXGBE_MAC_TX_CFG,
				(rd32(hw, TXGBE_MAC_TX_CFG) &
				~TXGBE_MAC_TX_CFG_SPEED_MASK) | TXGBE_MAC_TX_CFG_TE |
				TXGBE_MAC_TX_CFG_SPEED_1G);
		}

		/* Re configure MAC RX */
		reg = rd32(hw, TXGBE_MAC_RX_CFG);
		wr32(hw, TXGBE_MAC_RX_CFG, reg);
		wr32(hw, TXGBE_MAC_PKT_FLT, TXGBE_MAC_PKT_FLT_PR);
		reg = rd32(hw, TXGBE_MAC_WDG_TIMEOUT);
		wr32(hw, TXGBE_MAC_WDG_TIMEOUT, reg);
	}

	adapter->link_up = link_up;
	adapter->link_speed = link_speed;
	if (hw->mac.ops.dmac_config && hw->mac.dmac_config.watchdog_timer) {
		u8 num_tcs = netdev_get_num_tc(adapter->netdev);

		if (hw->mac.dmac_config.link_speed != link_speed ||
		    hw->mac.dmac_config.num_tcs != num_tcs) {
			hw->mac.dmac_config.link_speed = link_speed;
			hw->mac.dmac_config.num_tcs = num_tcs;
			TCALL(hw, mac.ops.dmac_config);
		}
	}
}

static void txgbe_update_default_up(struct txgbe_adapter *adapter)
{
	u8 up = 0;

	adapter->default_up = up;
}

/**
 * txgbe_watchdog_link_is_up - update netif_carrier status and
 *                             print link up message
 * @adapter - pointer to the device adapter structure
 **/
static void txgbe_watchdog_link_is_up(struct txgbe_adapter *adapter)
{
	struct net_device *netdev = adapter->netdev;
	struct txgbe_hw *hw = &adapter->hw;
	u32 link_speed = adapter->link_speed;
	bool flow_rx, flow_tx;

	/* only continue if link was previously down */
	if (netif_carrier_ok(netdev))
		return;

	adapter->flags2 &= ~TXGBE_FLAG2_SEARCH_FOR_SFP;

	/* flow_rx, flow_tx report link flow control status */
	flow_rx = (rd32(hw, TXGBE_MAC_RX_FLOW_CTRL) & 0x101) == 0x1;
	flow_tx = !!(TXGBE_RDB_RFCC_RFCE_802_3X &
		     rd32(hw, TXGBE_RDB_RFCC));

	e_info(drv, "NIC Link is Up %s, Flow Control: %s\n",
	       (link_speed == TXGBE_LINK_SPEED_10GB_FULL ?
	       "10 Gbps" :
	       (link_speed == TXGBE_LINK_SPEED_1GB_FULL ?
	       "1 Gbps" :
	       (link_speed == TXGBE_LINK_SPEED_100_FULL ?
	       "100 Mbps" :
	       (link_speed == TXGBE_LINK_SPEED_10_FULL ?
	       "10 Mbps" :
	       "unknown speed")))),
	       ((flow_rx && flow_tx) ? "RX/TX" :
	       (flow_rx ? "RX" :
	       (flow_tx ? "TX" : "None"))));

	netif_carrier_on(netdev);
	netif_tx_wake_all_queues(netdev);

	/* update the default user priority for VFs */
	txgbe_update_default_up(adapter);

	/* ping all the active vfs to let them know link has changed */
}

/**
 * txgbe_watchdog_link_is_down - update netif_carrier status and
 *                               print link down message
 * @adapter - pointer to the adapter structure
 **/
static void txgbe_watchdog_link_is_down(struct txgbe_adapter *adapter)
{
	struct net_device *netdev = adapter->netdev;
	struct txgbe_hw *hw = &adapter->hw;
	adapter->link_up = false;
	adapter->link_speed = 0;

	/* only continue if link was up previously */
	if (!netif_carrier_ok(netdev))
		return;

	if (hw->subsystem_device_id == TXGBE_ID_WX1820_KR_KX_KX4 ||
		hw->subsystem_device_id == TXGBE_ID_SP1000_KR_KX_KX4) {
		txgbe_bp_down_event(adapter);
	}

	if (test_bit(__TXGBE_PTP_RUNNING, &adapter->state))
		txgbe_ptp_start_cyclecounter(adapter);

	e_info(drv, "NIC Link is Down\n");
	netif_carrier_off(netdev);
	netif_tx_stop_all_queues(netdev);

	/* ping all the active vfs to let them know link has changed */

}

static bool txgbe_ring_tx_pending(struct txgbe_adapter *adapter)
{
	int i;

	for (i = 0; i < adapter->num_tx_queues; i++) {
		struct txgbe_ring *tx_ring = adapter->tx_ring[i];

		if (tx_ring->next_to_use != tx_ring->next_to_clean)
			return true;
	}

	return false;
}

static bool txgbe_vf_tx_pending(struct txgbe_adapter *adapter)
{
	struct txgbe_hw *hw = &adapter->hw;
	struct txgbe_ring_feature *vmdq = &adapter->ring_feature[RING_F_VMDQ];
	u32 q_per_pool = __ALIGN_MASK(1, ~vmdq->mask);

	u32 i, j;

	if (!adapter->num_vfs)
		return false;

	for (i = 0; i < adapter->num_vfs; i++) {
		for (j = 0; j < q_per_pool; j++) {
			u32 h, t;

			h = rd32(hw,
					   TXGBE_PX_TR_RPn(q_per_pool, i, j));
			t = rd32(hw,
					   TXGBE_PX_TR_WPn(q_per_pool, i, j));

			if (h != t)
				return true;
		}
	}

	return false;
}

/**
 * txgbe_watchdog_flush_tx - flush queues on link down
 * @adapter - pointer to the device adapter structure
 **/
static void txgbe_watchdog_flush_tx(struct txgbe_adapter *adapter)
{
	if (!netif_carrier_ok(adapter->netdev)) {
		if (txgbe_ring_tx_pending(adapter) ||
		    txgbe_vf_tx_pending(adapter)) {
			/* We've lost link, so the controller stops DMA,
			 * but we've got queued Tx work that's never going
			 * to get done, so reset controller to flush Tx.
			 * (Do the reset outside of interrupt context).
			 */
			e_warn(drv, "initiating reset due to lost link with "
				"pending Tx work\n");
			adapter->flags2 |= TXGBE_FLAG2_PF_RESET_REQUESTED;
		}
	}
}

/**
 * txgbe_watchdog_subtask - check and bring link up
 * @adapter - pointer to the device adapter structure
 **/
static void txgbe_watchdog_subtask(struct txgbe_adapter *adapter)
{
	u32 value = 0;
	struct txgbe_hw *hw = &adapter->hw;

	/* if interface is down do nothing */
	if (test_bit(__TXGBE_DOWN, &adapter->state) ||
	    test_bit(__TXGBE_REMOVING, &adapter->state) ||
	    test_bit(__TXGBE_RESETTING, &adapter->state))
		return;

	if (hw->subsystem_device_id == TXGBE_ID_WX1820_KR_KX_KX4 ||
		hw->subsystem_device_id == TXGBE_ID_SP1000_KR_KX_KX4) {
		txgbe_bp_watchdog_event(adapter);
	}

	if (BOND_CHECK_LINK_MODE == 1) {
		value = rd32(hw, 0x14404);
		value = value & 0x1;
		if (value == 1)
			adapter->flags |= TXGBE_FLAG_NEED_LINK_UPDATE;
	}
	if (!(adapter->flags2 & TXGBE_FLAG2_LINK_DOWN))
		txgbe_watchdog_update_link(adapter);

	if (adapter->link_up)
		txgbe_watchdog_link_is_up(adapter);
	else
		txgbe_watchdog_link_is_down(adapter);

	txgbe_update_stats(adapter);

	txgbe_watchdog_flush_tx(adapter);
}

/**
 * txgbe_sfp_detection_subtask - poll for SFP+ cable
 * @adapter - the txgbe adapter structure
 **/
static void txgbe_sfp_detection_subtask(struct txgbe_adapter *adapter)
{
	struct txgbe_hw *hw = &adapter->hw;
	struct txgbe_mac_info *mac = &hw->mac;
	s32 err;

	/* not searching for SFP so there is nothing to do here */
	if (!(adapter->flags2 & TXGBE_FLAG2_SEARCH_FOR_SFP) &&
	    !(adapter->flags2 & TXGBE_FLAG2_SFP_NEEDS_RESET))
		return;

	if (adapter->sfp_poll_time &&
	    time_after(adapter->sfp_poll_time, jiffies))
		return; /* If not yet time to poll for SFP */

	/* someone else is in init, wait until next service event */
	if (test_and_set_bit(__TXGBE_IN_SFP_INIT, &adapter->state))
		return;

	adapter->sfp_poll_time = jiffies + TXGBE_SFP_POLL_JIFFIES - 1;

	err = TCALL(hw, phy.ops.identify_sfp);
	if (err == TXGBE_ERR_SFP_NOT_SUPPORTED)
		goto sfp_out;

	if (err == TXGBE_ERR_SFP_NOT_PRESENT) {
		/* If no cable is present, then we need to reset
		 * the next time we find a good cable. */
		adapter->flags2 |= TXGBE_FLAG2_SFP_NEEDS_RESET;
	}

	/* exit on error */
	if (err)
		goto sfp_out;

	/* exit if reset not needed */
	if (!(adapter->flags2 & TXGBE_FLAG2_SFP_NEEDS_RESET))
		goto sfp_out;

	adapter->flags2 &= ~TXGBE_FLAG2_SFP_NEEDS_RESET;

	if (hw->phy.multispeed_fiber) {
		/* Set up dual speed SFP+ support */
		mac->ops.setup_link = txgbe_setup_mac_link_multispeed_fiber;
		mac->ops.setup_mac_link = txgbe_setup_mac_link;
		mac->ops.set_rate_select_speed =
					       txgbe_set_hard_rate_select_speed;
	} else {
		mac->ops.setup_link = txgbe_setup_mac_link;
		mac->ops.set_rate_select_speed =
					       txgbe_set_hard_rate_select_speed;
		hw->phy.autoneg_advertised = 0;
	}

	adapter->flags |= TXGBE_FLAG_NEED_LINK_CONFIG;
	e_info(probe, "detected SFP+: %d\n", hw->phy.sfp_type);

sfp_out:
	clear_bit(__TXGBE_IN_SFP_INIT, &adapter->state);

	if ((err == TXGBE_ERR_SFP_NOT_SUPPORTED) &&
	    adapter->netdev_registered) {
		e_dev_err("failed to initialize because an unsupported "
			  "SFP+ module type was detected.\n");
	}
}

/**
 * txgbe_sfp_link_config_subtask - set up link SFP after module install
 * @adapter - the txgbe adapter structure
 **/
static void txgbe_sfp_link_config_subtask(struct txgbe_adapter *adapter)
{
	struct txgbe_hw *hw = &adapter->hw;
	u32 speed;
	bool autoneg = false;
	u16 value;
	u8 device_type = hw->subsystem_id & 0xF0;

	if (!(adapter->flags & TXGBE_FLAG_NEED_LINK_CONFIG))
		return;

	/* someone else is in init, wait until next service event */
	if (test_and_set_bit(__TXGBE_IN_SFP_INIT, &adapter->state))
		return;

	adapter->flags &= ~TXGBE_FLAG_NEED_LINK_CONFIG;

	if (device_type == TXGBE_ID_XAUI) {
		/* clear ext phy int status */
		txgbe_read_mdio(&hw->phy_dev, hw->phy.addr, 0x03, 0x8011, &value);
		if (value & 0x400)
			adapter->flags |= TXGBE_FLAG_NEED_LINK_UPDATE;
		if (!(value & 0x800)) {
			clear_bit(__TXGBE_IN_SFP_INIT, &adapter->state);
			return;
		}
	}

	if (device_type == TXGBE_ID_MAC_XAUI ||
		(txgbe_get_media_type(hw) == txgbe_media_type_copper &&
		device_type == TXGBE_ID_SFI_XAUI)) {
		speed = TXGBE_LINK_SPEED_10GB_FULL;
	} else if (device_type == TXGBE_ID_MAC_SGMII) {
		speed = TXGBE_LINK_SPEED_1GB_FULL;
	} else {
		speed = hw->phy.autoneg_advertised;
		if ((!speed) && (hw->mac.ops.get_link_capabilities)) {
				TCALL(hw, mac.ops.get_link_capabilities, &speed, &autoneg);
			/* setup the highest link when no autoneg */
			if (!autoneg) {
				if (speed & TXGBE_LINK_SPEED_10GB_FULL)
					speed = TXGBE_LINK_SPEED_10GB_FULL;
			}
		}
	}

	TCALL(hw, mac.ops.setup_link, speed, txgbe_is_sfp(hw));

	adapter->flags |= TXGBE_FLAG_NEED_LINK_UPDATE;
	adapter->link_check_timeout = jiffies;
	clear_bit(__TXGBE_IN_SFP_INIT, &adapter->state);
}

static void txgbe_sfp_reset_eth_phy_subtask(struct txgbe_adapter *adapter)
{
	struct txgbe_hw *hw = &adapter->hw;
	u32 speed;
	bool linkup = true;
	u32 i = 0;

	if (!(adapter->flags2 & TXGBE_FLAG_NEED_ETH_PHY_RESET))
		return;

	adapter->flags2 &= ~TXGBE_FLAG_NEED_ETH_PHY_RESET;

	TCALL(hw, mac.ops.check_link, &speed, &linkup, false);
	if (!linkup) {
		txgbe_wr32_epcs(hw, TXGBE_VR_XS_OR_PCS_MMD_DIGI_CTL1,
					0xA000);
		/* wait phy initialization done */
		for (i = 0; i < TXGBE_PHY_INIT_DONE_POLLING_TIME; i++) {
			if ((txgbe_rd32_epcs(hw,
				TXGBE_VR_XS_OR_PCS_MMD_DIGI_CTL1) &
				TXGBE_VR_XS_OR_PCS_MMD_DIGI_CTL1_VR_RST) == 0)
				break;
			msleep(100);
		}
	}
}

/**
 * txgbe_service_timer - Timer Call-back
 * @data: pointer to adapter cast into an unsigned long
 **/
static void txgbe_service_timer(struct timer_list *t)
{
	struct txgbe_adapter *adapter = from_timer(adapter, t, service_timer);
	unsigned long next_event_offset;
	struct txgbe_hw *hw = &adapter->hw;

	/* poll faster when waiting for link */
	if (adapter->flags & TXGBE_FLAG_NEED_LINK_UPDATE) {
		if ((hw->subsystem_device_id & 0xF0) == TXGBE_ID_KR_KX_KX4)
			next_event_offset = HZ;
		else if (BOND_CHECK_LINK_MODE == 1)
			next_event_offset = HZ / 100;
		else
			next_event_offset = HZ / 10;
	} else
		next_event_offset = HZ * 2;

	if ((rd32(&adapter->hw, TXGBE_MIS_PF_SM) == 1) && (hw->bus.lan_id)) {
		adapter->flags2 |= TXGBE_FLAG2_PCIE_NEED_RECOVER;
	}

	/* Reset the timer */
	mod_timer(&adapter->service_timer, next_event_offset + jiffies);

	txgbe_service_event_schedule(adapter);
}

static void txgbe_reset_subtask(struct txgbe_adapter *adapter)
{
	u32 reset_flag = 0;
	u32 value = 0;

	if (!(adapter->flags2 & (TXGBE_FLAG2_PF_RESET_REQUESTED |
		TXGBE_FLAG2_DEV_RESET_REQUESTED |
		TXGBE_FLAG2_GLOBAL_RESET_REQUESTED |
		TXGBE_FLAG2_RESET_INTR_RECEIVED)))
		return;

	/* If we're already down, just bail */
	if (test_bit(__TXGBE_DOWN, &adapter->state) ||
	    test_bit(__TXGBE_REMOVING, &adapter->state))
		return;

	netdev_err(adapter->netdev, "Reset adapter\n");
	adapter->tx_timeout_count++;

	rtnl_lock();
	if (adapter->flags2 & TXGBE_FLAG2_GLOBAL_RESET_REQUESTED) {
		reset_flag |= TXGBE_FLAG2_GLOBAL_RESET_REQUESTED;
		adapter->flags2 &= ~TXGBE_FLAG2_GLOBAL_RESET_REQUESTED;
	}
	if (adapter->flags2 & TXGBE_FLAG2_DEV_RESET_REQUESTED) {
		reset_flag |= TXGBE_FLAG2_DEV_RESET_REQUESTED;
		adapter->flags2 &= ~TXGBE_FLAG2_DEV_RESET_REQUESTED;
	}
	if (adapter->flags2 & TXGBE_FLAG2_PF_RESET_REQUESTED) {
		reset_flag |= TXGBE_FLAG2_PF_RESET_REQUESTED;
		adapter->flags2 &= ~TXGBE_FLAG2_PF_RESET_REQUESTED;
	}

	if (adapter->flags2 & TXGBE_FLAG2_RESET_INTR_RECEIVED) {
		/* If there's a recovery already waiting, it takes
		* precedence before starting a new reset sequence.
		*/
		adapter->flags2 &= ~TXGBE_FLAG2_RESET_INTR_RECEIVED;
		value = rd32m(&adapter->hw, TXGBE_MIS_RST_ST,
				TXGBE_MIS_RST_ST_DEV_RST_TYPE_MASK) >>
				TXGBE_MIS_RST_ST_DEV_RST_TYPE_SHIFT;
		if (value == TXGBE_MIS_RST_ST_DEV_RST_TYPE_SW_RST) {
			adapter->hw.reset_type = TXGBE_SW_RESET;
			/* errata 7 */
			if (txgbe_mng_present(&adapter->hw) &&
				adapter->hw.revision_id == TXGBE_SP_MPW)
				adapter->flags2 |=
					TXGBE_FLAG2_MNG_REG_ACCESS_DISABLED;
		} else if (value == TXGBE_MIS_RST_ST_DEV_RST_TYPE_GLOBAL_RST)
			adapter->hw.reset_type = TXGBE_GLOBAL_RESET;
		adapter->hw.force_full_reset = true;
		txgbe_reinit_locked(adapter);
		adapter->hw.force_full_reset = false;
		goto unlock;
	}

	if (reset_flag & TXGBE_FLAG2_DEV_RESET_REQUESTED) {
		/* Request a Device Reset
		 *
		 * This will start the chip's countdown to the actual full
		 * chip reset event, and a warning interrupt to be sent
		 * to all PFs, including the requestor.  Our handler
		 * for the warning interrupt will deal with the shutdown
		 * and recovery of the switch setup.
		 */
		/*debug to up*/
		/*txgbe_dump(adapter);*/
		if (txgbe_mng_present(&adapter->hw)) {
			txgbe_reset_hostif(&adapter->hw);
		} else
			wr32m(&adapter->hw, TXGBE_MIS_RST,
				TXGBE_MIS_RST_SW_RST, TXGBE_MIS_RST_SW_RST);

	} else if (reset_flag & TXGBE_FLAG2_PF_RESET_REQUESTED) {
		/*debug to up*/
		txgbe_reinit_locked(adapter);
	} else if (reset_flag & TXGBE_FLAG2_GLOBAL_RESET_REQUESTED) {
		/* Request a Global Reset
		 *
		 * This will start the chip's countdown to the actual full
		 * chip reset event, and a warning interrupt to be sent
		 * to all PFs, including the requestor.  Our handler
		 * for the warning interrupt will deal with the shutdown
		 * and recovery of the switch setup.
		 */
		/*debug to up*/
		pci_save_state(adapter->pdev);
		if (txgbe_mng_present(&adapter->hw)) {
			txgbe_reset_hostif(&adapter->hw);
		} else
			wr32m(&adapter->hw, TXGBE_MIS_RST,
				TXGBE_MIS_RST_GLOBAL_RST,
				TXGBE_MIS_RST_GLOBAL_RST);

	}

unlock:
	rtnl_unlock();
}

static void txgbe_check_pcie_subtask(struct txgbe_adapter *adapter)
{
	if (!(adapter->flags2 & TXGBE_FLAG2_PCIE_NEED_RECOVER))
		return;

	e_info(probe, "do recovery\n");
	wr32m(&adapter->hw, TXGBE_MIS_PF_SM,
		TXGBE_MIS_PF_SM_SM, 0);
	adapter->flags2 &= ~TXGBE_FLAG2_PCIE_NEED_RECOVER;
}

/**
 * txgbe_service_task - manages and runs subtasks
 * @work: pointer to work_struct containing our data
 **/
static void txgbe_service_task(struct work_struct *work)
{
	struct txgbe_adapter *adapter = container_of(work,
						     struct txgbe_adapter,
						     service_task);
	if (TXGBE_REMOVED(adapter->hw.hw_addr)) {
		if (!test_bit(__TXGBE_DOWN, &adapter->state)) {
			rtnl_lock();
			txgbe_down(adapter);
			rtnl_unlock();
		}
		txgbe_service_event_complete(adapter);
		return;
	}

	if (adapter->flags2 & TXGBE_FLAG2_VXLAN_REREG_NEEDED) {
		adapter->flags2 &= ~TXGBE_FLAG2_VXLAN_REREG_NEEDED;
		udp_tunnel_get_rx_info(adapter->netdev);
	}

	txgbe_check_pcie_subtask(adapter);
	txgbe_reset_subtask(adapter);
	txgbe_sfp_detection_subtask(adapter);
	txgbe_sfp_link_config_subtask(adapter);
	txgbe_sfp_reset_eth_phy_subtask(adapter);
	txgbe_check_overtemp_subtask(adapter);
	txgbe_watchdog_subtask(adapter);
	txgbe_fdir_reinit_subtask(adapter);
	txgbe_check_hang_subtask(adapter);
	if (test_bit(__TXGBE_PTP_RUNNING, &adapter->state)) {
		txgbe_ptp_overflow_check(adapter);
		if (unlikely(adapter->flags &
			TXGBE_FLAG_RX_HWTSTAMP_IN_REGISTER))
			txgbe_ptp_rx_hang(adapter);
	}

	txgbe_service_event_complete(adapter);
}

static u8 get_ipv6_proto(struct sk_buff *skb, int offset)
{
	struct ipv6hdr *hdr = (struct ipv6hdr *)(skb->data + offset);
	u8 nexthdr = hdr->nexthdr;

	offset += sizeof(struct ipv6hdr);

	while (ipv6_ext_hdr(nexthdr)) {
		struct ipv6_opt_hdr _hdr, *hp;

		if (nexthdr == NEXTHDR_NONE)
			break;

		hp = skb_header_pointer(skb, offset, sizeof(_hdr), &_hdr);
		if (!hp)
			break;

		if (nexthdr == NEXTHDR_FRAGMENT) {
			break;
		} else if (nexthdr == NEXTHDR_AUTH) {
			offset +=  ipv6_authlen(hp);
		} else {
			offset +=  ipv6_optlen(hp);
		}

		nexthdr = hp->nexthdr;
	}

	return nexthdr;
}

union network_header {
	struct iphdr *ipv4;
	struct ipv6hdr *ipv6;
	void *raw;
};

static txgbe_dptype encode_tx_desc_ptype(const struct txgbe_tx_buffer *first)
{
	struct sk_buff *skb = first->skb;

	u8 tun_prot = 0;

	u8 l4_prot = 0;
	u8 ptype = 0;

	if (skb->encapsulation) {
		union network_header hdr;

		switch (first->protocol) {
		case __constant_htons(ETH_P_IP):
			tun_prot = ip_hdr(skb)->protocol;
			if (ip_hdr(skb)->frag_off & htons(IP_MF | IP_OFFSET))
				goto encap_frag;
			ptype = TXGBE_PTYPE_TUN_IPV4;
			break;
		case __constant_htons(ETH_P_IPV6):
			tun_prot = get_ipv6_proto(skb, skb_network_offset(skb));
			if (tun_prot == NEXTHDR_FRAGMENT)
				goto encap_frag;
			ptype = TXGBE_PTYPE_TUN_IPV6;
			break;
		default:
			goto exit;
		}

		if (tun_prot == IPPROTO_IPIP) {
			hdr.raw = (void *)inner_ip_hdr(skb);
			ptype |= TXGBE_PTYPE_PKT_IPIP;
		} else if (tun_prot == IPPROTO_UDP) {
			hdr.raw = (void *)inner_ip_hdr(skb);
			/* fixme: VXLAN-GPE neither ETHER nor IP */

			if (skb->inner_protocol_type != ENCAP_TYPE_ETHER ||
				skb->inner_protocol != htons(ETH_P_TEB)) {
				ptype |= TXGBE_PTYPE_PKT_IG;
			} else {
				if (((struct ethhdr *)
					skb_inner_mac_header(skb))->h_proto
					== htons(ETH_P_8021Q)) {
					ptype |= TXGBE_PTYPE_PKT_IGMV;
				} else {
					ptype |= TXGBE_PTYPE_PKT_IGM;
				}
			}

		} else if (tun_prot == IPPROTO_GRE) {
			hdr.raw = (void *)inner_ip_hdr(skb);
			if (skb->inner_protocol ==  htons(ETH_P_IP) ||
				skb->inner_protocol ==  htons(ETH_P_IPV6)) {
				ptype |= TXGBE_PTYPE_PKT_IG;
			} else {
				if (((struct ethhdr *)
					skb_inner_mac_header(skb))->h_proto
					== htons(ETH_P_8021Q)) {
					ptype |= TXGBE_PTYPE_PKT_IGMV;
				} else {
					ptype |= TXGBE_PTYPE_PKT_IGM;
				}
			}
		} else {
			goto exit;
		}

		switch (hdr.ipv4->version) {
		case IPVERSION:
			l4_prot = hdr.ipv4->protocol;
			if (hdr.ipv4->frag_off & htons(IP_MF | IP_OFFSET)) {
				ptype |= TXGBE_PTYPE_TYP_IPFRAG;
				goto exit;
			}
			break;
		case 6:
			l4_prot = get_ipv6_proto(skb,
						 skb_inner_network_offset(skb));
			ptype |= TXGBE_PTYPE_PKT_IPV6;
			if (l4_prot == NEXTHDR_FRAGMENT) {
				ptype |= TXGBE_PTYPE_TYP_IPFRAG;
				goto exit;
			}
			break;
		default:
			goto exit;
		}
	} else {
encap_frag:

		switch (first->protocol) {
		case __constant_htons(ETH_P_IP):
			l4_prot = ip_hdr(skb)->protocol;
			ptype = TXGBE_PTYPE_PKT_IP;
			if (ip_hdr(skb)->frag_off & htons(IP_MF | IP_OFFSET)) {
				ptype |= TXGBE_PTYPE_TYP_IPFRAG;
				goto exit;
			}
			break;
#ifdef NETIF_F_IPV6_CSUM
		case __constant_htons(ETH_P_IPV6):
			l4_prot = get_ipv6_proto(skb, skb_network_offset(skb));
			ptype = TXGBE_PTYPE_PKT_IP | TXGBE_PTYPE_PKT_IPV6;
			if (l4_prot == NEXTHDR_FRAGMENT) {
				ptype |= TXGBE_PTYPE_TYP_IPFRAG;
				goto exit;
			}
			break;
#endif /* NETIF_F_IPV6_CSUM */
		case __constant_htons(ETH_P_1588):
			ptype = TXGBE_PTYPE_L2_TS;
			goto exit;
		case __constant_htons(ETH_P_FIP):
			ptype = TXGBE_PTYPE_L2_FIP;
			goto exit;
		case __constant_htons(TXGBE_ETH_P_LLDP):
			ptype = TXGBE_PTYPE_L2_LLDP;
			goto exit;
		case __constant_htons(TXGBE_ETH_P_CNM):
			ptype = TXGBE_PTYPE_L2_CNM;
			goto exit;
		case __constant_htons(ETH_P_PAE):
			ptype = TXGBE_PTYPE_L2_EAPOL;
			goto exit;
		case __constant_htons(ETH_P_ARP):
			ptype = TXGBE_PTYPE_L2_ARP;
			goto exit;
		default:
			ptype = TXGBE_PTYPE_L2_MAC;
			goto exit;
		}

	}

	switch (l4_prot) {
	case IPPROTO_TCP:
		ptype |= TXGBE_PTYPE_TYP_TCP;
		break;
	case IPPROTO_UDP:
		ptype |= TXGBE_PTYPE_TYP_UDP;
		break;
	case IPPROTO_SCTP:
		ptype |= TXGBE_PTYPE_TYP_SCTP;
		break;
	default:
		ptype |= TXGBE_PTYPE_TYP_IP;
		break;
	}

exit:
	return txgbe_decode_ptype(ptype);
}

static int txgbe_tso(struct txgbe_ring *tx_ring,
		     struct txgbe_tx_buffer *first,
		     u8 *hdr_len,  txgbe_dptype dptype)
{
	struct sk_buff *skb = first->skb;
	u32 vlan_macip_lens, type_tucmd;
	u32 mss_l4len_idx, l4len;
	struct tcphdr *tcph;
	struct iphdr *iph;
	u32 tunhdr_eiplen_tunlen = 0;

	u8 tun_prot = 0;
	bool enc = skb->encapsulation;

		struct ipv6hdr *ipv6h;

	if (skb->ip_summed != CHECKSUM_PARTIAL)
		return 0;

	if (!skb_is_gso(skb))
		return 0;

	if (skb_header_cloned(skb)) {
		int err = pskb_expand_head(skb, 0, 0, GFP_ATOMIC);
		if (err)
			return err;
	}

	iph = enc ? inner_ip_hdr(skb) : ip_hdr(skb);

	if (iph->version == 4) {

		tcph = enc ? inner_tcp_hdr(skb) : tcp_hdr(skb);

		iph->tot_len = 0;
		iph->check = 0;
		tcph->check = ~csum_tcpudp_magic(iph->saddr,
						iph->daddr, 0,
						IPPROTO_TCP,
						0);
		first->tx_flags |= TXGBE_TX_FLAGS_TSO |
		   TXGBE_TX_FLAGS_CSUM |
		   TXGBE_TX_FLAGS_IPV4 |
		   TXGBE_TX_FLAGS_CC;

	} else if (iph->version == 6 && skb_is_gso_v6(skb)) {

		ipv6h = enc ? inner_ipv6_hdr(skb) : ipv6_hdr(skb);
		tcph = enc ? inner_tcp_hdr(skb) : tcp_hdr(skb);

		ipv6h->payload_len = 0;
		tcph->check =
		    ~csum_ipv6_magic(&ipv6h->saddr,
				     &ipv6h->daddr,
				     0, IPPROTO_TCP, 0);
		first->tx_flags |= TXGBE_TX_FLAGS_TSO |
				   TXGBE_TX_FLAGS_CSUM |
				   TXGBE_TX_FLAGS_CC;
	}

	/* compute header lengths */

	l4len = enc ? inner_tcp_hdrlen(skb) : tcp_hdrlen(skb);
	*hdr_len = enc ? (skb_inner_transport_header(skb) - skb->data)
		       : skb_transport_offset(skb);
	*hdr_len += l4len;

	/* update gso size and bytecount with header size */
	first->gso_segs = skb_shinfo(skb)->gso_segs;
	first->bytecount += (first->gso_segs - 1) * *hdr_len;

	/* mss_l4len_id: use 0 as index for TSO */
	mss_l4len_idx = l4len << TXGBE_TXD_L4LEN_SHIFT;
	mss_l4len_idx |= skb_shinfo(skb)->gso_size << TXGBE_TXD_MSS_SHIFT;

	/* vlan_macip_lens: HEADLEN, MACLEN, VLAN tag */

	if (enc) {
		switch (first->protocol) {
		case __constant_htons(ETH_P_IP):
			tun_prot = ip_hdr(skb)->protocol;
			first->tx_flags |= TXGBE_TX_FLAGS_OUTER_IPV4;
			break;
		case __constant_htons(ETH_P_IPV6):
			tun_prot = ipv6_hdr(skb)->nexthdr;
			break;
		default:
			break;
		}
		switch (tun_prot) {
		case IPPROTO_UDP:
			tunhdr_eiplen_tunlen = TXGBE_TXD_TUNNEL_UDP;
			tunhdr_eiplen_tunlen |=
					((skb_network_header_len(skb) >> 2) <<
					TXGBE_TXD_OUTER_IPLEN_SHIFT) |
					(((skb_inner_mac_header(skb) -
					skb_transport_header(skb)) >> 1) <<
					TXGBE_TXD_TUNNEL_LEN_SHIFT);
			break;
		case IPPROTO_GRE:
			tunhdr_eiplen_tunlen = TXGBE_TXD_TUNNEL_GRE;
			tunhdr_eiplen_tunlen |=
					((skb_network_header_len(skb) >> 2) <<
					TXGBE_TXD_OUTER_IPLEN_SHIFT) |
					(((skb_inner_mac_header(skb) -
					skb_transport_header(skb)) >> 1) <<
					TXGBE_TXD_TUNNEL_LEN_SHIFT);
			break;
		case IPPROTO_IPIP:
			tunhdr_eiplen_tunlen = (((char *)inner_ip_hdr(skb)-
						(char *)ip_hdr(skb)) >> 2) <<
						TXGBE_TXD_OUTER_IPLEN_SHIFT;
			break;
		default:
			break;
		}

		vlan_macip_lens = skb_inner_network_header_len(skb) >> 1;
	} else
		vlan_macip_lens = skb_network_header_len(skb) >> 1;

	vlan_macip_lens |= skb_network_offset(skb) << TXGBE_TXD_MACLEN_SHIFT;
	vlan_macip_lens |= first->tx_flags & TXGBE_TX_FLAGS_VLAN_MASK;

	type_tucmd = dptype.ptype << 24;
	txgbe_tx_ctxtdesc(tx_ring, vlan_macip_lens, tunhdr_eiplen_tunlen,
		type_tucmd, mss_l4len_idx);

	return 1;
}

static void txgbe_tx_csum(struct txgbe_ring *tx_ring,
			  struct txgbe_tx_buffer *first, txgbe_dptype dptype)
{
	struct sk_buff *skb = first->skb;
	u32 vlan_macip_lens = 0;
	u32 mss_l4len_idx = 0;
	u32 tunhdr_eiplen_tunlen = 0;

	u8 tun_prot = 0;

	u32 type_tucmd;

	if (skb->ip_summed != CHECKSUM_PARTIAL) {
		if (!(first->tx_flags & TXGBE_TX_FLAGS_HW_VLAN) &&
		    !(first->tx_flags & TXGBE_TX_FLAGS_CC))
			return;
		vlan_macip_lens = skb_network_offset(skb) <<
				  TXGBE_TXD_MACLEN_SHIFT;
	} else {
		u8 l4_prot = 0;

		union {
			struct iphdr *ipv4;
			struct ipv6hdr *ipv6;
			u8 *raw;
		} network_hdr;
		union {
			struct tcphdr *tcphdr;
			u8 *raw;
		} transport_hdr;

		if (skb->encapsulation) {
			network_hdr.raw = skb_inner_network_header(skb);
			transport_hdr.raw = skb_inner_transport_header(skb);
			vlan_macip_lens = skb_network_offset(skb) <<
					  TXGBE_TXD_MACLEN_SHIFT;
			switch (first->protocol) {
			case __constant_htons(ETH_P_IP):
				tun_prot = ip_hdr(skb)->protocol;
				break;
			case __constant_htons(ETH_P_IPV6):
				tun_prot = ipv6_hdr(skb)->nexthdr;
				break;
			default:
				if (unlikely(net_ratelimit())) {
					dev_warn(tx_ring->dev,
					 "partial checksum but version=%d\n",
					 network_hdr.ipv4->version);
				}
				return;
			}
			switch (tun_prot) {
			case IPPROTO_UDP:
				tunhdr_eiplen_tunlen = TXGBE_TXD_TUNNEL_UDP;
				tunhdr_eiplen_tunlen |=
					((skb_network_header_len(skb) >> 2) <<
					TXGBE_TXD_OUTER_IPLEN_SHIFT) |
					(((skb_inner_mac_header(skb) -
					skb_transport_header(skb)) >> 1) <<
					TXGBE_TXD_TUNNEL_LEN_SHIFT);
				break;
			case IPPROTO_GRE:
				tunhdr_eiplen_tunlen = TXGBE_TXD_TUNNEL_GRE;
				tunhdr_eiplen_tunlen |=
					((skb_network_header_len(skb) >> 2) <<
					TXGBE_TXD_OUTER_IPLEN_SHIFT) |
					(((skb_inner_mac_header(skb) -
					skb_transport_header(skb)) >> 1) <<
					TXGBE_TXD_TUNNEL_LEN_SHIFT);
				break;
			case IPPROTO_IPIP:
				tunhdr_eiplen_tunlen =
					(((char *)inner_ip_hdr(skb)-
					(char *)ip_hdr(skb)) >> 2) <<
					TXGBE_TXD_OUTER_IPLEN_SHIFT;
				break;
			default:
				break;
			}

		} else {
			network_hdr.raw = skb_network_header(skb);
			transport_hdr.raw = skb_transport_header(skb);
			vlan_macip_lens = skb_network_offset(skb) <<
					  TXGBE_TXD_MACLEN_SHIFT;
		}

		switch (network_hdr.ipv4->version) {
		case IPVERSION:
			vlan_macip_lens |=
				(transport_hdr.raw - network_hdr.raw) >> 1;
			l4_prot = network_hdr.ipv4->protocol;
			break;
		case 6:
			vlan_macip_lens |=
				(transport_hdr.raw - network_hdr.raw) >> 1;
			l4_prot = network_hdr.ipv6->nexthdr;
			break;
		default:
			break;
		}

		switch (l4_prot) {
		case IPPROTO_TCP:

		mss_l4len_idx = (transport_hdr.tcphdr->doff * 4) <<
				TXGBE_TXD_L4LEN_SHIFT;
			break;
		case IPPROTO_SCTP:
			mss_l4len_idx = sizeof(struct sctphdr) <<
					TXGBE_TXD_L4LEN_SHIFT;
			break;
		case IPPROTO_UDP:
			mss_l4len_idx = sizeof(struct udphdr) <<
					TXGBE_TXD_L4LEN_SHIFT;
			break;
		default:
			break;
		}

		/* update TX checksum flag */
		first->tx_flags |= TXGBE_TX_FLAGS_CSUM;
	}
	first->tx_flags |= TXGBE_TX_FLAGS_CC;
	/* vlan_macip_lens: MACLEN, VLAN tag */
	vlan_macip_lens |= first->tx_flags & TXGBE_TX_FLAGS_VLAN_MASK;

	type_tucmd = dptype.ptype << 24;
	txgbe_tx_ctxtdesc(tx_ring, vlan_macip_lens, tunhdr_eiplen_tunlen,
			 type_tucmd, mss_l4len_idx);
}

static u32 txgbe_tx_cmd_type(u32 tx_flags)
{
	/* set type for advanced descriptor with frame checksum insertion */
	u32 cmd_type = TXGBE_TXD_DTYP_DATA |
		       TXGBE_TXD_IFCS;

	/* set HW vlan bit if vlan is present */
	cmd_type |= TXGBE_SET_FLAG(tx_flags, TXGBE_TX_FLAGS_HW_VLAN,
				   TXGBE_TXD_VLE);

	/* set segmentation enable bits for TSO/FSO */
	cmd_type |= TXGBE_SET_FLAG(tx_flags, TXGBE_TX_FLAGS_TSO,
				   TXGBE_TXD_TSE);

	/* set timestamp bit if present */
	cmd_type |= TXGBE_SET_FLAG(tx_flags, TXGBE_TX_FLAGS_TSTAMP,
				   TXGBE_TXD_MAC_TSTAMP);

	cmd_type |= TXGBE_SET_FLAG(tx_flags, TXGBE_TX_FLAGS_LINKSEC,
				   TXGBE_TXD_LINKSEC);

	return cmd_type;
}

static void txgbe_tx_olinfo_status(union txgbe_tx_desc *tx_desc,
				   u32 tx_flags, unsigned int paylen)
{
	u32 olinfo_status = paylen << TXGBE_TXD_PAYLEN_SHIFT;

	/* enable L4 checksum for TSO and TX checksum offload */
	olinfo_status |= TXGBE_SET_FLAG(tx_flags,
					TXGBE_TX_FLAGS_CSUM,
					TXGBE_TXD_L4CS);

	/* enble IPv4 checksum for TSO */
	olinfo_status |= TXGBE_SET_FLAG(tx_flags,
					TXGBE_TX_FLAGS_IPV4,
					TXGBE_TXD_IIPCS);
	/* enable outer IPv4 checksum for TSO */
	olinfo_status |= TXGBE_SET_FLAG(tx_flags,
					TXGBE_TX_FLAGS_OUTER_IPV4,
					TXGBE_TXD_EIPCS);
	/*
	 * Check Context must be set if Tx switch is enabled, which it
	 * always is for case where virtual functions are running
	 */
	olinfo_status |= TXGBE_SET_FLAG(tx_flags,
					TXGBE_TX_FLAGS_CC,
					TXGBE_TXD_CC);

	olinfo_status |= TXGBE_SET_FLAG(tx_flags,
					TXGBE_TX_FLAGS_IPSEC,
					TXGBE_TXD_IPSEC);

	tx_desc->read.olinfo_status = cpu_to_le32(olinfo_status);
}

static int __txgbe_maybe_stop_tx(struct txgbe_ring *tx_ring, u16 size)
{
	netif_stop_subqueue(tx_ring->netdev, tx_ring->queue_index);

	/* Herbert's original patch had:
	 *  smp_mb__after_netif_stop_queue();
	 * but since that doesn't exist yet, just open code it.
	 */
	smp_mb();

	/* We need to check again in a case another CPU has just
	 * made room available.
	 */
	if (likely(txgbe_desc_unused(tx_ring) < size))
		return -EBUSY;

	/* A reprieve! - use start_queue because it doesn't call schedule */
	netif_start_subqueue(tx_ring->netdev, tx_ring->queue_index);
	++tx_ring->tx_stats.restart_queue;
	return 0;
}

static inline int txgbe_maybe_stop_tx(struct txgbe_ring *tx_ring, u16 size)
{
	if (likely(txgbe_desc_unused(tx_ring) >= size))
		return 0;

	return __txgbe_maybe_stop_tx(tx_ring, size);
}

#define TXGBE_TXD_CMD (TXGBE_TXD_EOP | \
		       TXGBE_TXD_RS)

static int txgbe_tx_map(struct txgbe_ring *tx_ring,
			 struct txgbe_tx_buffer *first,
			 const u8 hdr_len)
{
	struct sk_buff *skb = first->skb;
	struct txgbe_tx_buffer *tx_buffer;
	union txgbe_tx_desc *tx_desc;
	skb_frag_t *frag;
	dma_addr_t dma;
	unsigned int data_len, size;
	u32 tx_flags = first->tx_flags;
	u32 cmd_type = txgbe_tx_cmd_type(tx_flags);
	u16 i = tx_ring->next_to_use;

	tx_desc = TXGBE_TX_DESC(tx_ring, i);

	txgbe_tx_olinfo_status(tx_desc, tx_flags, skb->len - hdr_len);

	size = skb_headlen(skb);
	data_len = skb->data_len;

	dma = dma_map_single(tx_ring->dev, skb->data, size, DMA_TO_DEVICE);

	tx_buffer = first;

	for (frag = &skb_shinfo(skb)->frags[0];; frag++) {
		if (dma_mapping_error(tx_ring->dev, dma))
			goto dma_error;

		/* record length, and DMA address */
		dma_unmap_len_set(tx_buffer, len, size);
		dma_unmap_addr_set(tx_buffer, dma, dma);

		tx_desc->read.buffer_addr = cpu_to_le64(dma);

		while (unlikely(size > TXGBE_MAX_DATA_PER_TXD)) {
			tx_desc->read.cmd_type_len =
				cpu_to_le32(cmd_type ^ TXGBE_MAX_DATA_PER_TXD);

			i++;
			tx_desc++;
			if (i == tx_ring->count) {
				tx_desc = TXGBE_TX_DESC(tx_ring, 0);
				i = 0;
			}
			tx_desc->read.olinfo_status = 0;

			dma += TXGBE_MAX_DATA_PER_TXD;
			size -= TXGBE_MAX_DATA_PER_TXD;

			tx_desc->read.buffer_addr = cpu_to_le64(dma);
		}

		if (likely(!data_len))
			break;

		tx_desc->read.cmd_type_len = cpu_to_le32(cmd_type ^ size);

		i++;
		tx_desc++;
		if (i == tx_ring->count) {
			tx_desc = TXGBE_TX_DESC(tx_ring, 0);
			i = 0;
		}
		tx_desc->read.olinfo_status = 0;

		size = skb_frag_size(frag);

		data_len -= size;

		dma = skb_frag_dma_map(tx_ring->dev, frag, 0, size,
				       DMA_TO_DEVICE);

		tx_buffer = &tx_ring->tx_buffer_info[i];
	}

	/* write last descriptor with RS and EOP bits */
	cmd_type |= size | TXGBE_TXD_CMD;
	tx_desc->read.cmd_type_len = cpu_to_le32(cmd_type);

	netdev_tx_sent_queue(txring_txq(tx_ring), first->bytecount);

	/* set the timestamp */
	first->time_stamp = jiffies;

	/*
	 * Force memory writes to complete before letting h/w know there
	 * are new descriptors to fetch.  (Only applicable for weak-ordered
	 * memory model archs, such as IA-64).
	 *
	 * We also need this memory barrier to make certain all of the
	 * status bits have been updated before next_to_watch is written.
	 */
	wmb();

	/* set next_to_watch value indicating a packet is present */
	first->next_to_watch = tx_desc;

	i++;
	if (i == tx_ring->count)
		i = 0;

	tx_ring->next_to_use = i;

	txgbe_maybe_stop_tx(tx_ring, DESC_NEEDED);

	skb_tx_timestamp(skb);

	if (netif_xmit_stopped(txring_txq(tx_ring)) || !netdev_xmit_more()) {
		writel(i, tx_ring->tail);
		/* The following mmiowb() is required on certain
		 * architechtures (IA64/Altix in particular) in order to
		 * synchronize the I/O calls with respect to a spin lock. This
		 * is because the wmb() on those architectures does not
		 * guarantee anything for posted I/O writes.
		 *
		 * Note that the associated spin_unlock() is not within the
		 * driver code, but in the networking core stack.
		 */
		mmiowb();
	}

	return 0;
dma_error:
	dev_err(tx_ring->dev, "TX DMA map failed\n");

	/* clear dma mappings for failed tx_buffer_info map */
	for (;;) {
		tx_buffer = &tx_ring->tx_buffer_info[i];
		if (dma_unmap_len(tx_buffer, len))
			dma_unmap_page(tx_ring->dev,
				       dma_unmap_addr(tx_buffer, dma),
				       dma_unmap_len(tx_buffer, len),
				       DMA_TO_DEVICE);
		dma_unmap_len_set(tx_buffer, len, 0);
		if (tx_buffer == first)
			break;
		if (i == 0)
			i += tx_ring->count;
		i--;
	}

	dev_kfree_skb_any(first->skb);
	first->skb = NULL;

	tx_ring->next_to_use = i;

	return -1;
}

static void txgbe_atr(struct txgbe_ring *ring,
		      struct txgbe_tx_buffer *first,
		      txgbe_dptype dptype)
{
	struct txgbe_q_vector *q_vector = ring->q_vector;
	union txgbe_atr_hash_dword input = { .dword = 0 };
	union txgbe_atr_hash_dword common = { .dword = 0 };
	union network_header hdr;
	struct tcphdr *th;

	/* if ring doesn't have a interrupt vector, cannot perform ATR */
	if (!q_vector)
		return;

	/* do nothing if sampling is disabled */
	if (!ring->atr_sample_rate)
		return;

	ring->atr_count++;

	if (dptype.etype) {
		if (TXGBE_PTYPE_TYP_TCP != TXGBE_PTYPE_TYPL4(dptype.ptype))
			return;
		hdr.raw = (void *)skb_inner_network_header(first->skb);
		th = inner_tcp_hdr(first->skb);
	} else

	{
		if (TXGBE_PTYPE_PKT_IP != TXGBE_PTYPE_PKT(dptype.ptype) ||
		    TXGBE_PTYPE_TYP_TCP != TXGBE_PTYPE_TYPL4(dptype.ptype))
			return;
		hdr.raw = (void *)skb_network_header(first->skb);
		th = tcp_hdr(first->skb);
	}

	/* skip this packet since it is invalid or the socket is closing */
	if (!th || th->fin)
		return;

	/* sample on all syn packets or once every atr sample count */
	if (!th->syn && (ring->atr_count < ring->atr_sample_rate))
		return;

	/* reset sample count */
	ring->atr_count = 0;

	/*
	 * src and dst are inverted, think how the receiver sees them
	 *
	 * The input is broken into two sections, a non-compressed section
	 * containing vm_pool, vlan_id, and flow_type.  The rest of the data
	 * is XORed together and stored in the compressed dword.
	 */
	input.formatted.vlan_id = htons((u16)dptype.ptype);

	/*
	 * since src port and flex bytes occupy the same word XOR them together
	 * and write the value to source port portion of compressed dword
	 */
	if (first->tx_flags & TXGBE_TX_FLAGS_SW_VLAN)
		common.port.src ^= th->dest ^ first->skb->protocol;
	else if (first->tx_flags & TXGBE_TX_FLAGS_HW_VLAN)
		common.port.src ^= th->dest ^ first->skb->vlan_proto;
	else
		common.port.src ^= th->dest ^ first->protocol;
	common.port.dst ^= th->source;

	if (TXGBE_PTYPE_PKT_IPV6 & TXGBE_PTYPE_PKT(dptype.ptype)) {
		input.formatted.flow_type = TXGBE_ATR_FLOW_TYPE_TCPV6;
		common.ip ^= hdr.ipv6->saddr.s6_addr32[0] ^
					 hdr.ipv6->saddr.s6_addr32[1] ^
					 hdr.ipv6->saddr.s6_addr32[2] ^
					 hdr.ipv6->saddr.s6_addr32[3] ^
					 hdr.ipv6->daddr.s6_addr32[0] ^
					 hdr.ipv6->daddr.s6_addr32[1] ^
					 hdr.ipv6->daddr.s6_addr32[2] ^
					 hdr.ipv6->daddr.s6_addr32[3];
	} else {
		input.formatted.flow_type = TXGBE_ATR_FLOW_TYPE_TCPV4;
		common.ip ^= hdr.ipv4->saddr ^ hdr.ipv4->daddr;
	}

	/* This assumes the Rx queue and Tx queue are bound to the same CPU */
	txgbe_fdir_add_signature_filter(&q_vector->adapter->hw,
					      input, common, ring->queue_index);
}

/**
 *	skb_pad			-	zero pad the tail of an skb
 *	@skb: buffer to pad
 *	@pad: space to pad
 *
 *	Ensure that a buffer is followed by a padding area that is zero
 *	filled. Used by network drivers which may DMA or transfer data
 *	beyond the buffer end onto the wire.
 *
 *	May return error in out of memory cases. The skb is freed on error.
 */

int txgbe_skb_pad_nonzero(struct sk_buff *skb, int pad)
{
	int err;
	int ntail;

	/* If the skbuff is non linear tailroom is always zero.. */
	if (!skb_cloned(skb) && skb_tailroom(skb) >= pad) {
		memset(skb->data+skb->len, 0x1, pad);
		return 0;
	}

	ntail = skb->data_len + pad - (skb->end - skb->tail);
	if (likely(skb_cloned(skb) || ntail > 0)) {
		err = pskb_expand_head(skb, 0, ntail, GFP_ATOMIC);
		if (unlikely(err))
			goto free_skb;
	}

	/* FIXME: The use of this function with non-linear skb's really needs
	 * to be audited.
	 */
	err = skb_linearize(skb);
	if (unlikely(err))
		goto free_skb;

	memset(skb->data + skb->len, 0x1, pad);
	return 0;

free_skb:
	kfree_skb(skb);
	return err;
}

netdev_tx_t txgbe_xmit_frame_ring(struct sk_buff *skb,
				  struct txgbe_adapter *adapter,
				  struct txgbe_ring *tx_ring)
{
	struct txgbe_tx_buffer *first;
	int tso;
	u32 tx_flags = 0;
	unsigned short f;
	u16 count = TXD_USE_COUNT(skb_headlen(skb));
	__be16 protocol = skb->protocol;
	u8 hdr_len = 0;
	txgbe_dptype dptype;

	/* work around hw errata 3 */
	u16 _llcLen, *llcLen;
	llcLen = skb_header_pointer(skb, ETH_HLEN - 2, sizeof(u16), &_llcLen);
	if (*llcLen == 0x3 || *llcLen == 0x4 || *llcLen == 0x5) {
		if (txgbe_skb_pad_nonzero(skb, ETH_ZLEN - skb->len))
			return -ENOMEM;
		__skb_put(skb, ETH_ZLEN - skb->len);
	}

	/*
	 * need: 1 descriptor per page * PAGE_SIZE/TXGBE_MAX_DATA_PER_TXD,
	 *       + 1 desc for skb_headlen/TXGBE_MAX_DATA_PER_TXD,
	 *       + 2 desc gap to keep tail from touching head,
	 *       + 1 desc for context descriptor,
	 * otherwise try next time
	 */
	for (f = 0; f < skb_shinfo(skb)->nr_frags; f++)
		count += TXD_USE_COUNT(skb_frag_size(&skb_shinfo(skb)->
						     frags[f]));

	if (txgbe_maybe_stop_tx(tx_ring, count + 3)) {
		tx_ring->tx_stats.tx_busy++;
		return NETDEV_TX_BUSY;
	}

	/* record the location of the first descriptor for this packet */
	first = &tx_ring->tx_buffer_info[tx_ring->next_to_use];
	first->skb = skb;
	first->bytecount = skb->len;
	first->gso_segs = 1;

	/* if we have a HW VLAN tag being added default to the HW one */
	if (skb_vlan_tag_present(skb)) {
		tx_flags |= skb_vlan_tag_get(skb) << TXGBE_TX_FLAGS_VLAN_SHIFT;
		tx_flags |= TXGBE_TX_FLAGS_HW_VLAN;
	/* else if it is a SW VLAN check the next protocol and store the tag */
	} else if (protocol == htons(ETH_P_8021Q)) {
		struct vlan_hdr *vhdr, _vhdr;
		vhdr = skb_header_pointer(skb, ETH_HLEN, sizeof(_vhdr), &_vhdr);
		if (!vhdr)
			goto out_drop;

		protocol = vhdr->h_vlan_encapsulated_proto;
		tx_flags |= ntohs(vhdr->h_vlan_TCI) <<
				  TXGBE_TX_FLAGS_VLAN_SHIFT;
		tx_flags |= TXGBE_TX_FLAGS_SW_VLAN;
	}

	if (unlikely(skb_shinfo(skb)->tx_flags & SKBTX_HW_TSTAMP) &&
		adapter->ptp_clock) {
		if (!test_and_set_bit_lock(__TXGBE_PTP_TX_IN_PROGRESS,
				   &adapter->state)) {
			skb_shinfo(skb)->tx_flags |= SKBTX_IN_PROGRESS;
			tx_flags |= TXGBE_TX_FLAGS_TSTAMP;

			/* schedule check for Tx timestamp */
			adapter->ptp_tx_skb = skb_get(skb);
			adapter->ptp_tx_start = jiffies;
			schedule_work(&adapter->ptp_tx_work);
		} else {
			adapter->tx_hwtstamp_skipped++;
		}
	}

	if ((adapter->flags & TXGBE_FLAG_DCB_ENABLED) &&
	    ((tx_flags & (TXGBE_TX_FLAGS_HW_VLAN | TXGBE_TX_FLAGS_SW_VLAN)) ||
	     (skb->priority != TC_PRIO_CONTROL))) {
		tx_flags &= ~TXGBE_TX_FLAGS_VLAN_PRIO_MASK;
			tx_flags |= skb->priority <<
				    TXGBE_TX_FLAGS_VLAN_PRIO_SHIFT;
		if (tx_flags & TXGBE_TX_FLAGS_SW_VLAN) {
			struct vlan_ethhdr *vhdr;
			if (skb_header_cloned(skb) &&
			    pskb_expand_head(skb, 0, 0, GFP_ATOMIC))
				goto out_drop;
			vhdr = (struct vlan_ethhdr *)skb->data;
			vhdr->h_vlan_TCI = htons(tx_flags >>
						 TXGBE_TX_FLAGS_VLAN_SHIFT);
		} else {
			tx_flags |= TXGBE_TX_FLAGS_HW_VLAN;
		}
	}

	/* record initial flags and protocol */
	first->tx_flags = tx_flags;
	first->protocol = protocol;

	dptype = encode_tx_desc_ptype(first);

	tso = txgbe_tso(tx_ring, first, &hdr_len, dptype);
	if (tso < 0)
		goto out_drop;
	else if (!tso)
		txgbe_tx_csum(tx_ring, first, dptype);

	/* add the ATR filter if ATR is on */
	if (test_bit(__TXGBE_TX_FDIR_INIT_DONE, &tx_ring->state))
		txgbe_atr(tx_ring, first, dptype);

	if (txgbe_tx_map(tx_ring, first, hdr_len))
		goto cleanup_tx_tstamp;

	return NETDEV_TX_OK;

out_drop:
	dev_kfree_skb_any(first->skb);
	first->skb = NULL;

cleanup_tx_tstamp:
	if (unlikely(tx_flags & TXGBE_TX_FLAGS_TSTAMP)) {
		dev_kfree_skb_any(adapter->ptp_tx_skb);
		adapter->ptp_tx_skb = NULL;
		cancel_work_sync(&adapter->ptp_tx_work);
		clear_bit_unlock(__TXGBE_PTP_TX_IN_PROGRESS, &adapter->state);
	}

	return NETDEV_TX_OK;
}

static netdev_tx_t txgbe_xmit_frame(struct sk_buff *skb,
				    struct net_device *netdev)
{
	struct txgbe_adapter *adapter = netdev_priv(netdev);
	struct txgbe_ring *tx_ring;
	unsigned int r_idx = skb->queue_mapping;

	if (!netif_carrier_ok(netdev)) {
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}

	/*
	 * The minimum packet size for olinfo paylen is 17 so pad the skb
	 * in order to meet this minimum size requirement.
	 */
	if (skb_put_padto(skb, 17))
		return NETDEV_TX_OK;

	if (r_idx >= adapter->num_tx_queues)
		r_idx = r_idx % adapter->num_tx_queues;
	tx_ring = adapter->tx_ring[r_idx];

	return txgbe_xmit_frame_ring(skb, adapter, tx_ring);
}

/**
 * txgbe_set_mac - Change the Ethernet Address of the NIC
 * @netdev: network interface device structure
 * @p: pointer to an address structure
 *
 * Returns 0 on success, negative on failure
 **/
static int txgbe_set_mac(struct net_device *netdev, void *p)
{
	struct txgbe_adapter *adapter = netdev_priv(netdev);
	struct txgbe_hw *hw = &adapter->hw;
	struct sockaddr *addr = p;

	if (!is_valid_ether_addr(addr->sa_data))
		return -EADDRNOTAVAIL;

	txgbe_del_mac_filter(adapter, hw->mac.addr, VMDQ_P(0));
	memcpy(netdev->dev_addr, addr->sa_data, netdev->addr_len);
	memcpy(hw->mac.addr, addr->sa_data, netdev->addr_len);

	txgbe_mac_set_default_filter(adapter, hw->mac.addr);

	return 0;
}

/**
 * txgbe_add_sanmac_netdev - Add the SAN MAC address to the corresponding
 * netdev->dev_addr_list
 * @netdev: network interface device structure
 *
 * Returns non-zero on failure
 **/
static int txgbe_add_sanmac_netdev(struct net_device *dev)
{
	int err = 0;
	struct txgbe_adapter *adapter = netdev_priv(dev);
	struct txgbe_hw *hw = &adapter->hw;

	if (is_valid_ether_addr(hw->mac.san_addr)) {
		rtnl_lock();
		err = dev_addr_add(dev, hw->mac.san_addr,
				   NETDEV_HW_ADDR_T_SAN);
		rtnl_unlock();

		/* update SAN MAC vmdq pool selection */
		TCALL(hw, mac.ops.set_vmdq_san_mac, VMDQ_P(0));
	}
	return err;
}

/**
 * txgbe_del_sanmac_netdev - Removes the SAN MAC address to the corresponding
 * netdev->dev_addr_list
 * @netdev: network interface device structure
 *
 * Returns non-zero on failure
 **/
static int txgbe_del_sanmac_netdev(struct net_device *dev)
{
	int err = 0;
	struct txgbe_adapter *adapter = netdev_priv(dev);
	struct txgbe_mac_info *mac = &adapter->hw.mac;

	if (is_valid_ether_addr(mac->san_addr)) {
		rtnl_lock();
		err = dev_addr_del(dev, mac->san_addr, NETDEV_HW_ADDR_T_SAN);
		rtnl_unlock();
	}
	return err;
}

static int txgbe_mii_ioctl(struct net_device *netdev, struct ifreq *ifr,
			   int cmd)
{
	struct mii_ioctl_data *mii = (struct mii_ioctl_data *) &ifr->ifr_data;
	int prtad, devad, ret;
	struct txgbe_adapter *adapter = netdev_priv(netdev);
	struct txgbe_hw *hw = &adapter->hw;
	u16 value = 0;

	prtad = (mii->phy_id & MDIO_PHY_ID_PRTAD) >> 5;
	devad = (mii->phy_id & MDIO_PHY_ID_DEVAD);

	if (cmd == SIOCGMIIREG) {
		ret = txgbe_read_mdio(&hw->phy_dev, prtad, devad, mii->reg_num,
					&value);
		if (ret < 0)
			return ret;
		mii->val_out = value;
		return 0;
	} else {
		return txgbe_write_mdio(&hw->phy_dev, prtad, devad,
			mii->reg_num, mii->val_in);
	}
}

static int txgbe_ioctl(struct net_device *netdev, struct ifreq *ifr, int cmd)
{

	struct txgbe_adapter *adapter = netdev_priv(netdev);

	switch (cmd) {
	case SIOCGHWTSTAMP:
		return txgbe_ptp_get_ts_config(adapter, ifr);
	case SIOCSHWTSTAMP:
		return txgbe_ptp_set_ts_config(adapter, ifr);
	case SIOCGMIIREG:
	case SIOCSMIIREG:
		return txgbe_mii_ioctl(netdev, ifr, cmd);
	default:
		return -EOPNOTSUPP;
	}
}

/* txgbe_validate_rtr - verify 802.1Qp to Rx packet buffer mapping is valid.
 * @adapter: pointer to txgbe_adapter
 * @tc: number of traffic classes currently enabled
 *
 * Configure a valid 802.1Qp to Rx packet buffer mapping ie confirm
 * 802.1Q priority maps to a packet buffer that exists.
 */
static void txgbe_validate_rtr(struct txgbe_adapter *adapter, u8 tc)
{
	struct txgbe_hw *hw = &adapter->hw;
	u32 reg, rsave;

	reg = rd32(hw, TXGBE_RDB_UP2TC);
	rsave = reg;
	if (reg != rsave)
		wr32(hw, TXGBE_RDB_UP2TC, reg);

	return;
}

/**
 * txgbe_set_prio_tc_map - Configure netdev prio tc map
 * @adapter: Pointer to adapter struct
 *
 * Populate the netdev user priority to tc map
 */
static void txgbe_set_prio_tc_map(struct txgbe_adapter __maybe_unused *adapter)
{
	UNREFERENCED_PARAMETER(adapter);
}

/**
 * txgbe_setup_tc - routine to configure net_device for multiple traffic
 * classes.
 *
 * @netdev: net device to configure
 * @tc: number of traffic classes to enable
 */
int txgbe_setup_tc(struct net_device *dev, u8 tc)
{
	struct txgbe_adapter *adapter = netdev_priv(dev);

	if (tc && adapter->num_vmdqs > TXGBE_MAX_DCBMACVLANS)
		return -EBUSY;

	/* Hardware has to reinitialize queues and interrupts to
	 * match packet buffer alignment. Unfortunately, the
	 * hardware is not flexible enough to do this dynamically.
	 */
	if (netif_running(dev))
		txgbe_close(dev);
	else
		txgbe_reset(adapter);

	txgbe_clear_interrupt_scheme(adapter);

	if (tc) {
		netdev_set_num_tc(dev, tc);
		txgbe_set_prio_tc_map(adapter);
	} else {
		netdev_reset_tc(dev);
	}

	txgbe_validate_rtr(adapter, tc);

	txgbe_init_interrupt_scheme(adapter);
	if (netif_running(dev))
		txgbe_open(dev);

	return 0;
}

static int txgbe_setup_tc_mqprio(struct net_device *dev,
				 struct tc_mqprio_qopt *mqprio)
{
	mqprio->hw = TC_MQPRIO_HW_OFFLOAD_TCS;
	return txgbe_setup_tc(dev, mqprio->num_tc);
}

static int __txgbe_setup_tc(struct net_device *dev, enum tc_setup_type type,
		 void *type_data)
{
	switch (type) {
	case TC_SETUP_QDISC_MQPRIO:
		return txgbe_setup_tc_mqprio(dev, type_data);
	default:
		return -EOPNOTSUPP;
	}
}

void txgbe_do_reset(struct net_device *netdev)
{
	struct txgbe_adapter *adapter = netdev_priv(netdev);

	if (netif_running(netdev))
		txgbe_reinit_locked(adapter);
	else
		txgbe_reset(adapter);
}

static netdev_features_t txgbe_fix_features(struct net_device *netdev,
					    netdev_features_t features)
{
	struct txgbe_adapter *adapter = netdev_priv(netdev);

	/* If Rx checksum is disabled, then RSC/LRO should also be disabled */
	if (!(features & NETIF_F_RXCSUM))
		features &= ~NETIF_F_LRO;

	/* Turn off LRO if not RSC capable */
	if (!(adapter->flags2 & TXGBE_FLAG2_RSC_CAPABLE))
		features &= ~NETIF_F_LRO;

	return features;
}

static int txgbe_set_features(struct net_device *netdev,
			      netdev_features_t features)
{
	struct txgbe_adapter *adapter = netdev_priv(netdev);
	bool need_reset = false;

	/* Make sure RSC matches LRO, reset if change */
	if (!(features & NETIF_F_LRO)) {
		if (adapter->flags2 & TXGBE_FLAG2_RSC_ENABLED)
			need_reset = true;
		adapter->flags2 &= ~TXGBE_FLAG2_RSC_ENABLED;
	} else if ((adapter->flags2 & TXGBE_FLAG2_RSC_CAPABLE) &&
		   !(adapter->flags2 & TXGBE_FLAG2_RSC_ENABLED)) {
		if (adapter->rx_itr_setting == 1 ||
		    adapter->rx_itr_setting > TXGBE_MIN_RSC_ITR) {
			adapter->flags2 |= TXGBE_FLAG2_RSC_ENABLED;
			need_reset = true;
		} else if ((netdev->features ^ features) & NETIF_F_LRO) {

			e_info(probe, "rx-usecs set too low, "
			       "disabling RSC\n");
		}
	}

	/*
	 * Check if Flow Director n-tuple support was enabled or disabled.  If
	 * the state changed, we need to reset.
	 */
	switch (features & NETIF_F_NTUPLE) {
	case NETIF_F_NTUPLE:
		/* turn off ATR, enable perfect filters and reset */
		if (!(adapter->flags & TXGBE_FLAG_FDIR_PERFECT_CAPABLE))
			need_reset = true;

		adapter->flags &= ~TXGBE_FLAG_FDIR_HASH_CAPABLE;
		adapter->flags |= TXGBE_FLAG_FDIR_PERFECT_CAPABLE;
		break;
	default:
		/* turn off perfect filters, enable ATR and reset */
		if (adapter->flags & TXGBE_FLAG_FDIR_PERFECT_CAPABLE)
			need_reset = true;

		adapter->flags &= ~TXGBE_FLAG_FDIR_PERFECT_CAPABLE;

		/* We cannot enable ATR if VMDq is enabled */
		if (adapter->flags & TXGBE_FLAG_VMDQ_ENABLED)
			break;

		/* We cannot enable ATR if we have 2 or more traffic classes */
		if (netdev_get_num_tc(netdev) > 1)
			break;

		/* We cannot enable ATR if RSS is disabled */
		if (adapter->ring_feature[RING_F_RSS].limit <= 1)
			break;

		/* A sample rate of 0 indicates ATR disabled */
		if (!adapter->atr_sample_rate)
			break;

		adapter->flags |= TXGBE_FLAG_FDIR_HASH_CAPABLE;
		break;
	}

	if (features & NETIF_F_HW_VLAN_CTAG_RX)
		txgbe_vlan_strip_enable(adapter);
	else
		txgbe_vlan_strip_disable(adapter);

	if (adapter->flags & TXGBE_FLAG_VXLAN_OFFLOAD_CAPABLE &&
	    features & NETIF_F_RXCSUM) {
		if (!need_reset)
			adapter->flags2 |= TXGBE_FLAG2_VXLAN_REREG_NEEDED;
	} else {
		txgbe_clear_vxlan_port(adapter);
	}

	if (features & NETIF_F_RXHASH) {
		if (!(adapter->flags2 & TXGBE_FLAG2_RSS_ENABLED)) {
			wr32m(&adapter->hw, TXGBE_RDB_RA_CTL,
				TXGBE_RDB_RA_CTL_RSS_EN, TXGBE_RDB_RA_CTL_RSS_EN);
			adapter->flags2 |= TXGBE_FLAG2_RSS_ENABLED;
		}
	} else {
		if (adapter->flags2 & TXGBE_FLAG2_RSS_ENABLED) {
			wr32m(&adapter->hw, TXGBE_RDB_RA_CTL,
				TXGBE_RDB_RA_CTL_RSS_EN, ~TXGBE_RDB_RA_CTL_RSS_EN);
			adapter->flags2 &= ~TXGBE_FLAG2_RSS_ENABLED;
		}
	}

	if (need_reset)
		txgbe_do_reset(netdev);

	return 0;
}

/**
 * txgbe_add_udp_tunnel_port - Get notifications about adding UDP tunnel ports
 * @dev: The port's netdev
 * @ti: Tunnel endpoint information
 **/
static void txgbe_add_udp_tunnel_port(struct net_device *dev,
				      struct udp_tunnel_info *ti)
{
	struct txgbe_adapter *adapter = netdev_priv(dev);
	struct txgbe_hw *hw = &adapter->hw;
	__be16 port = ti->port;

	if (ti->sa_family != AF_INET)
		return;

	switch (ti->type) {
	case UDP_TUNNEL_TYPE_VXLAN:
		if (!(adapter->flags & TXGBE_FLAG_VXLAN_OFFLOAD_CAPABLE))
			return;

		if (adapter->vxlan_port == port)
			return;

		if (adapter->vxlan_port) {
			netdev_info(dev,
				    "VXLAN port %d set, not adding port %d\n",
				    ntohs(adapter->vxlan_port),
				    ntohs(port));
			return;
		}

		adapter->vxlan_port = port;
		wr32(hw, TXGBE_CFG_VXLAN, port);
		break;
	case UDP_TUNNEL_TYPE_GENEVE:
		if (adapter->geneve_port == port)
			return;

		if (adapter->geneve_port) {
			netdev_info(dev,
				    "GENEVE port %d set, not adding port %d\n",
				    ntohs(adapter->geneve_port),
				    ntohs(port));
			return;
		}

		adapter->geneve_port = port;
		wr32(hw, TXGBE_CFG_GENEVE, port);
		break;
	default:
		return;
	}
}

/**
 * ixgbe_del_udp_tunnel_port - Get notifications about removing UDP tunnel ports
 * @dev: The port's netdev
 * @ti: Tunnel endpoint information
 **/
static void txgbe_del_udp_tunnel_port(struct net_device *dev,
				      struct udp_tunnel_info *ti)
{
	struct txgbe_adapter *adapter = netdev_priv(dev);

	if (ti->type != UDP_TUNNEL_TYPE_VXLAN &&
	    ti->type != UDP_TUNNEL_TYPE_GENEVE)
		return;

	if (ti->sa_family != AF_INET)
		return;

	switch (ti->type) {
	case UDP_TUNNEL_TYPE_VXLAN:
		if (!(adapter->flags & TXGBE_FLAG_VXLAN_OFFLOAD_CAPABLE))
			return;

		if (adapter->vxlan_port != ti->port) {
			netdev_info(dev, "VXLAN port %d not found\n",
				    ntohs(ti->port));
			return;
		}

		txgbe_clear_vxlan_port(adapter);
		adapter->flags2 |= TXGBE_FLAG2_VXLAN_REREG_NEEDED;
		break;
	case UDP_TUNNEL_TYPE_GENEVE:
		if (adapter->geneve_port != ti->port) {
			netdev_info(dev, "GENEVE port %d not found\n",
				    ntohs(ti->port));
			return;
		}

		adapter->geneve_port = 0;
		wr32(&adapter->hw, TXGBE_CFG_GENEVE, 0);
		break;
	default:
		return;
	}
}

static int txgbe_ndo_fdb_add(struct ndmsg *ndm, struct nlattr *tb[],
			     struct net_device *dev,
			     const unsigned char *addr,
			     u16 vid,
			     u16 flags)
{
	/* guarantee we can provide a unique filter for the unicast address */
	if (is_unicast_ether_addr(addr) || is_link_local_ether_addr(addr)) {
		if (TXGBE_MAX_PF_MACVLANS <= netdev_uc_count(dev))
			return -ENOMEM;
	}

	return ndo_dflt_fdb_add(ndm, tb, dev, addr, vid, flags);
}

static int txgbe_ndo_bridge_setlink(struct net_device *dev,
				    struct nlmsghdr *nlh,
				    __always_unused u16 flags)
{
	struct txgbe_adapter *adapter = netdev_priv(dev);
	struct nlattr *attr, *br_spec;
	int rem;

	if (!(adapter->flags & TXGBE_FLAG_SRIOV_ENABLED))
		return -EOPNOTSUPP;

	br_spec = nlmsg_find_attr(nlh, sizeof(struct ifinfomsg), IFLA_AF_SPEC);

	nla_for_each_nested(attr, br_spec, rem) {
		__u16 mode;

		if (nla_type(attr) != IFLA_BRIDGE_MODE)
			continue;

		mode = nla_get_u16(attr);
		if (mode == BRIDGE_MODE_VEPA) {
			adapter->flags |= TXGBE_FLAG_SRIOV_VEPA_BRIDGE_MODE;
		} else if (mode == BRIDGE_MODE_VEB) {
			adapter->flags &= ~TXGBE_FLAG_SRIOV_VEPA_BRIDGE_MODE;
		} else {
			return -EINVAL;
		}

		adapter->bridge_mode = mode;

		/* re-configure settings related to bridge mode */
		txgbe_configure_bridge_mode(adapter);

		e_info(drv, "enabling bridge mode: %s\n",
			mode == BRIDGE_MODE_VEPA ? "VEPA" : "VEB");
	}

	return 0;
}

static int txgbe_ndo_bridge_getlink(struct sk_buff *skb, u32 pid, u32 seq,
				    struct net_device *dev,
				    u32 __maybe_unused filter_mask,
				    int nlflags)
{
	struct txgbe_adapter *adapter = netdev_priv(dev);
	u16 mode;

	if (!(adapter->flags & TXGBE_FLAG_SRIOV_ENABLED))
		return 0;

	mode = adapter->bridge_mode;
	return ndo_dflt_bridge_getlink(skb, pid, seq, dev, mode, 0, 0, nlflags,
				       filter_mask, NULL);
}

#define TXGBE_MAX_TUNNEL_HDR_LEN 80
static netdev_features_t
txgbe_features_check(struct sk_buff *skb, struct net_device *dev,
		     netdev_features_t features)
{
	u32 vlan_num = 0;
	u16 vlan_depth = skb->mac_len;
	__be16 type = skb->protocol;
	struct vlan_hdr *vh;

	if (skb_vlan_tag_present(skb)) {
		vlan_num++;
	}

	if (vlan_depth) {
		vlan_depth -= VLAN_HLEN;
	} else {
		vlan_depth = ETH_HLEN;
	}

	while (type == htons(ETH_P_8021Q) || type == htons(ETH_P_8021AD)) {
		vlan_num++;
		vh = (struct vlan_hdr *)(skb->data + vlan_depth);
		type = vh->h_vlan_encapsulated_proto;
		vlan_depth += VLAN_HLEN;

	}

	if (vlan_num > 2)
		features &= ~(NETIF_F_HW_VLAN_CTAG_TX |
			    NETIF_F_HW_VLAN_STAG_TX);

	if (skb->encapsulation) {
		if (unlikely(skb_inner_mac_header(skb) -
			     skb_transport_header(skb) >
			     TXGBE_MAX_TUNNEL_HDR_LEN))
			return features & ~NETIF_F_CSUM_MASK;
	}
	return features;
}

static const struct net_device_ops txgbe_netdev_ops = {
	.ndo_open               = txgbe_open,
	.ndo_stop               = txgbe_close,
	.ndo_start_xmit         = txgbe_xmit_frame,
	.ndo_set_rx_mode        = txgbe_set_rx_mode,
	.ndo_validate_addr      = eth_validate_addr,
	.ndo_set_mac_address    = txgbe_set_mac,
	.ndo_change_mtu		    = txgbe_change_mtu,
	.ndo_tx_timeout         = txgbe_tx_timeout,
	.ndo_vlan_rx_add_vid    = txgbe_vlan_rx_add_vid,
	.ndo_vlan_rx_kill_vid   = txgbe_vlan_rx_kill_vid,
	.ndo_do_ioctl           = txgbe_ioctl,
	.ndo_get_stats64        = txgbe_get_stats64,
	.ndo_setup_tc           = __txgbe_setup_tc,
	.ndo_fdb_add            = txgbe_ndo_fdb_add,
	.ndo_bridge_setlink     = txgbe_ndo_bridge_setlink,
	.ndo_bridge_getlink     = txgbe_ndo_bridge_getlink,
	.ndo_udp_tunnel_add     = txgbe_add_udp_tunnel_port,
	.ndo_udp_tunnel_del     = txgbe_del_udp_tunnel_port,
	.ndo_features_check     = txgbe_features_check,
	.ndo_set_features       = txgbe_set_features,
	.ndo_fix_features       = txgbe_fix_features,
};

void txgbe_assign_netdev_ops(struct net_device *dev)
{
	dev->netdev_ops = &txgbe_netdev_ops;
	txgbe_set_ethtool_ops(dev);
	dev->watchdog_timeo = 5 * HZ;
}

/**
 * txgbe_wol_supported - Check whether device supports WoL
 * @adapter: the adapter private structure
 * @device_id: the device ID
 * @subdev_id: the subsystem device ID
 *
 * This function is used by probe and ethtool to determine
 * which devices have WoL support
 *
 **/
int txgbe_wol_supported(struct txgbe_adapter *adapter)
{
	struct txgbe_hw *hw = &adapter->hw;
	u16 wol_cap = adapter->eeprom_cap & TXGBE_DEVICE_CAPS_WOL_MASK;

	/* check eeprom to see if WOL is enabled */
	if ((wol_cap == TXGBE_DEVICE_CAPS_WOL_PORT0_1) ||
	    ((wol_cap == TXGBE_DEVICE_CAPS_WOL_PORT0) &&
	     (hw->bus.func == 0)))
		return true;
	else
		return false;
}

/**
 * txgbe_probe - Device Initialization Routine
 * @pdev: PCI device information struct
 * @ent: entry in txgbe_pci_tbl
 *
 * Returns 0 on success, negative on failure
 *
 * txgbe_probe initializes an adapter identified by a pci_dev structure.
 * The OS initialization, configuring of the adapter private structure,
 * and a hardware reset occur.
 **/
static int txgbe_probe(struct pci_dev *pdev,
				const struct pci_device_id __always_unused *ent)
{
	struct net_device *netdev;
	struct txgbe_adapter *adapter = NULL;
	struct txgbe_hw *hw = NULL;
	static int cards_found;
	int err, pci_using_dac, expected_gts;
	u16 offset = 0;
	u16 eeprom_verh = 0, eeprom_verl = 0;
	u16 eeprom_cfg_blkh = 0, eeprom_cfg_blkl = 0;
	u32 etrack_id = 0;
	u16 build = 0, major = 0, patch = 0;
	char *info_string, *i_s_var;
	u8 part_str[TXGBE_PBANUM_LENGTH];
	unsigned int indices = MAX_TX_QUEUES;

	bool disable_dev = false;
/* #ifndef NETIF_F_GSO_PARTIA */
	netdev_features_t hw_features;

	err = pci_enable_device_mem(pdev);
	if (err)
		return err;

	if (!dma_set_mask(pci_dev_to_dev(pdev), DMA_BIT_MASK(64)) &&
		!dma_set_coherent_mask(pci_dev_to_dev(pdev), DMA_BIT_MASK(64))) {
		pci_using_dac = 1;
	} else {
		err = dma_set_mask(pci_dev_to_dev(pdev), DMA_BIT_MASK(32));
		if (err) {
			err = dma_set_coherent_mask(pci_dev_to_dev(pdev),
						    DMA_BIT_MASK(32));
			if (err) {
				dev_err(pci_dev_to_dev(pdev), "No usable DMA "
					"configuration, aborting\n");
				goto err_dma;
			}
		}
		pci_using_dac = 0;
	}

	err = pci_request_selected_regions(pdev, pci_select_bars(pdev,
					   IORESOURCE_MEM), txgbe_driver_name);
	if (err) {
		dev_err(pci_dev_to_dev(pdev),
			"pci_request_selected_regions failed 0x%x\n", err);
		goto err_pci_reg;
	}

	hw = vmalloc(sizeof(struct txgbe_hw));
	if (!hw) {
		pr_info("Unable to allocate memory for early mac check\n");
	} else {
		hw->vendor_id = pdev->vendor;
		hw->device_id = pdev->device;
		vfree(hw);
	}

	pci_enable_pcie_error_reporting(pdev);
	pci_set_master(pdev);
	/* errata 16 */
	pcie_capability_clear_and_set_word(pdev, PCI_EXP_DEVCTL,
										PCI_EXP_DEVCTL_READRQ,
										0x1000);

	netdev = alloc_etherdev_mq(sizeof(struct txgbe_adapter), indices);
	if (!netdev) {
		err = -ENOMEM;
		goto err_alloc_etherdev;
	}

	SET_NETDEV_DEV(netdev, pci_dev_to_dev(pdev));

	adapter = netdev_priv(netdev);
	adapter->netdev = netdev;
	adapter->pdev = pdev;
	hw = &adapter->hw;
	hw->back = adapter;
	adapter->msg_enable = (1 << DEFAULT_DEBUG_LEVEL_SHIFT) - 1;

	hw->hw_addr = ioremap(pci_resource_start(pdev, 0),
			      pci_resource_len(pdev, 0));
	adapter->io_addr = hw->hw_addr;
	if (!hw->hw_addr) {
		err = -EIO;
		goto err_ioremap;
	}

	txgbe_assign_netdev_ops(netdev);
	strncpy(netdev->name, pci_name(pdev), sizeof(netdev->name) - 1);
	adapter->bd_number = cards_found;

	/* setup the private structure */
	err = txgbe_sw_init(adapter);
	if (err)
		goto err_sw_init;

	/*
	 * check_options must be called before setup_link to set up
	 * hw->fc completely
	 */
	txgbe_check_options(adapter);
	txgbe_bp_mode_setting(adapter);
	TCALL(hw, mac.ops.set_lan_id);

	/* check if flash load is done after hw power up */
	err = txgbe_check_flash_load(hw, TXGBE_SPI_ILDR_STATUS_PERST);
	if (err)
		goto err_sw_init;
	err = txgbe_check_flash_load(hw, TXGBE_SPI_ILDR_STATUS_PWRRST);
	if (err)
		goto err_sw_init;

	/* reset_hw fills in the perm_addr as well */
	hw->phy.reset_if_overtemp = true;
	err = TCALL(hw, mac.ops.reset_hw);
	hw->phy.reset_if_overtemp = false;
	if (err == TXGBE_ERR_SFP_NOT_PRESENT) {
		err = 0;
	} else if (err == TXGBE_ERR_SFP_NOT_SUPPORTED) {
		e_dev_err("failed to load because an unsupported SFP+ "
			  "module type was detected.\n");
		e_dev_err("Reload the driver after installing a supported "
			  "module.\n");
		goto err_sw_init;
	} else if (err) {
		e_dev_err("HW Init failed: %d\n", err);
		goto err_sw_init;
	}

	netdev->features |= NETIF_F_SG |
			    NETIF_F_IP_CSUM;

#ifdef NETIF_F_IPV6_CSUM
	netdev->features |= NETIF_F_IPV6_CSUM;
#endif

	netdev->features |= NETIF_F_HW_VLAN_CTAG_TX |
			    NETIF_F_HW_VLAN_CTAG_RX;

	netdev->features |= txgbe_tso_features();

	if (adapter->flags2 & TXGBE_FLAG2_RSS_ENABLED)
		netdev->features |= NETIF_F_RXHASH;

	netdev->features |= NETIF_F_RXCSUM;

	/* copy netdev features into list of user selectable features */
	hw_features = netdev->hw_features;
	hw_features |= netdev->features;

	/* give us the option of enabling RSC/LRO later */
	if (adapter->flags2 & TXGBE_FLAG2_RSC_CAPABLE)
		hw_features |= NETIF_F_LRO;

	/* set this bit last since it cannot be part of hw_features */
	netdev->features |= NETIF_F_HW_VLAN_CTAG_FILTER;

	netdev->features |= NETIF_F_NTUPLE;

	adapter->flags |= TXGBE_FLAG_FDIR_PERFECT_CAPABLE;
	hw_features |= NETIF_F_NTUPLE;
	netdev->hw_features = hw_features;

	netdev->vlan_features |= NETIF_F_SG |
				 NETIF_F_IP_CSUM |
				 NETIF_F_IPV6_CSUM |
				 NETIF_F_TSO |
				 NETIF_F_TSO6;

	netdev->hw_enc_features |= NETIF_F_SG | NETIF_F_IP_CSUM |
							   TXGBE_GSO_PARTIAL_FEATURES | NETIF_F_TSO;
	if (netdev->features & NETIF_F_LRO) {
		if ((adapter->flags2 & TXGBE_FLAG2_RSC_CAPABLE) &&
		    ((adapter->rx_itr_setting == 1) ||
		     (adapter->rx_itr_setting > TXGBE_MIN_RSC_ITR))) {
			adapter->flags2 |= TXGBE_FLAG2_RSC_ENABLED;
		} else if (adapter->flags2 & TXGBE_FLAG2_RSC_CAPABLE) {
			e_dev_info("InterruptThrottleRate set too high, "
				   "disabling RSC\n");
		}
	}

	netdev->priv_flags |= IFF_UNICAST_FLT;
	netdev->priv_flags |= IFF_SUPP_NOFCS;

	netdev->min_mtu = ETH_MIN_MTU;
	netdev->max_mtu = TXGBE_MAX_JUMBO_FRAME_SIZE - (ETH_HLEN + ETH_FCS_LEN);

	if (pci_using_dac) {
		netdev->features |= NETIF_F_HIGHDMA;
		netdev->vlan_features |= NETIF_F_HIGHDMA;
	}

	/* make sure the EEPROM is good */
	if (TCALL(hw, eeprom.ops.validate_checksum, NULL)) {
		e_dev_err("The EEPROM Checksum Is Not Valid\n");
		err = -EIO;
		goto err_sw_init;
	}

	memcpy(netdev->dev_addr, hw->mac.perm_addr, netdev->addr_len);

	if (!is_valid_ether_addr(netdev->dev_addr)) {
		e_dev_err("invalid MAC address\n");
		err = -EIO;
		goto err_sw_init;
	}

	txgbe_mac_set_default_filter(adapter, hw->mac.perm_addr);

	timer_setup(&adapter->service_timer, txgbe_service_timer, 0);

	if (TXGBE_REMOVED(hw->hw_addr)) {
		err = -EIO;
		goto err_sw_init;
	}
	INIT_WORK(&adapter->service_task, txgbe_service_task);
	set_bit(__TXGBE_SERVICE_INITED, &adapter->state);
	clear_bit(__TXGBE_SERVICE_SCHED, &adapter->state);

	err = txgbe_init_interrupt_scheme(adapter);
	if (err)
		goto err_sw_init;

	/* WOL not supported for all devices */
	adapter->wol = 0;
	TCALL(hw, eeprom.ops.read,
		hw->eeprom.sw_region_offset + TXGBE_DEVICE_CAPS,
		&adapter->eeprom_cap);

	if ((hw->subsystem_device_id & TXGBE_WOL_MASK) == TXGBE_WOL_SUP &&
		hw->bus.lan_id == 0) {
		adapter->wol = TXGBE_PSR_WKUP_CTL_MAG;
		wr32(hw, TXGBE_PSR_WKUP_CTL, adapter->wol);
	}
	hw->wol_enabled = !!(adapter->wol);

	device_set_wakeup_enable(pci_dev_to_dev(adapter->pdev), adapter->wol);

	/*
	 * Save off EEPROM version number and Option Rom version which
	 * together make a unique identify for the eeprom
	 */
	TCALL(hw, eeprom.ops.read,
		hw->eeprom.sw_region_offset + TXGBE_EEPROM_VERSION_H,
		&eeprom_verh);
	TCALL(hw, eeprom.ops.read,
		hw->eeprom.sw_region_offset + TXGBE_EEPROM_VERSION_L,
		&eeprom_verl);
	etrack_id = (eeprom_verh << 16) | eeprom_verl;

	TCALL(hw, eeprom.ops.read,
		hw->eeprom.sw_region_offset + TXGBE_ISCSI_BOOT_CONFIG, &offset);

	/* Make sure offset to SCSI block is valid */
	if (!(offset == 0x0) && !(offset == 0xffff)) {
		TCALL(hw, eeprom.ops.read, offset + 0x84, &eeprom_cfg_blkh);
		TCALL(hw, eeprom.ops.read, offset + 0x83, &eeprom_cfg_blkl);

		/* Only display Option Rom if exist */
		if (eeprom_cfg_blkl && eeprom_cfg_blkh) {
			major = eeprom_cfg_blkl >> 8;
			build = (eeprom_cfg_blkl << 8) | (eeprom_cfg_blkh >> 8);
			patch = eeprom_cfg_blkh & 0x00ff;

			snprintf(adapter->eeprom_id, sizeof(adapter->eeprom_id),
				 "0x%08x, %d.%d.%d", etrack_id, major, build,
				 patch);
		} else {
			snprintf(adapter->eeprom_id, sizeof(adapter->eeprom_id),
				 "0x%08x", etrack_id);
		}
	} else {
		snprintf(adapter->eeprom_id, sizeof(adapter->eeprom_id),
			 "0x%08x", etrack_id);
	}

	/* reset the hardware with the new settings */
	err = TCALL(hw, mac.ops.start_hw);
	if (err == TXGBE_ERR_EEPROM_VERSION) {
		/* We are running on a pre-production device, log a warning */
		e_dev_warn("This device is a pre-production adapter/LOM. "
			   "Please be aware there may be issues associated "
			   "with your hardware.  If you are experiencing "
			   "problems please contact your hardware "
			   "representative who provided you with this "
			   "hardware.\n");
	} else if (err) {
		e_dev_err("HW init failed\n");
		goto err_register;
	}

	/* pick up the PCI bus settings for reporting later */
	TCALL(hw, mac.ops.get_bus_info);

	strcpy(netdev->name, "eth%d");
	err = register_netdev(netdev);
	if (err)
		goto err_register;

	pci_set_drvdata(pdev, adapter);
	adapter->netdev_registered = true;

	if (!((hw->subsystem_device_id & TXGBE_NCSI_MASK) == TXGBE_NCSI_SUP))
		/* power down the optics for SFP+ fiber */
		TCALL(hw, mac.ops.disable_tx_laser);

	/* carrier off reporting is important to ethtool even BEFORE open */
	netif_carrier_off(netdev);
	/* keep stopping all the transmit queues for older kernels */
	netif_tx_stop_all_queues(netdev);

	/* print all messages at the end so that we use our eth%d name */

	/* calculate the expected PCIe bandwidth required for optimal
	 * performance. Note that some older parts will never have enough
	 * bandwidth due to being older generation PCIe parts. We clamp these
	 * parts to ensure that no warning is displayed, as this could confuse
	 * users otherwise. */

	expected_gts = txgbe_enumerate_functions(adapter) * 10;

	/* don't check link if we failed to enumerate functions */
	if (expected_gts > 0)
		txgbe_check_minimum_link(adapter, expected_gts);

	if ((hw->subsystem_device_id & TXGBE_NCSI_MASK) == TXGBE_NCSI_SUP)
		e_info(probe, "NCSI : support");
	else
		e_info(probe, "NCSI : unsupported");

	/* First try to read PBA as a string */
	err = txgbe_read_pba_string(hw, part_str, TXGBE_PBANUM_LENGTH);
	if (err)

		strncpy(part_str, "Unknown", TXGBE_PBANUM_LENGTH);
	if (txgbe_is_sfp(hw) && hw->phy.sfp_type != txgbe_sfp_type_not_present)
		e_info(probe, "PHY: %d, SFP+: %d, PBA No: %s\n",
		       hw->phy.type, hw->phy.sfp_type, part_str);
	else
		e_info(probe, "PHY: %d, PBA No: %s\n",
		       hw->phy.type, part_str);

	e_dev_info("%02x:%02x:%02x:%02x:%02x:%02x\n",
		   netdev->dev_addr[0], netdev->dev_addr[1],
		   netdev->dev_addr[2], netdev->dev_addr[3],
		   netdev->dev_addr[4], netdev->dev_addr[5]);

#define INFO_STRING_LEN 255
	info_string = kzalloc(INFO_STRING_LEN, GFP_KERNEL);
	if (!info_string) {
		e_err(probe, "allocation for info string failed\n");
		goto no_info_string;
	}
	i_s_var = info_string;
	i_s_var += sprintf(info_string, "Enabled Features: ");
	i_s_var += sprintf(i_s_var, "RxQ: %d TxQ: %d ",
			   adapter->num_rx_queues, adapter->num_tx_queues);
	if (adapter->flags & TXGBE_FLAG_FDIR_HASH_CAPABLE)
		i_s_var += sprintf(i_s_var, "FdirHash ");
	if (adapter->flags & TXGBE_FLAG_DCB_ENABLED)
		i_s_var += sprintf(i_s_var, "DCB ");
	if (adapter->flags & TXGBE_FLAG_TPH_ENABLED)
		i_s_var += sprintf(i_s_var, "TPH ");
	if (adapter->flags2 & TXGBE_FLAG2_RSC_ENABLED)
		i_s_var += sprintf(i_s_var, "RSC ");
	if (adapter->flags & TXGBE_FLAG_VXLAN_OFFLOAD_ENABLE)
		i_s_var += sprintf(i_s_var, "vxlan_rx ");

	BUG_ON(i_s_var > (info_string + INFO_STRING_LEN));
	/* end features printing */
	e_info(probe, "%s\n", info_string);
	kfree(info_string);
no_info_string:
	/* firmware requires blank driver version */
	TCALL(hw, mac.ops.set_fw_drv_ver, 0xFF, 0xFF, 0xFF, 0xFF);

	/* add san mac addr to netdev */
	txgbe_add_sanmac_netdev(netdev);

	e_info(probe, "WangXun(R) 10 Gigabit Network Connection\n");
	cards_found++;

	/* setup link for SFP devices with MNG FW, else wait for TXGBE_UP */
	if (txgbe_mng_present(hw) && txgbe_is_sfp(hw))
		TCALL(hw, mac.ops.setup_link,
			TXGBE_LINK_SPEED_10GB_FULL | TXGBE_LINK_SPEED_1GB_FULL,
			true);

	TCALL(hw, mac.ops.setup_eee,
		(adapter->flags2 & TXGBE_FLAG2_EEE_CAPABLE) &&
		(adapter->flags2 & TXGBE_FLAG2_EEE_ENABLED));

	return 0;

err_register:
	txgbe_clear_interrupt_scheme(adapter);
	txgbe_release_hw_control(adapter);
err_sw_init:
	adapter->flags2 &= ~TXGBE_FLAG2_SEARCH_FOR_SFP;
	kfree(adapter->mac_table);
	iounmap(adapter->io_addr);
err_ioremap:
	disable_dev = !test_and_set_bit(__TXGBE_DISABLED, &adapter->state);
	free_netdev(netdev);
err_alloc_etherdev:
	pci_release_selected_regions(pdev,
				     pci_select_bars(pdev, IORESOURCE_MEM));
err_pci_reg:
err_dma:
	if (!adapter || disable_dev)
		pci_disable_device(pdev);
	return err;
}

/**
 * txgbe_remove - Device Removal Routine
 * @pdev: PCI device information struct
 *
 * txgbe_remove is called by the PCI subsystem to alert the driver
 * that it should release a PCI device.  The could be caused by a
 * Hot-Plug event, or because the driver is going to be removed from
 * memory.
 **/
static void txgbe_remove(struct pci_dev *pdev)
{
	struct txgbe_adapter *adapter = pci_get_drvdata(pdev);
	struct net_device *netdev;
	bool disable_dev;

	/* if !adapter then we already cleaned up in probe */
	if (!adapter)
		return;

	netdev = adapter->netdev;
	set_bit(__TXGBE_REMOVING, &adapter->state);
	cancel_work_sync(&adapter->service_task);

	/* remove the added san mac */
	txgbe_del_sanmac_netdev(netdev);

	if (adapter->netdev_registered) {
		unregister_netdev(netdev);
		adapter->netdev_registered = false;
	}

	txgbe_clear_interrupt_scheme(adapter);
	txgbe_release_hw_control(adapter);

	iounmap(adapter->io_addr);
	pci_release_selected_regions(pdev,
				     pci_select_bars(pdev, IORESOURCE_MEM));

	kfree(adapter->mac_table);
	disable_dev = !test_and_set_bit(__TXGBE_DISABLED, &adapter->state);
	free_netdev(netdev);

	pci_disable_pcie_error_reporting(pdev);

	if (disable_dev)
		pci_disable_device(pdev);
}

static bool txgbe_check_cfg_remove(struct txgbe_hw *hw, struct pci_dev *pdev)
{
	u16 value;

	pci_read_config_word(pdev, PCI_VENDOR_ID, &value);
	if (value == TXGBE_FAILED_READ_CFG_WORD) {
		txgbe_remove_adapter(hw);
		return true;
	}
	return false;
}

u16 txgbe_read_pci_cfg_word(struct txgbe_hw *hw, u32 reg)
{
	struct txgbe_adapter *adapter = hw->back;
	u16 value;

	if (TXGBE_REMOVED(hw->hw_addr))
		return TXGBE_FAILED_READ_CFG_WORD;
	pci_read_config_word(adapter->pdev, reg, &value);
	if (value == TXGBE_FAILED_READ_CFG_WORD &&
		txgbe_check_cfg_remove(hw, adapter->pdev))
		return TXGBE_FAILED_READ_CFG_WORD;
	return value;
}

void txgbe_write_pci_cfg_word(struct txgbe_hw *hw, u32 reg, u16 value)
{
	struct txgbe_adapter *adapter = hw->back;

	if (TXGBE_REMOVED(hw->hw_addr))
		return;
	pci_write_config_word(adapter->pdev, reg, value);
}

static struct pci_driver txgbe_driver = {
	.name     = txgbe_driver_name,
	.id_table = txgbe_pci_tbl,
	.probe    = txgbe_probe,
	.remove   = txgbe_remove,
#ifdef CONFIG_PM
	.suspend  = txgbe_suspend,
	.resume   = txgbe_resume,
#endif
	.shutdown = txgbe_shutdown,
};

/**
 * txgbe_init_module - Driver Registration Routine
 *
 * txgbe_init_module is the first routine called when the driver is
 * loaded. All it does is register with the PCI subsystem.
 **/
static int __init txgbe_init_module(void)
{
	int ret;
	pr_info("%s - version %s\n", txgbe_driver_string, txgbe_driver_version);
	pr_info("%s\n", txgbe_copyright);

	txgbe_wq = create_singlethread_workqueue(txgbe_driver_name);
	if (!txgbe_wq) {
		pr_err("%s: Failed to create workqueue\n", txgbe_driver_name);
		return -ENOMEM;
	}

	ret = pci_register_driver(&txgbe_driver);
	return ret;
}

module_init(txgbe_init_module);

/**
 * txgbe_exit_module - Driver Exit Cleanup Routine
 *
 * txgbe_exit_module is called just before the driver is removed
 * from memory.
 **/
static void __exit txgbe_exit_module(void)
{
	pci_unregister_driver(&txgbe_driver);
	if (txgbe_wq) {
		destroy_workqueue(txgbe_wq);
	}
}

module_exit(txgbe_exit_module);

/* txgbe_main.c */
