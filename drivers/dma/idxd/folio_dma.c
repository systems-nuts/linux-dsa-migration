// SPDX-License-Identifier: GPL-2.0
/*
 * Author: Karim Manaouil - k.manaouil@gmail.com
 *
 * Edinburgh, 2026
 *
 */
#include <asm/rdtsc.h>
#include <asm/tsc.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/device.h>
#include <linux/io-64-nonatomic-lo-hi.h>
#include <linux/dma-direct.h>
#include <linux/dmaengine.h>
#include <linux/latency.h>
#include <uapi/linux/idxd.h>
#include <linux/migrate_copy_offload.h>
#include "registers.h"
#include "idxd.h"

#define CREATE_TRACE_POINTS
#include "folio_dma_trace.h"


/*
 * if IOMMU is in passthrough mode with cmdline having iommu.passthrough=1,
 * then we don't have to call dma_map_*().
 */
#undef IOMMU_PT

static bool idxd_migrator_enabled = false;

static struct idxd_wq *wqs[MAX_NUMNODES];
static size_t nr_wqs;

static DEFINE_MUTEX(idxd_lock);

static struct idxd_wq *active_wq;

static inline void advance_list_head(struct list_head **head,
		unsigned int nr_pos)
{
	int i;

	for (i = 0; i < nr_pos; i++)
		*head = (*head)->next;
}

static struct idxd_desc *__get_desc(struct idxd_wq *wq, unsigned int idx,
		bool batch, int cpu)
{
	struct idxd_desc *descs = batch? wq->batch_descs: wq->descs;
	struct idxd_device *idxd = wq->idxd;
	struct idxd_desc *desc;

	desc = &descs[idx];
	desc->cpu = cpu;
	desc->dma_completed = false;

	if (device_pasid_enabled(idxd))
		desc->hw->pasid = idxd->pasid;

	return desc;
}

static inline dma_addr_t get_compl_addr(struct idxd_wq *wq,
		unsigned int idx, bool batch)
{
	dma_addr_t base = batch? wq->batch_compls_addr : wq->compls_addr;
	struct idxd_device *idxd = wq->idxd;

	BUG_ON(!batch && idx >= wq->size);
	BUG_ON(batch && idx >= wq->num_batch_descs);

	return base + idxd->data->compl_size * idx;
}

/*
 * Only accepts multiples of 2 for now
 */
static noinline int submit_folios(struct idxd_wq *wq, struct list_head *dst_folios,
		struct list_head *src_folios, unsigned int len, bool batch)
{
	u32 flags = IDXD_OP_FLAG_CRAV | IDXD_OP_FLAG_RCR;
	struct folio *folio_src, *folio_dst;
	unsigned int batch_size, nr_descs;
	struct idxd_desc *descs;
	void __iomem *portal;
	struct device *dev;
	int i;

	dev = &wq->idxd->pdev->dev;
	batch_size = min_t(unsigned int, wq->size, wq->max_batch_size);

	folio_src = list_first_entry(src_folios, struct folio, lru);
	folio_dst = list_first_entry(dst_folios, struct folio, lru);

	memset(wq->hw_descs, 0, sizeof(*wq->hw_descs) * len);
	memset(wq->compls, 0, sizeof(*wq->compls) * len);

