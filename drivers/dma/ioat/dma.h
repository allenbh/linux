/*
 * Copyright(c) 2004 - 2009 Intel Corporation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * The full GNU General Public License is included in this distribution in the
 * file called COPYING.
 */
#ifndef IOATDMA_H
#define IOATDMA_H

#include <linux/dmaengine.h>
#include "hw.h"
#include "registers.h"
#include <linux/init.h>
#include <linux/dmapool.h>
#include <linux/cache.h>
#include <linux/pci_ids.h>
#include <net/tcp.h>

#define IOAT_DMA_VERSION  "4.00"

#define IOAT_DMA_DCA_ANY_CPU		~0

#define to_ioatdma_device(dev) container_of(dev, struct ioatdma_device, dma_dev)
#define to_dev(ioat_chan) (&(ioat_chan)->ioat_dma->pdev->dev)
#define to_pdev(ioat_chan) ((ioat_chan)->ioat_dma->pdev)

#define chan_num(ch) ((int)((ch)->reg_base - (ch)->ioat_dma->reg_base) / 0x80)

/*
 * workaround for IOAT ver.3.0 null descriptor issue
 * (channel returns error when size is 0)
 */
#define NULL_DESC_BUFFER_SIZE 1

enum ioat_irq_mode {
	IOAT_NOIRQ = 0,
	IOAT_MSIX,
	IOAT_MSI,
	IOAT_INTX
};

/**
 * struct ioatdma_device - internal representation of a IOAT device
 * @pdev: PCI-Express device
 * @reg_base: MMIO register space base address
 * @dma_pool: for allocating DMA descriptors
 * @dma_dev: embedded struct dma_device
 * @version: version of ioatdma device
 * @msix_entries: irq handlers
 * @idx: per channel data
 * @dca: direct cache access context
 * @intr_quirk: interrupt setup quirk (for ioat_v1 devices)
 * @enumerate_channels: hw version specific channel enumeration
 * @reset_hw: hw version specific channel (re)initialization
 * @cleanup_fn: select between the v2 and v3 cleanup routines
 * @timer_fn: select between the v2 and v3 timer watchdog routines
 * @self_test: hardware version specific self test for each supported op type
 *
 * Note: the v3 cleanup routine supports raid operations
 */
struct ioatdma_device {
	struct pci_dev *pdev;
	void __iomem *reg_base;
	struct pci_pool *dma_pool;
	struct pci_pool *completion_pool;
#define MAX_SED_POOLS	5
	struct dma_pool *sed_hw_pool[MAX_SED_POOLS];
	struct dma_device dma_dev;
	u8 version;
	struct msix_entry msix_entries[4];
	struct ioatdma_chan *idx[4];
	struct dca_provider *dca;
	enum ioat_irq_mode irq_mode;
	u32 cap;
	void (*intr_quirk)(struct ioatdma_device *ioat_dma);
	int (*enumerate_channels)(struct ioatdma_device *ioat_dma);
	int (*reset_hw)(struct ioatdma_chan *ioat_chan);
	void (*cleanup_fn)(unsigned long data);
	void (*timer_fn)(unsigned long data);
	int (*self_test)(struct ioatdma_device *ioat_dma);
};

