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
 * based on ixgbe_lib.c, Copyright(c) 1999 - 2017 Intel Corporation.
 * Contact Information:
 * Linux NICS <linux.nics@intel.com>
 * e1000-devel Mailing List <e1000-devel@lists.sourceforge.net>
 * Intel Corporation, 5200 N.E. Elam Young Parkway, Hillsboro, OR 97124-6497
 */


#include "txgbe.h"

/**
 * txgbe_cache_ring_dcb_vmdq - Descriptor ring to register mapping for VMDq
 * @adapter: board private structure to initialize
 *
 * Cache the descriptor ring offsets for VMDq to the assigned rings.  It
 * will also try to cache the proper offsets if RSS/FCoE are enabled along
 * with VMDq.
 *
 **/
static bool txgbe_cache_ring_dcb_vmdq(struct txgbe_adapter *adapter)
{
	struct txgbe_ring_feature *vmdq = &adapter->ring_feature[RING_F_VMDQ];
	int i;
	u16 reg_idx;
	u8 tcs = netdev_get_num_tc(adapter->netdev);

	/* verify we have DCB enabled before proceeding */
	if (tcs <= 1)
		return false;

	/* verify we have VMDq enabled before proceeding */
	if (!(adapter->flags & TXGBE_FLAG_VMDQ_ENABLED))
		return false;

	/* start at VMDq register offset for SR-IOV enabled setups */
	reg_idx = vmdq->offset * __ALIGN_MASK(1, ~vmdq->mask);
	for (i = 0; i < adapter->num_rx_queues; i++, reg_idx++) {
		/* If we are greater than indices move to next pool */
		if ((reg_idx & ~vmdq->mask) >= tcs)
			reg_idx = __ALIGN_MASK(reg_idx, ~vmdq->mask);
		adapter->rx_ring[i]->reg_idx = reg_idx;
	}

	reg_idx = vmdq->offset * __ALIGN_MASK(1, ~vmdq->mask);
	for (i = 0; i < adapter->num_tx_queues; i++, reg_idx++) {
		/* If we are greater than indices move to next pool */
		if ((reg_idx & ~vmdq->mask) >= tcs)
			reg_idx = __ALIGN_MASK(reg_idx, ~vmdq->mask);
		adapter->tx_ring[i]->reg_idx = reg_idx;
	}

	return true;
}

/* txgbe_get_first_reg_idx - Return first register index associated with ring */
static void txgbe_get_first_reg_idx(struct txgbe_adapter *adapter, u8 tc,
				    u16 *tx, u16 *rx)
{
	struct net_device *dev = adapter->netdev;
	u8 num_tcs = netdev_get_num_tc(dev);

	*tx = 0;
	*rx = 0;


	if (num_tcs > 4) {
		/*
		 * TCs    : TC0/1 TC2/3 TC4-7
		 * TxQs/TC:    32    16     8
		 * RxQs/TC:    16    16    16
		 */
		*rx = tc << 4;
		if (tc < 3)
			*tx = tc << 5;          /*   0,  32,  64 */
		else if (tc < 5)
			*tx = (tc + 2) << 4;    /*  80,  96 */
		else
			*tx = (tc + 8) << 3;    /* 104, 112, 120 */
	} else {
		/*
		 * TCs    : TC0 TC1 TC2/3
		 * TxQs/TC:  64  32    16
		 * RxQs/TC:  32  32    32
		 */
		*rx = tc << 5;
		if (tc < 2)
			*tx = tc << 6;          /*  0,  64 */
		else
			*tx = (tc + 4) << 4;    /* 96, 112 */
	}

}

/**
 * txgbe_cache_ring_dcb - Descriptor ring to register mapping for DCB
 * @adapter: board private structure to initialize
 *
 * Cache the descriptor ring offsets for DCB to the assigned rings.
 *
 **/