	/*
	 * Phase 1: prepare descriptors
	 */
	for (i = 0; i < len; i++) {
		struct idxd_desc *desc = __get_desc(wq, i, false, -1);
		struct dsa_hw_desc *hw_desc = desc->hw;
		dma_addr_t hwaddr_src, hwaddr_dst;
		dma_addr_t compl_addr;

		BUG_ON(folio_size(folio_src) != folio_size(folio_dst));
#ifdef IOMMU_PT
		hwaddr_src = PFN_PHYS(folio_pfn(folio_src));
		hwaddr_dst = PFN_PHYS(folio_pfn(folio_dst));
#else
		hwaddr_src = dma_map_page(dev, &folio_src->page, 0,
					  folio_size(folio_src), DMA_TO_DEVICE);
		if (dma_mapping_error(dev, hwaddr_src))
			BUG();
		hwaddr_dst = dma_map_page(dev, &folio_dst->page, 0,
					  folio_size(folio_dst), DMA_FROM_DEVICE);
		if (dma_mapping_error(dev, hwaddr_dst))
			BUG();
#endif
		compl_addr = batch ? 0 : get_compl_addr(wq, i, false);

		idxd_prep_desc_common(wq, hw_desc, DSA_OPCODE_MEMMOVE,
			      hwaddr_src, hwaddr_dst, folio_size(folio_src),
			      compl_addr, batch ? 0 : flags);

		folio_src = list_next_entry(folio_src, lru);
		folio_dst = list_next_entry(folio_dst, lru);
	}

	nr_descs = batch ? DIV_ROUND_UP(len, batch_size) : len;

	if (batch) {
		dma_addr_t hw_descs_addr = wq->hw_descs_addr;
		unsigned int blen, nr_remaining = len;

		memset(wq->hw_batch_descs, 0, sizeof(*wq->hw_batch_descs) * nr_descs);
		memset(wq->batch_compls, 0, sizeof(*wq->batch_compls) * nr_descs);

		for (i = 0; i < nr_descs; i++) {
			struct idxd_desc *bdesc = __get_desc(wq, i, true, -1);
			struct dsa_hw_desc *hw_bdesc = bdesc->hw;
			dma_addr_t bcompl_addr = get_compl_addr(wq, i, true);

			blen = min_t(unsigned int, batch_size, nr_remaining);

			idxd_prep_batch_desc(wq, hw_bdesc, hw_descs_addr, blen,
					     bcompl_addr, flags);

			hw_descs_addr += sizeof(struct dsa_hw_desc) * blen;
			nr_remaining -= blen;
		}
	}

	/*
	 * Phase 2: submit descriptors
	 */
	descs = batch ? wq->batch_descs : wq->descs;

	portal = idxd_wq_portal_addr(wq);
	/* Flush the writes before DMA for coherence */
	wmb();
	for (i = 0; i < nr_descs; i++) {
		struct idxd_desc *desc = &descs[i];

		iosubmit_cmds512(portal, desc->hw, 1);
	}

	wq->nr_descs_in_flight = len;
	wq->in_flight = true;

	return 0;
}

static noinline int poll_completions(struct idxd_wq *wq, bool batch)
{
	struct idxd_desc *descs = batch ? wq->batch_descs : wq->descs;
	unsigned int len = wq->nr_descs_in_flight;
	unsigned int batch_size, to_complete;
	unsigned int completed = 0;
	int i, rc = 0;

	batch_size = min_t(size_t, wq->size, wq->max_batch_size);
	to_complete = batch ? DIV_ROUND_UP(len, batch_size) : len;

	while (completed < to_complete) {
		for (i = 0; i < to_complete; i++) {
			struct idxd_desc *desc = &descs[i];

			if (desc->dma_completed)
				continue;
			if (!READ_ONCE(desc->completion->status))
				continue;
			if (desc->completion->status != DSA_COMP_SUCCESS) {
				pr_err("descriptor %i failed %u\n", i, desc->completion->status);
				rc = -EIO;
			}
			desc->dma_completed = true;
			completed++;
		}
		cpu_relax();
	}

#ifndef IOMMU_PT
	struct device *dev = &wq->idxd->pdev->dev;
	for (i = 0; i < len; i++) {
		struct idxd_desc *desc = &wq->descs[i];
		struct dsa_hw_desc *hw = desc->hw;

		dma_unmap_page(dev, hw->src_addr, hw->xfer_size, DMA_TO_DEVICE);
		dma_unmap_page(dev, hw->dst_addr, hw->xfer_size, DMA_FROM_DEVICE);
	}
#endif
	return rc;
}