struct ioatdma_chan {
	struct dma_chan dma_chan;
	void __iomem *reg_base;
	dma_addr_t last_completion;
	spinlock_t cleanup_lock;
	unsigned long state;
	#define IOAT_COMPLETION_PENDING 0
	#define IOAT_COMPLETION_ACK 1
	#define IOAT_RESET_PENDING 2
	#define IOAT_KOBJ_INIT_FAIL 3
	#define IOAT_RESHAPE_PENDING 4
	#define IOAT_RUN 5
	#define IOAT_CHAN_ACTIVE 6
	struct timer_list timer;
	#define COMPLETION_TIMEOUT msecs_to_jiffies(100)
	#define IDLE_TIMEOUT msecs_to_jiffies(2000)
	#define RESET_DELAY msecs_to_jiffies(100)
	struct ioatdma_device *ioat_dma;
	dma_addr_t completion_dma;
	u64 *completion;
	struct tasklet_struct cleanup_task;
	struct kobject kobj;

/* ioat v2 / v3 channel attributes
 * @xfercap_log; log2 of channel max transfer length (for fast division)
 * @head: allocated index
 * @issued: hardware notification point
 * @tail: cleanup index
 * @dmacount: identical to 'head' except for occasionally resetting to zero
 * @alloc_order: log2 of the number of allocated descriptors
 * @produce: number of descriptors to produce at submit time
 * @ring: software ring buffer implementation of hardware ring
 * @prep_lock: serializes descriptor preparation (producers)
 */
	size_t xfercap_log;
	u16 head;
	u16 issued;
	u16 tail;
	u16 dmacount;
	u16 alloc_order;
	u16 produce;
	struct ioat_ring_ent **ring;
	spinlock_t prep_lock;
};

struct ioat_sysfs_entry {
	struct attribute attr;
	ssize_t (*show)(struct dma_chan *, char *);
};

/**
 * struct ioat_sed_ent - wrapper around super extended hardware descriptor
 * @hw: hardware SED
 * @sed_dma: dma address for the SED
 * @list: list member
 * @parent: point to the dma descriptor that's the parent
 */
struct ioat_sed_ent {
	struct ioat_sed_raw_descriptor *hw;
	dma_addr_t dma;
	struct ioat_ring_ent *parent;
	unsigned int hw_pool;
};

static inline struct ioatdma_chan *to_ioat_chan(struct dma_chan *c)
{
	return container_of(c, struct ioatdma_chan, dma_chan);
}



/* wrapper around hardware descriptor format + additional software fields */

#ifdef DEBUG
#define set_desc_id(desc, i) ((desc)->id = (i))
#define desc_id(desc) ((desc)->id)
#else
#define set_desc_id(desc, i)
#define desc_id(desc) (0)
#endif

static inline void
__dump_desc_dbg(struct ioatdma_chan *ioat_chan, struct ioat_dma_descriptor *hw,
		struct dma_async_tx_descriptor *tx, int id)
{
	struct device *dev = to_dev(ioat_chan);

	dev_dbg(dev, "desc[%d]: (%#llx->%#llx) cookie: %d flags: %#x"
		" ctl: %#10.8x (op: %#x int_en: %d compl: %d)\n", id,
		(unsigned long long) tx->phys,
		(unsigned long long) hw->next, tx->cookie, tx->flags,
		hw->ctl, hw->ctl_f.op, hw->ctl_f.int_en, hw->ctl_f.compl_write);
}

#define dump_desc_dbg(c, d) \
	({ if (d) __dump_desc_dbg(c, d->hw, &d->txd, desc_id(d)); 0; })

static inline struct ioatdma_chan *
ioat_chan_by_index(struct ioatdma_device *ioat_dma, int index)
{
	return ioat_dma->idx[index];
}

static inline u64 ioat_chansts_32(struct ioatdma_chan *ioat_chan)
{
	u8 ver = ioat_chan->ioat_dma->version;
	u64 status;
	u32 status_lo;

	/* We need to read the low address first as this causes the
	 * chipset to latch the upper bits for the subsequent read
	 */
	status_lo = readl(ioat_chan->reg_base + IOAT_CHANSTS_OFFSET_LOW(ver));
	status = readl(ioat_chan->reg_base + IOAT_CHANSTS_OFFSET_HIGH(ver));
	status <<= 32;
	status |= status_lo;

	return status;
}

#if BITS_PER_LONG == 64