static bool txgbe_cache_ring_dcb(struct txgbe_adapter *adapter)
{
	int tc, offset, rss_i, i;
	u16 tx_idx, rx_idx;
	struct net_device *dev = adapter->netdev;
	u8 num_tcs = netdev_get_num_tc(dev);

	if (num_tcs <= 1)
		return false;

	rss_i = adapter->ring_feature[RING_F_RSS].indices;

	for (tc = 0, offset = 0; tc < num_tcs; tc++, offset += rss_i) {
		txgbe_get_first_reg_idx(adapter, (u8)tc, &tx_idx, &rx_idx);
		for (i = 0; i < rss_i; i++, tx_idx++, rx_idx++) {
			adapter->tx_ring[offset + i]->reg_idx = tx_idx;
			adapter->rx_ring[offset + i]->reg_idx = rx_idx;
			adapter->tx_ring[offset + i]->dcb_tc = (u8)tc;
			adapter->rx_ring[offset + i]->dcb_tc = (u8)tc;
		}
	}

	return true;
}

/**
 * txgbe_cache_ring_vmdq - Descriptor ring to register mapping for VMDq
 * @adapter: board private structure to initialize
 *
 * Cache the descriptor ring offsets for VMDq to the assigned rings.  It
 * will also try to cache the proper offsets if RSS/FCoE/SRIOV are enabled along
 * with VMDq.
 *
 **/
static bool txgbe_cache_ring_vmdq(struct txgbe_adapter *adapter)
{
	struct txgbe_ring_feature *vmdq = &adapter->ring_feature[RING_F_VMDQ];
	struct txgbe_ring_feature *rss = &adapter->ring_feature[RING_F_RSS];
	int i;
	u16 reg_idx;

	/* only proceed if VMDq is enabled */
	if (!(adapter->flags & TXGBE_FLAG_VMDQ_ENABLED))
		return false;

	/* start at VMDq register offset for SR-IOV enabled setups */
	reg_idx = vmdq->offset * __ALIGN_MASK(1, ~vmdq->mask);
	for (i = 0; i < adapter->num_rx_queues; i++, reg_idx++) {
		/* If we are greater than indices move to next pool */
		if ((reg_idx & ~vmdq->mask) >= rss->indices)
			reg_idx = __ALIGN_MASK(reg_idx, ~vmdq->mask);
		adapter->rx_ring[i]->reg_idx = reg_idx;
	}

	reg_idx = vmdq->offset * __ALIGN_MASK(1, ~vmdq->mask);
	for (i = 0; i < adapter->num_tx_queues; i++, reg_idx++) {
		/* If we are greater than indices move to next pool */
		if ((reg_idx & rss->mask) >= rss->indices)
			reg_idx = __ALIGN_MASK(reg_idx, ~vmdq->mask);
		adapter->tx_ring[i]->reg_idx = reg_idx;
	}

	return true;
}

/**
 * txgbe_cache_ring_rss - Descriptor ring to register mapping for RSS
 * @adapter: board private structure to initialize
 *
 * Cache the descriptor ring offsets for RSS, ATR, FCoE, and SR-IOV.
 *
 **/
static bool txgbe_cache_ring_rss(struct txgbe_adapter *adapter)
{
	u16 i;

	for (i = 0; i < adapter->num_rx_queues; i++)
		adapter->rx_ring[i]->reg_idx = i;

	for (i = 0; i < adapter->num_tx_queues; i++)
		adapter->tx_ring[i]->reg_idx = i;

	return true;
}

/**
 * txgbe_cache_ring_register - Descriptor ring to register mapping
 * @adapter: board private structure to initialize
 *
 * Once we know the feature-set enabled for the device, we'll cache
 * the register offset the descriptor ring is assigned to.
 *
 * Note, the order the various feature calls is important.  It must start with
 * the "most" features enabled at the same time, then trickle down to the
 * least amount of features turned on at once.
 **/
static void txgbe_cache_ring_register(struct txgbe_adapter *adapter)
{
	if (txgbe_cache_ring_dcb_vmdq(adapter))
		return;

	if (txgbe_cache_ring_dcb(adapter))
		return;

	if (txgbe_cache_ring_vmdq(adapter))
		return;

	txgbe_cache_ring_rss(adapter);
}

#define TXGBE_RSS_64Q_MASK      0x3F
#define TXGBE_RSS_16Q_MASK      0xF
#define TXGBE_RSS_8Q_MASK       0x7
#define TXGBE_RSS_4Q_MASK       0x3
#define TXGBE_RSS_2Q_MASK       0x1
#define TXGBE_RSS_DISABLED_MASK 0x0

/**
 * txgbe_set_dcb_vmdq_queues: Allocate queues for VMDq devices w/ DCB
 * @adapter: board private structure to initialize
 *
 * When VMDq (Virtual Machine Devices queue) is enabled, allocate queues
 * and VM pools where appropriate.  Also assign queues based on DCB
 * priorities and map accordingly..
 *
 **/