static int complete_copy(void)
{
	unsigned int nr_descs = active_wq ? active_wq->nr_descs_in_flight : 0;
	int ret = 0;

	if (active_wq && active_wq->in_flight) {
		ret = poll_completions(active_wq, false);
		active_wq->in_flight = false;
	}
	active_wq = NULL;
	mutex_unlock(&idxd_lock);

	return ret;
}

static int folios_copy_dma(struct list_head *dst_list,
		struct list_head *src_list, unsigned int nr_folios)
{
	int ret;

	trace_idxd_folios_copy_dma_begin(nr_folios);

	/*
	 * This is going to be unlocked in complete_copy()
	 */
	if (!mutex_trylock(&idxd_lock)) {
		ret = -EBUSY;
		goto out;
	}

	active_wq = wqs[numa_node_id()];

	/*
	 * Batching requires careful handling of the number of descriptors.
	 * Disable it for now.
	 */
	if (!active_wq || nr_folios > active_wq->size) {
		ret = -EINVAL;
		goto unlock;
	}

	ret = submit_folios(active_wq, dst_list, src_list, nr_folios, false);
	if (!ret)
		goto out;
unlock:
	active_wq = NULL;
	mutex_unlock(&idxd_lock);
out:
	trace_idxd_folios_copy_dma_end(nr_folios, ret);
	return ret;
}

static struct migrator idxd_dma_migrator = {
	.name = "idxd",
	.offload_copy = folios_copy_dma,
	.complete_copy = complete_copy,
	.owner = THIS_MODULE,
};

static int idxd_fast_drv_probe(struct idxd_dev *idxd_dev)
{
	struct device *dev = &idxd_dev->conf_dev;
	struct idxd_wq *wq = idxd_dev_to_wq(idxd_dev);
	struct idxd_device *idxd = wq->idxd;
	int rc, node;

	if (idxd->state != IDXD_DEV_ENABLED)
		return -ENXIO;

	mutex_lock(&wq->wq_lock);
	if (!idxd_wq_driver_name_match(wq, dev)) {
		idxd->cmd_status = IDXD_SCMD_WQ_NO_DRV_NAME;
		rc = -ENODEV;
		goto err;
	}

	wq->type = IDXD_WQT_KERNEL;

	rc = idxd_drv_enable_wq(wq);
	if (rc < 0) {
		dev_dbg(dev, "Enable wq %d failed: %d\n", wq->id, rc);
		rc = -ENXIO;
		goto err;
	}

	wq->in_flight = false;

	node = dev_to_node(&idxd->pdev->dev);
	wqs[node] = wq;
	nr_wqs++;

	idxd->cmd_status = 0;
	mutex_unlock(&wq->wq_lock);

	mutex_lock(&idxd_lock);
	if (!idxd_migrator_enabled) {
		rc = migrate_offload_register(&idxd_dma_migrator);
		if (rc) {
			mutex_unlock(&idxd_lock);
			return rc;
		}
		idxd_migrator_enabled = true;
		pr_info("idxd: folio dma migrator is enabled\n");
	}
	mutex_unlock(&idxd_lock);

	return 0;
err:
	wq->type = IDXD_WQT_NONE;
	mutex_unlock(&wq->wq_lock);
	return rc;
}

static void idxd_fast_drv_remove(struct idxd_dev *idxd_dev)
{
	struct idxd_wq *wq = idxd_dev_to_wq(idxd_dev);

	mutex_lock(&wq->wq_lock);
	__idxd_wq_quiesce(wq);
	idxd_drv_disable_wq(wq);
	mutex_unlock(&wq->wq_lock);

	mutex_lock(&idxd_lock);
	migrate_offload_unregister(&idxd_dma_migrator);
	mutex_unlock(&idxd_lock);
}

static enum idxd_dev_type dev_types[] = {
	IDXD_DEV_WQ,
	IDXD_DEV_NONE,
};

struct idxd_device_driver idxd_fast_drv = {
	.probe = idxd_fast_drv_probe,
	.remove = idxd_fast_drv_remove,
	.name = "fast",
	.type = dev_types,
};
EXPORT_SYMBOL_GPL(idxd_fast_drv);