static inline u64 ioat_chansts(struct ioatdma_chan *ioat_chan)
{
	u8 ver = ioat_chan->ioat_dma->version;
	u64 status;

	 /* With IOAT v3.3 the status register is 64bit.  */
	if (ver >= IOAT_VER_3_3)
		status = readq(ioat_chan->reg_base + IOAT_CHANSTS_OFFSET(ver));
	else
		status = ioat_chansts_32(ioat_chan);

	return status;
}

#else
#define ioat_chansts ioat_chansts_32
#endif

static inline u64 ioat_chansts_to_addr(u64 status)
{
	return status & IOAT_CHANSTS_COMPLETED_DESCRIPTOR_ADDR;
}

static inline u32 ioat_chanerr(struct ioatdma_chan *ioat_chan)
{
	return readl(ioat_chan->reg_base + IOAT_CHANERR_OFFSET);
}

static inline void ioat_suspend(struct ioatdma_chan *ioat_chan)
{
	u8 ver = ioat_chan->ioat_dma->version;

	writeb(IOAT_CHANCMD_SUSPEND,
	       ioat_chan->reg_base + IOAT_CHANCMD_OFFSET(ver));
}

static inline void ioat_reset(struct ioatdma_chan *ioat_chan)
{
	u8 ver = ioat_chan->ioat_dma->version;

	writeb(IOAT_CHANCMD_RESET,
	       ioat_chan->reg_base + IOAT_CHANCMD_OFFSET(ver));
}

static inline bool ioat_reset_pending(struct ioatdma_chan *ioat_chan)
{
	u8 ver = ioat_chan->ioat_dma->version;
	u8 cmd;

	cmd = readb(ioat_chan->reg_base + IOAT_CHANCMD_OFFSET(ver));
	return (cmd & IOAT_CHANCMD_RESET) == IOAT_CHANCMD_RESET;
}

static inline bool is_ioat_active(unsigned long status)
{
	return ((status & IOAT_CHANSTS_STATUS) == IOAT_CHANSTS_ACTIVE);
}

static inline bool is_ioat_idle(unsigned long status)
{
	return ((status & IOAT_CHANSTS_STATUS) == IOAT_CHANSTS_DONE);
}

static inline bool is_ioat_halted(unsigned long status)
{
	return ((status & IOAT_CHANSTS_STATUS) == IOAT_CHANSTS_HALTED);
}

static inline bool is_ioat_suspended(unsigned long status)
{
	return ((status & IOAT_CHANSTS_STATUS) == IOAT_CHANSTS_SUSPENDED);
}

/* channel was fatally programmed */
static inline bool is_ioat_bug(unsigned long err)
{
	return !!err;
}

int ioat_probe(struct ioatdma_device *ioat_dma);
int ioat_register(struct ioatdma_device *ioat_dma);
int ioat_dma_self_test(struct ioatdma_device *ioat_dma);
void ioat_dma_remove(struct ioatdma_device *ioat_dma);
struct dca_provider *ioat_dca_init(struct pci_dev *pdev, void __iomem *iobase);
void ioat_init_channel(struct ioatdma_device *ioat_dma,
		       struct ioatdma_chan *ioat_chan, int idx);
enum dma_status ioat_dma_tx_status(struct dma_chan *c, dma_cookie_t cookie,
				   struct dma_tx_state *txstate);
bool ioat_cleanup_preamble(struct ioatdma_chan *ioat_chan,
			   dma_addr_t *phys_complete);
void ioat_kobject_add(struct ioatdma_device *ioat_dma, struct kobj_type *type);
void ioat_kobject_del(struct ioatdma_device *ioat_dma);
int ioat_dma_setup_interrupts(struct ioatdma_device *ioat_dma);
void ioat_stop(struct ioatdma_chan *ioat_chan);
extern const struct sysfs_ops ioat_sysfs_ops;
extern struct ioat_sysfs_entry ioat_version_attr;
extern struct ioat_sysfs_entry ioat_cap_attr;
#endif /* IOATDMA_H */