static bool txgbe_set_dcb_vmdq_queues(struct txgbe_adapter *adapter)
{
	u16 i;
	u16 vmdq_i = adapter->ring_feature[RING_F_VMDQ].limit;
	u16 vmdq_m = 0;
	u8 tcs = netdev_get_num_tc(adapter->netdev);

	/* verify we have DCB enabled before proceeding */
	if (tcs <= 1)
		return false;

	/* verify we have VMDq enabled before proceeding */
	if (!(adapter->flags & TXGBE_FLAG_VMDQ_ENABLED))
		return false;

	/* Add starting offset to total pool count */
	vmdq_i += adapter->ring_feature[RING_F_VMDQ].offset;

	/* 16 pools w/ 8 TC per pool */
	if (tcs > 4) {
		vmdq_i = min_t(u16, vmdq_i, 16);
		vmdq_m = TXGBE_VMDQ_8Q_MASK;
	/* 32 pools w/ 4 TC per pool */
	} else {
		vmdq_i = min_t(u16, vmdq_i, 32);
		vmdq_m = TXGBE_VMDQ_4Q_MASK;
	}

	/* remove the starting offset from the pool count */
	vmdq_i -= adapter->ring_feature[RING_F_VMDQ].offset;

	/* save features for later use */
	adapter->ring_feature[RING_F_VMDQ].indices = vmdq_i;
	adapter->ring_feature[RING_F_VMDQ].mask = vmdq_m;

	/*
	 * We do not support DCB, VMDq, and RSS all simultaneously
	 * so we will disable RSS since it is the lowest priority
	 */
	adapter->ring_feature[RING_F_RSS].indices = 1;
	adapter->ring_feature[RING_F_RSS].mask = TXGBE_RSS_DISABLED_MASK;

	adapter->queues_per_pool = tcs;

	adapter->num_tx_queues = vmdq_i * tcs;
	adapter->num_rx_queues = vmdq_i * tcs;

	/* disable ATR as it is not supported when VMDq is enabled */
	adapter->flags &= ~TXGBE_FLAG_FDIR_HASH_CAPABLE;

	/* configure TC to queue mapping */
	for (i = 0; i < tcs; i++)
		netdev_set_tc_queue(adapter->netdev, (u8)i, 1, i);

	return true;
}

/**
 * txgbe_set_dcb_queues: Allocate queues for a DCB-enabled device
 * @adapter: board private structure to initialize
 *
 * When DCB (Data Center Bridging) is enabled, allocate queues for
 * each traffic class.  If multiqueue isn't available,then abort DCB
 * initialization.
 *
 * This function handles all combinations of DCB, RSS, and FCoE.
 *
 **/
static bool txgbe_set_dcb_queues(struct txgbe_adapter *adapter)
{
	struct net_device *dev = adapter->netdev;
	struct txgbe_ring_feature *f;
	u16 rss_i, rss_m, i;
	u16 tcs;

	/* Map queue offset and counts onto allocated tx queues */
	tcs = netdev_get_num_tc(dev);

	if (tcs <= 1)
		return false;

	/* determine the upper limit for our current DCB mode */
	rss_i = dev->num_tx_queues / tcs;

	if (tcs > 4) {
		/* 8 TC w/ 8 queues per TC */
		rss_i = min_t(u16, rss_i, 8);
		rss_m = TXGBE_RSS_8Q_MASK;
	} else {
		/* 4 TC w/ 16 queues per TC */
		rss_i = min_t(u16, rss_i, 16);
		rss_m = TXGBE_RSS_16Q_MASK;
	}

	/* set RSS mask and indices */
	f = &adapter->ring_feature[RING_F_RSS];
	rss_i = min_t(u16, rss_i, f->limit);
	f->indices = rss_i;
	f->mask = rss_m;

	/* disable ATR as it is not supported when DCB is enabled */
	adapter->flags &= ~TXGBE_FLAG_FDIR_HASH_CAPABLE;

	for (i = 0; i < tcs; i++)
		netdev_set_tc_queue(dev, (u8)i, rss_i, rss_i * i);

	adapter->num_tx_queues = rss_i * tcs;
	adapter->num_rx_queues = rss_i * tcs;

	return true;
}

/**
 * txgbe_set_vmdq_queues: Allocate queues for VMDq devices
 * @adapter: board private structure to initialize
 *
 * When VMDq (Virtual Machine Devices queue) is enabled, allocate queues
 * and VM pools where appropriate.  If RSS is available, then also try and
 * enable RSS and map accordingly.
 *
 **/
static bool txgbe_set_vmdq_queues(struct txgbe_adapter *adapter)
{
	u16 vmdq_i = adapter->ring_feature[RING_F_VMDQ].limit;
	u16 vmdq_m = 0;
	u16 rss_i = adapter->ring_feature[RING_F_RSS].limit;
	u16 rss_m = TXGBE_RSS_DISABLED_MASK;

	/* only proceed if VMDq is enabled */
	if (!(adapter->flags & TXGBE_FLAG_VMDQ_ENABLED))
		return false;
	/* Add starting offset to total pool count */
	vmdq_i += adapter->ring_feature[RING_F_VMDQ].offset;

	/* double check we are limited to maximum pools */
	vmdq_i = min_t(u16, TXGBE_MAX_VMDQ_INDICES, vmdq_i);

	/* 64 pool mode with 2 queues per pool, or
	 * 16/32/64 pool mode with 1 queue per pool */
	if ((vmdq_i > 32) || (rss_i < 4)) {
		vmdq_m = TXGBE_VMDQ_2Q_MASK;
		rss_m = TXGBE_RSS_2Q_MASK;
		rss_i = min_t(u16, rss_i, 2);
	/* 32 pool mode with 4 queues per pool */
	} else {
		vmdq_m = TXGBE_VMDQ_4Q_MASK;
		rss_m = TXGBE_RSS_4Q_MASK;
		rss_i = 4;
	}

	/* remove the starting offset from the pool count */
	vmdq_i -= adapter->ring_feature[RING_F_VMDQ].offset;

	/* save features for later use */
	adapter->ring_feature[RING_F_VMDQ].indices = vmdq_i;
	adapter->ring_feature[RING_F_VMDQ].mask = vmdq_m;

	/* limit RSS based on user input and save for later use */
	adapter->ring_feature[RING_F_RSS].indices = rss_i;
	adapter->ring_feature[RING_F_RSS].mask = rss_m;

	adapter->queues_per_pool = rss_i;

	adapter->num_rx_queues = vmdq_i * rss_i;
	adapter->num_tx_queues = vmdq_i * rss_i;

	/* disable ATR as it is not supported when VMDq is enabled */
	adapter->flags &= ~TXGBE_FLAG_FDIR_HASH_CAPABLE;

	return true;
}

/**
 * txgbe_set_rss_queues: Allocate queues for RSS
 * @adapter: board private structure to initialize
 *
 * This is our "base" multiqueue mode.  RSS (Receive Side Scaling) will try
 * to allocate one Rx queue per CPU, and if available, one Tx queue per CPU.
 *
 **/
static bool txgbe_set_rss_queues(struct txgbe_adapter *adapter)
{
	struct txgbe_ring_feature *f;
	u16 rss_i;

	/* set mask for 16 queue limit of RSS */
	f = &adapter->ring_feature[RING_F_RSS];
	rss_i = f->limit;

	f->indices = rss_i;
	f->mask = TXGBE_RSS_64Q_MASK;

	/* disable ATR by default, it will be configured below */
	adapter->flags &= ~TXGBE_FLAG_FDIR_HASH_CAPABLE;

	/*
	 * Use Flow Director in addition to RSS to ensure the best
	 * distribution of flows across cores, even when an FDIR flow
	 * isn't matched.
	 */
	if (rss_i > 1 && adapter->atr_sample_rate) {
		f = &adapter->ring_feature[RING_F_FDIR];

		rss_i = f->indices = f->limit;

		if (!(adapter->flags & TXGBE_FLAG_FDIR_PERFECT_CAPABLE))
			adapter->flags |= TXGBE_FLAG_FDIR_HASH_CAPABLE;
	}

	adapter->num_rx_queues = rss_i;
	adapter->num_tx_queues = rss_i;

	return true;
}

/*
 * txgbe_set_num_queues: Allocate queues for device, feature dependent
 * @adapter: board private structure to initialize
 *
 * This is the top level queue allocation routine.  The order here is very
 * important, starting with the "most" number of features turned on at once,
 * and ending with the smallest set of features.  This way large combinations
 * can be allocated if they're turned on, and smaller combinations are the
 * fallthrough conditions.
 *
 **/
static void txgbe_set_num_queues(struct txgbe_adapter *adapter)
{
	/* Start with base case */
	adapter->num_rx_queues = 1;
	adapter->num_tx_queues = 1;
	adapter->queues_per_pool = 1;

	if (txgbe_set_dcb_vmdq_queues(adapter))
		return;

	if (txgbe_set_dcb_queues(adapter))
		return;

	if (txgbe_set_vmdq_queues(adapter))
		return;

	txgbe_set_rss_queues(adapter);
}

/**
 * txgbe_acquire_msix_vectors - acquire MSI-X vectors
 * @adapter: board private structure
 *
 * Attempts to acquire a suitable range of MSI-X vector interrupts. Will
 * return a negative error code if unable to acquire MSI-X vectors for any
 * reason.
 */
static int txgbe_acquire_msix_vectors(struct txgbe_adapter *adapter)
{
	struct txgbe_hw *hw = &adapter->hw;
	int i, vectors, vector_threshold;

	if (!(adapter->flags & TXGBE_FLAG_MSIX_CAPABLE))
		return -EOPNOTSUPP;

	/* We start by asking for one vector per queue pair */
	vectors = max(adapter->num_rx_queues, adapter->num_tx_queues);

	/* It is easy to be greedy for MSI-X vectors. However, it really
	 * doesn't do much good if we have a lot more vectors than CPUs. We'll
	 * be somewhat conservative and only ask for (roughly) the same number
	 * of vectors as there are CPUs.
	 */
	vectors = min_t(int, vectors, num_online_cpus());

	/* Some vectors are necessary for non-queue interrupts */
	vectors += NON_Q_VECTORS;

	/* Hardware can only support a maximum of hw.mac->max_msix_vectors.
	 * With features such as RSS and VMDq, we can easily surpass the
	 * number of Rx and Tx descriptor queues supported by our device.
	 * Thus, we cap the maximum in the rare cases where the CPU count also
	 * exceeds our vector limit
	 */
	vectors = min_t(int, vectors, hw->mac.max_msix_vectors);

	/* We want a minimum of two MSI-X vectors for (1) a TxQ[0] + RxQ[0]
	 * handler, and (2) an Other (Link Status Change, etc.) handler.
	 */
	vector_threshold = MIN_MSIX_COUNT;

	adapter->msix_entries = kcalloc(vectors,
					sizeof(struct msix_entry),
					GFP_KERNEL);
	if (!adapter->msix_entries)
		return -ENOMEM;

	for (i = 0; i < vectors; i++)
		adapter->msix_entries[i].entry = i;

	vectors = pci_enable_msix_range(adapter->pdev, adapter->msix_entries,
					vector_threshold, vectors);
	if (vectors < 0) {
		/* A negative count of allocated vectors indicates an error in
		 * acquiring within the specified range of MSI-X vectors */
		e_dev_warn("Failed to allocate MSI-X interrupts. Err: %d\n",
			   vectors);

		adapter->flags &= ~TXGBE_FLAG_MSIX_ENABLED;
		kfree(adapter->msix_entries);
		adapter->msix_entries = NULL;

		return vectors;
	}

	/* we successfully allocated some number of vectors within our
	 * requested range.
	 */
	adapter->flags |= TXGBE_FLAG_MSIX_ENABLED;

	/* Adjust for only the vectors we'll use, which is minimum
	 * of max_q_vectors, or the number of vectors we were allocated.
	 */
	vectors -= NON_Q_VECTORS;
	adapter->num_q_vectors = min_t(int, vectors, adapter->max_q_vectors);

	return 0;
}

static void txgbe_add_ring(struct txgbe_ring *ring,
			   struct txgbe_ring_container *head)
{
	ring->next = head->ring;
	head->ring = ring;
	head->count++;
}

/**
 * txgbe_alloc_q_vector - Allocate memory for a single interrupt vector
 * @adapter: board private structure to initialize
 * @v_count: q_vectors allocated on adapter, used for ring interleaving
 * @v_idx: index of vector in adapter struct
 * @txr_count: total number of Tx rings to allocate
 * @txr_idx: index of first Tx ring to allocate
 * @rxr_count: total number of Rx rings to allocate
 * @rxr_idx: index of first Rx ring to allocate
 *
 * We allocate one q_vector.  If allocation fails we return -ENOMEM.
 **/
static int txgbe_alloc_q_vector(struct txgbe_adapter *adapter,
				unsigned int v_count, unsigned int v_idx,
				unsigned int txr_count, unsigned int txr_idx,
				unsigned int rxr_count, unsigned int rxr_idx)
{
	struct txgbe_q_vector *q_vector;
	struct txgbe_ring *ring;
	int node = -1;
	int cpu = -1;
	u8 tcs = netdev_get_num_tc(adapter->netdev);
	int ring_count, size;

	/* note this will allocate space for the ring structure as well! */
	ring_count = txr_count + rxr_count;
	size = sizeof(struct txgbe_q_vector) +
	       (sizeof(struct txgbe_ring) * ring_count);

	/* customize cpu for Flow Director mapping */
	if ((tcs <= 1) && !(adapter->flags & TXGBE_FLAG_VMDQ_ENABLED)) {
		u16 rss_i = adapter->ring_feature[RING_F_RSS].indices;
		if (rss_i > 1 && adapter->atr_sample_rate) {
			if (cpu_online(v_idx)) {
				cpu = v_idx;
				node = cpu_to_node(cpu);
			}
		}
	}

	/* allocate q_vector and rings */
	q_vector = kzalloc_node(size, GFP_KERNEL, node);
	if (!q_vector)
		q_vector = kzalloc(size, GFP_KERNEL);
	if (!q_vector)
		return -ENOMEM;

	/* setup affinity mask and node */
	if (cpu != -1)
		cpumask_set_cpu(cpu, &q_vector->affinity_mask);
	q_vector->numa_node = node;

	/* initialize CPU for DCA */
	q_vector->cpu = -1;

	/* initialize NAPI */
	netif_napi_add(adapter->netdev, &q_vector->napi,
		       txgbe_poll, 64);

	/* tie q_vector and adapter together */
	adapter->q_vector[v_idx] = q_vector;
	q_vector->adapter = adapter;
	q_vector->v_idx = v_idx;

	/* initialize work limits */
	q_vector->tx.work_limit = adapter->tx_work_limit;
	q_vector->rx.work_limit = adapter->rx_work_limit;

	/* initialize pointer to rings */
	ring = q_vector->ring;

	/* intialize ITR */
	if (txr_count && !rxr_count) {
		/* tx only vector */
		if (adapter->tx_itr_setting == 1)
			q_vector->itr = TXGBE_12K_ITR;
		else
			q_vector->itr = adapter->tx_itr_setting;
	} else {
		/* rx or rx/tx vector */
		if (adapter->rx_itr_setting == 1)
			q_vector->itr = TXGBE_20K_ITR;
		else
			q_vector->itr = adapter->rx_itr_setting;
	}

	while (txr_count) {
		/* assign generic ring traits */
		ring->dev = pci_dev_to_dev(adapter->pdev);
		ring->netdev = adapter->netdev;

		/* configure backlink on ring */
		ring->q_vector = q_vector;

		/* update q_vector Tx values */
		txgbe_add_ring(ring, &q_vector->tx);

		/* apply Tx specific ring traits */
		ring->count = adapter->tx_ring_count;
		if (adapter->num_vmdqs > 1)
			ring->queue_index =
				txr_idx % adapter->queues_per_pool;
		else
			ring->queue_index = txr_idx;

		/* assign ring to adapter */
		adapter->tx_ring[txr_idx] = ring;

		/* update count and index */
		txr_count--;
		txr_idx += v_count;

		/* push pointer to next ring */
		ring++;
	}

	while (rxr_count) {
		/* assign generic ring traits */
		ring->dev = pci_dev_to_dev(adapter->pdev);
		ring->netdev = adapter->netdev;

		/* configure backlink on ring */
		ring->q_vector = q_vector;

		/* update q_vector Rx values */
		txgbe_add_ring(ring, &q_vector->rx);

		/* apply Rx specific ring traits */
		ring->count = adapter->rx_ring_count;
		if (adapter->num_vmdqs > 1)
			ring->queue_index =
				rxr_idx % adapter->queues_per_pool;
		else
			ring->queue_index = rxr_idx;

		/* assign ring to adapter */
		adapter->rx_ring[rxr_idx] = ring;

		/* update count and index */
		rxr_count--;
		rxr_idx += v_count;

		/* push pointer to next ring */
		ring++;
	}

	return 0;
}

/**
 * txgbe_free_q_vector - Free memory allocated for specific interrupt vector
 * @adapter: board private structure to initialize
 * @v_idx: Index of vector to be freed
 *
 * This function frees the memory allocated to the q_vector.  In addition if
 * NAPI is enabled it will delete any references to the NAPI struct prior
 * to freeing the q_vector.
 **/
static void txgbe_free_q_vector(struct txgbe_adapter *adapter, int v_idx)
{
	struct txgbe_q_vector *q_vector = adapter->q_vector[v_idx];
	struct txgbe_ring *ring;

	txgbe_for_each_ring(ring, q_vector->tx)
		adapter->tx_ring[ring->queue_index] = NULL;

	txgbe_for_each_ring(ring, q_vector->rx)
		adapter->rx_ring[ring->queue_index] = NULL;

	adapter->q_vector[v_idx] = NULL;
	netif_napi_del(&q_vector->napi);
	kfree_rcu(q_vector, rcu);
}

/**
 * txgbe_alloc_q_vectors - Allocate memory for interrupt vectors
 * @adapter: board private structure to initialize
 *
 * We allocate one q_vector per queue interrupt.  If allocation fails we
 * return -ENOMEM.
 **/
static int txgbe_alloc_q_vectors(struct txgbe_adapter *adapter)
{
	unsigned int q_vectors = adapter->num_q_vectors;
	unsigned int rxr_remaining = adapter->num_rx_queues;
	unsigned int txr_remaining = adapter->num_tx_queues;
	unsigned int rxr_idx = 0, txr_idx = 0, v_idx = 0;
	int err;

	if (q_vectors >= (rxr_remaining + txr_remaining)) {
		for (; rxr_remaining; v_idx++) {
			err = txgbe_alloc_q_vector(adapter, q_vectors, v_idx,
						   0, 0, 1, rxr_idx);
			if (err)
				goto err_out;

			/* update counts and index */
			rxr_remaining--;
			rxr_idx++;
		}
	}

	for (; v_idx < q_vectors; v_idx++) {
		int rqpv = DIV_ROUND_UP(rxr_remaining, q_vectors - v_idx);
		int tqpv = DIV_ROUND_UP(txr_remaining, q_vectors - v_idx);
		err = txgbe_alloc_q_vector(adapter, q_vectors, v_idx,
					   tqpv, txr_idx,
					   rqpv, rxr_idx);

		if (err)
			goto err_out;

		/* update counts and index */
		rxr_remaining -= rqpv;
		txr_remaining -= tqpv;
		rxr_idx++;
		txr_idx++;
	}

	return 0;

err_out:
	adapter->num_tx_queues = 0;
	adapter->num_rx_queues = 0;
	adapter->num_q_vectors = 0;

	while (v_idx--)
		txgbe_free_q_vector(adapter, v_idx);

	return -ENOMEM;
}

/**
 * txgbe_free_q_vectors - Free memory allocated for interrupt vectors
 * @adapter: board private structure to initialize
 *
 * This function frees the memory allocated to the q_vectors.  In addition if
 * NAPI is enabled it will delete any references to the NAPI struct prior
 * to freeing the q_vector.
 **/
static void txgbe_free_q_vectors(struct txgbe_adapter *adapter)
{
	int v_idx = adapter->num_q_vectors;

	adapter->num_tx_queues = 0;
	adapter->num_rx_queues = 0;
	adapter->num_q_vectors = 0;

	while (v_idx--)
		txgbe_free_q_vector(adapter, v_idx);
}

void txgbe_reset_interrupt_capability(struct txgbe_adapter *adapter)
{
	if (adapter->flags & TXGBE_FLAG_MSIX_ENABLED) {
		adapter->flags &= ~TXGBE_FLAG_MSIX_ENABLED;
		pci_disable_msix(adapter->pdev);
		kfree(adapter->msix_entries);
		adapter->msix_entries = NULL;
	} else if (adapter->flags & TXGBE_FLAG_MSI_ENABLED) {
		adapter->flags &= ~TXGBE_FLAG_MSI_ENABLED;
		pci_disable_msi(adapter->pdev);
	}
}

/**
 * txgbe_set_interrupt_capability - set MSI-X or MSI if supported
 * @adapter: board private structure to initialize
 *
 * Attempt to configure the interrupts using the best available
 * capabilities of the hardware and the kernel.
 **/
void txgbe_set_interrupt_capability(struct txgbe_adapter *adapter)
{
	int err;

	/* We will try to get MSI-X interrupts first */
	if (!txgbe_acquire_msix_vectors(adapter))
		return;

	/* At this point, we do not have MSI-X capabilities. We need to
	 * reconfigure or disable various features which require MSI-X
	 * capability.
	 */

	/* Disable DCB unless we only have a single traffic class */
	if (netdev_get_num_tc(adapter->netdev) > 1) {
		e_dev_warn("Number of DCB TCs exceeds number of available "
			"queues. Disabling DCB support.\n");
		netdev_reset_tc(adapter->netdev);
	}

	/* Disable VMDq support */
	e_dev_warn("Disabling VMQd support\n");
	adapter->flags &= ~TXGBE_FLAG_VMDQ_ENABLED;

	/* Disable RSS */
	e_dev_warn("Disabling RSS support\n");
	adapter->ring_feature[RING_F_RSS].limit = 1;

	/* recalculate number of queues now that many features have been
	 * changed or disabled.
	 */
	txgbe_set_num_queues(adapter);
	adapter->num_q_vectors = 1;

	if (!(adapter->flags & TXGBE_FLAG_MSI_CAPABLE))
		return;

	err = pci_enable_msi(adapter->pdev);
	if (err)
		e_dev_warn("Failed to allocate MSI interrupt, falling back to "
		"legacy. Error: %d\n",
			   err);
	else
		adapter->flags |= TXGBE_FLAG_MSI_ENABLED;
}

/**
 * txgbe_init_interrupt_scheme - Determine proper interrupt scheme
 * @adapter: board private structure to initialize
 *
 * We determine which interrupt scheme to use based on...
 * - Kernel support (MSI, MSI-X)
 *   - which can be user-defined (via MODULE_PARAM)
 * - Hardware queue count (num_*_queues)
 *   - defined by miscellaneous hardware support/features (RSS, etc.)
 **/
int txgbe_init_interrupt_scheme(struct txgbe_adapter *adapter)
{
	int err;

	/* Number of supported queues */
	txgbe_set_num_queues(adapter);

	/* Set interrupt mode */
	txgbe_set_interrupt_capability(adapter);

	/* Allocate memory for queues */
	err = txgbe_alloc_q_vectors(adapter);
	if (err) {
		e_err(probe, "Unable to allocate memory for queue vectors\n");
		txgbe_reset_interrupt_capability(adapter);
		return err;
	}

	txgbe_cache_ring_register(adapter);

	set_bit(__TXGBE_DOWN, &adapter->state);

	return 0;
}

/**
 * txgbe_clear_interrupt_scheme - Clear the current interrupt scheme settings
 * @adapter: board private structure to clear interrupt scheme on
 *
 * We go through and clear interrupt specific resources and reset the structure
 * to pre-load conditions
 **/
void txgbe_clear_interrupt_scheme(struct txgbe_adapter *adapter)
{
	txgbe_free_q_vectors(adapter);
	txgbe_reset_interrupt_capability(adapter);
}

void txgbe_tx_ctxtdesc(struct txgbe_ring *tx_ring, u32 vlan_macip_lens,
		       u32 fcoe_sof_eof, u32 type_tucmd, u32 mss_l4len_idx)
{
	struct txgbe_tx_context_desc *context_desc;
	u16 i = tx_ring->next_to_use;

	context_desc = TXGBE_TX_CTXTDESC(tx_ring, i);

	i++;
	tx_ring->next_to_use = (i < tx_ring->count) ? i : 0;

	/* set bits to identify this as an advanced context descriptor */
	type_tucmd |= TXGBE_TXD_DTYP_CTXT;
	context_desc->vlan_macip_lens   = cpu_to_le32(vlan_macip_lens);
	context_desc->seqnum_seed       = cpu_to_le32(fcoe_sof_eof);
	context_desc->type_tucmd_mlhl   = cpu_to_le32(type_tucmd);
	context_desc->mss_l4len_idx     = cpu_to_le32(mss_l4len_idx);
}
