/*
 * Copyright (c) 2017 Samsung Electronics Co., Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/io.h>
#include <linux/debugfs.h>
#include <linux/module.h>
#include <linux/iommu.h>
#include <linux/of_reserved_mem.h>
#include <linux/uaccess.h>
#include "chub_dbg.h"
#include "chub_ipc.h"
#include "chub.h"
#ifdef CONFIG_CHRE_SENSORHUB_HAL
#include "main.h"
#endif

#define NUM_OF_GPR (17)
#define GPR_PC_INDEX (16)
#define AREA_NAME_MAX (8)
/* it's align ramdump side to prevent override */
#define SRAM_ALIGN (1024)
#define S_IRWUG (0660)

struct map_info {
	char name[AREA_NAME_MAX];
	u32 offset;
	u32 size;
};

struct dbg_dump {
	struct map_info info[DBG_AREA_MAX];
	long long time;
	int reason;
	struct contexthub_ipc_info chub;
	struct ipc_area ipc_addr[IPC_REG_MAX];
	u32 gpr[NUM_OF_GPR];
	int sram_start;
	char sram[];
};

static struct dbg_dump *p_dbg_dump;
static struct reserved_mem *chub_rmem;

void chub_dbg_dump_gpr(struct contexthub_ipc_info *ipc)
{
	int ret = contexthub_request(ipc);

	if (ret) {
		pr_err("%s: fails to contexthub_request\n", __func__);
		return;
	}

	if (p_dbg_dump) {
		int i;
		struct dbg_dump *p_dump = p_dbg_dump;

		IPC_HW_WRITE_DUMPGPR_CTRL(ipc->chub_dumpgrp, 0x1);
		/* dump GPR */
		for (i = 0; i <= GPR_PC_INDEX - 1; i++)
			p_dump->gpr[i] =
			    readl(ipc->chub_dumpgrp + REG_CHUB_DUMPGPR_GP0R +
				  i * 4);
		p_dump->gpr[GPR_PC_INDEX] =
		    readl(ipc->chub_dumpgrp + REG_CHUB_DUMPGPR_PCR);
	}

	contexthub_release(ipc);
}

static u32 get_dbg_dump_size(void)
{
	return sizeof(struct dbg_dump) + ipc_get_chub_mem_size();
};

#ifdef	CONFIG_CONTEXTHUB_DEBUG
static void chub_dbg_write_file(struct device *dev, char *name, void *buf, int size)
{
	struct file *filp;
	char file_name[32];
	mm_segment_t old_fs;
	struct dbg_dump *p_dump = p_dbg_dump;
	u32 sec = p_dump->time / NSEC_PER_SEC;

	snprintf(file_name, sizeof(file_name), "/data/nano-%02u-%06u-%s.dump",
		 p_dump->reason, sec, name);

	old_fs = get_fs();
	set_fs(KERNEL_DS);

	filp = filp_open(file_name, O_RDWR | O_TRUNC | O_CREAT, S_IRWUG);
	if (IS_ERR(filp)) {
		dev_warn(dev, "%s: open file fail\n", __func__);
		goto out;
	}

	vfs_write(filp, buf, size,  &filp->f_pos);
	vfs_fsync(filp, 0);
	filp_close(filp, NULL);

	dev_dbg(dev, "%s is created with %d size\n", file_name,
		get_dbg_dump_size());
out:
	set_fs(old_fs);
}
#endif

void chub_dbg_dump_hw(struct contexthub_ipc_info *ipc, int reason)
{
	int ret = contexthub_request(ipc);

	if (ret) {
		pr_err("%s: fails to contexthub_request\n", __func__);
		return;
	}

	if (p_dbg_dump) {
		p_dbg_dump->time = sched_clock();
		p_dbg_dump->reason = reason;

		/* dump GPR */
		chub_dbg_dump_gpr(ipc);
		/* dump SRAM to reserved DRAM */
		memcpy_fromio(&p_dbg_dump->sram[p_dbg_dump->sram_start],
			      ipc_get_base(IPC_REG_DUMP),
			      ipc_get_chub_mem_size());

#ifdef	CONFIG_CONTEXTHUB_DEBUG
		/* write file */
		dev_dbg(ipc->dev,
			"%s: write file: sram:%p, dram:%p(off:%d), size:%d\n",
			__func__, ipc_get_base(IPC_REG_DUMP),
			&p_dbg_dump->sram[p_dbg_dump->sram_start],
			p_dbg_dump->sram_start, ipc_get_chub_mem_size());
		chub_dbg_write_file(ipc->dev, "dram",
			p_dbg_dump, sizeof(struct dbg_dump));
		chub_dbg_write_file(ipc->dev, "sram",
			&p_dbg_dump->sram[p_dbg_dump->sram_start],
			ipc_get_chub_mem_size());
#endif
	}
	contexthub_release(ipc);
}

void chub_dbg_check_and_download_image(struct contexthub_ipc_info *ipc)
{
	u32 *bl = vmalloc(ipc_get_offset(IPC_REG_BL));
	int ret;

	memcpy_fromio(bl, ipc_get_base(IPC_REG_BL), ipc_get_offset(IPC_REG_BL));
	contexthub_download_image(ipc, 1);

	ret = memcmp(bl, ipc_get_base(IPC_REG_BL), ipc_get_offset(IPC_REG_BL));
	if (ret) {
		int i;
		u32 *bl_image = (u32 *)ipc_get_base(IPC_REG_BL);

		pr_info("bl doens't match with size %d\n", ipc_get_offset(IPC_REG_BL));

		for (i = 0; i < ipc_get_offset(IPC_REG_BL) / 4; i++)
			if (bl[i] != bl_image[i]) {
				pr_info("bl[%d] %x -> wrong %x\n", i,
					bl_image[i], bl[i]);
				break;
			}
	}
	contexthub_download_image(ipc, 0);

	/* os image is dumped on &p_dbg_dump->sram[p_dbg_dump->sram_start] */
	ret = memcmp(&p_dbg_dump->sram[p_dbg_dump->sram_start],
		     ipc_get_base(IPC_REG_OS), ipc_get_offset(IPC_REG_OS));

	if (ret)
		pr_info("os doens't match with size %d\n",
			ipc_get_offset(IPC_REG_OS));

	vfree(bl);
}

void chub_dbg_dump_status(struct contexthub_ipc_info *ipc)
{
	int val;
	struct nanohub_data *data = ipc->data;

	CSP_PRINTF_INFO
	    ("CHUB DUMP: nanohub driver status\nwu:%d wu_l:%d acq:%d irq1_apInt:%d fired:%d\n",
	     atomic_read(&data->wakeup_cnt),
	     atomic_read(&data->wakeup_lock_cnt),
	     atomic_read(&data->wakeup_acquired),
	     atomic_read(&ipc->irq1_apInt), nanohub_irq1_fired(data));

	if (!contexthub_is_run(ipc)) {
		pr_warn("%s: chub isn't run\n", __func__);
		return;
	}

#ifndef USE_IPC_BUF
	CSP_PRINTF_INFO
	    ("CHUB DUMP: contexthub driver status\nflag:%x cnt:%d, order:%lu\nalive container:\n",
	     ipc->read_lock.flag, atomic_read(&ipc->read_lock.cnt),
	     ipc->recv_order.order);
	for (val = 0; val < IRQ_EVT_CH_MAX; val++)
		if (ipc->recv_order.container[val])
			CSP_PRINTF_INFO("container[%d]:%lu\n", val,
					ipc->recv_order.container[val]);
#endif
	for (val = 0; val < CHUB_ERR_MAX; val++)
		if (ipc->err_cnt[val])
			CSP_PRINTF_INFO("error %d occurs %d times\n",
					val, ipc->err_cnt[val]);
	ipc_dump();
	/* dump nanohub kernel status */
	CSP_PRINTF_INFO("CHUB DUMP: Request to dump nanohub kernel status\n");
	ipc_write_debug_event(AP, (u32)MAILBOX_EVT_DUMP_STATUS);
	ipc_add_evt(IPC_EVT_A2C, IRQ_EVT_A2C_DEBUG);
	log_flush(ipc->fw_log);
}

static ssize_t chub_bin_sram_read(struct file *file, struct kobject *kobj,
				  struct bin_attribute *battr, char *buf,
				  loff_t off, size_t size)
{
	struct device *dev = kobj_to_dev(kobj);

	dev_dbg(dev, "%s(%lld, %zu)\n", __func__, off, size);

	if (!contexthub_is_run(dev_get_drvdata(dev))) {
		pr_warn("%s: chub isn't run\n", __func__);
		return -EINVAL;
	}

	memcpy_fromio(buf, battr->private + off, size);
	return size;
}

static ssize_t chub_bin_dram_read(struct file *file, struct kobject *kobj,
				  struct bin_attribute *battr, char *buf,
				  loff_t off, size_t size)
{
	struct device *dev = kobj_to_dev(kobj);

	dev_dbg(dev, "%s(%lld, %zu)\n", __func__, off, size);
	memcpy(buf, battr->private + off, size);
	return size;
}

static BIN_ATTR_RO(chub_bin_sram, 0);
static BIN_ATTR_RO(chub_bin_dram, 0);

static struct bin_attribute *chub_bin_attrs[] = {
	&bin_attr_chub_bin_sram,
	&bin_attr_chub_bin_dram,
};

#define SIZE_UTC_NAME (16)

char chub_utc_name[][SIZE_UTC_NAME] = {
	[IPC_DEBUG_UTC_STOP] = "stop",
	[IPC_DEBUG_UTC_AGING] = "aging",
	[IPC_DEBUG_UTC_WDT] = "wdt",
	[IPC_DEBUG_UTC_RTC] = "rtc",
	[IPC_DEBUG_UTC_TIMER] = "timer",
	[IPC_DEBUG_UTC_MEM] = "mem",
	[IPC_DEBUG_UTC_GPIO] = "gpio",
	[IPC_DEBUG_UTC_SPI] = "spi",
	[IPC_DEBUG_UTC_CMU] = "cmu",
	[IPC_DEBUG_UTC_GPIO] = "gpio",
	[IPC_DEBUG_UTC_TIME_SYNC] = "time_sync",
	[IPC_DEBUG_UTC_ASSERT] = "assert",
	[IPC_DEBUG_UTC_FAULT] = "fault",
	[IPC_DEBUG_UTC_CHECK_STATUS] = "stack",
	[IPC_DEBUG_UTC_CHECK_CPU_UTIL] = "utilization",
	[IPC_DEBUG_UTC_HEAP_DEBUG] = "heap",
	[IPC_DEBUG_UTC_HANG] = "hang",
	[IPC_DEBUG_NANOHUB_CHUB_ALIVE] = "alive",
};

static ssize_t chub_utc_show(struct device *kobj,
			     struct device_attribute *attr, char *buf)
{
	int i;
	int index = 0;

	for (i = 0; i < sizeof(chub_utc_name) / SIZE_UTC_NAME; i++)
		if (chub_utc_name[i][0])
			index +=
			    sprintf(buf + index, "%d %s\n", i,
				    chub_utc_name[i]);

	return index;
}

static ssize_t chub_utc_store(struct device *dev,
			      struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct contexthub_ipc_info *ipc = dev_get_drvdata(dev);
	long event;
	int err;

	err = kstrtol(&buf[0], 10, &event);
	if (!err) {
		if (event == IPC_DEBUG_NANOHUB_CHUB_ALIVE)
			event = MAILBOX_EVT_CHUB_ALIVE;

		if (event != MAILBOX_EVT_CHUB_ALIVE) {
			err = contexthub_request(ipc);
			if (err)
				pr_err("%s: fails to request contexthub. ret:%d\n", __func__, err);
		}

		contexthub_ipc_write_event(ipc, event);
		if (event != MAILBOX_EVT_CHUB_ALIVE)
			contexthub_release(ipc);

		return count;
	} else {
		return 0;
	}
}

static ssize_t chub_ipc_store(struct device *dev,
			      struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct contexthub_ipc_info *ipc = dev_get_drvdata(dev);
	char input[PACKET_SIZE_MAX];
	char output[PACKET_SIZE_MAX];
	int ret;

	memset(input, 0, PACKET_SIZE_MAX);
	memset(output, 0, PACKET_SIZE_MAX);

	if (count <= PACKET_SIZE_MAX) {
		memset(input, 0, PACKET_SIZE_MAX);
		memcpy(input, buf, count);
	} else {
		pr_err("%s: ipc size(%d) is bigger than max(%d)\n",
			__func__, (int)count, (int)PACKET_SIZE_MAX);
		return -EINVAL;
	}

	ret = contexthub_request(ipc);
	if (ret) {
		pr_err("%s: fails to request contexthub. ret:%d\n", __func__, ret);
		return ret;
	}

	ret = contexthub_ipc_write_event(ipc, (u32)IPC_DEBUG_UTC_IPC_TEST_START);
	if (ret) {
		pr_err("%s: fails to set start test event. ret:%d\n", __func__, ret);
		count = ret;
		goto out;
	}

	ret = contexthub_ipc_write(ipc, input, count, IPC_MAX_TIMEOUT);
	if (ret != count) {
		pr_info("%s: fail to write\n", __func__);
		return -EINVAL;
	}

	ret = contexthub_ipc_read(ipc, output, 0, IPC_MAX_TIMEOUT);
	if (count != ret) {
		pr_info("%s: fail to read ret:%d\n", __func__, ret);
		return -EINVAL;
	}

	if (strncmp(input, output, count)) {
		pr_info("%s: fail to compare input/output\n", __func__);
		print_hex_dump(KERN_CONT, "chub input:",
				       DUMP_PREFIX_OFFSET, 16, 1, input,
				       count, false);
		print_hex_dump(KERN_CONT, "chub output:",
				       DUMP_PREFIX_OFFSET, 16, 1, output,
				       count, false);
		return 0;
	}
	ret = contexthub_ipc_write_event(ipc, (u32)IPC_DEBUG_UTC_IPC_TEST_END);
	if (ret) {
		pr_err("%s: fails to set end test event. ret:%d\n", __func__, ret);
		count = ret;
	} else
		pr_info("[%s pass] len:%d, str: %s\n", __func__, (int)count, output);

out:
	contexthub_release(ipc);

	return count;
}

static ssize_t chub_get_dump_status_store(struct device *dev,
					  struct device_attribute *attr,
					  const char *buf, size_t count)
{
	struct contexthub_ipc_info *ipc = dev_get_drvdata(dev);
	int ret = contexthub_request(ipc);

	if (ret) {
		pr_err("%s: fails to contexthub_request\n", __func__);
		return 0;
	}

	chub_dbg_dump_status(ipc);
	contexthub_ipc_write_event(ipc, MAILBOX_EVT_DUMP_STATUS);

	contexthub_release(ipc);
	return count;
}

static ssize_t chub_get_gpr_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct contexthub_ipc_info *ipc = dev_get_drvdata(dev);
	char *pbuf = buf;
	int i;

	if (p_dbg_dump) {
		chub_dbg_dump_gpr(ipc);

		pbuf +=
		    sprintf(pbuf, "========================================\n");
		pbuf += sprintf(pbuf, "CHUB CPU register dump\n");

		for (i = 0; i <= 15; i++)
			pbuf +=
			    sprintf(pbuf, "R%02d        : %08x\n", i,
				    p_dbg_dump->gpr[i]);

		pbuf +=
		    sprintf(pbuf, "PC         : %08x\n",
			    p_dbg_dump->gpr[GPR_PC_INDEX]);
		pbuf +=
		    sprintf(pbuf, "========================================\n");
	}

	return pbuf - buf;
}

static ssize_t chub_set_dump_hw_store(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, size_t count)
{
	struct contexthub_ipc_info *ipc = dev_get_drvdata(dev);

	chub_dbg_dump_hw(ipc, 0);
	return count;
}

static ssize_t chub_wakeup_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	long event;
	int ret;
	struct contexthub_ipc_info *ipc = dev_get_drvdata(dev);
	ret = kstrtol(&buf[0], 10, &event);
	if (ret)
		return ret;

	if (event)
		ret = contexthub_request(ipc);
	else
		contexthub_release(ipc);

	return ret ? ret : count;
}

static struct device_attribute attributes[] = {
	__ATTR(get_gpr, 0440, chub_get_gpr_show, NULL),
	__ATTR(dump_status, 0220, NULL, chub_get_dump_status_store),
	__ATTR(dump_hw, 0220, NULL, chub_set_dump_hw_store),
	__ATTR(utc, 0664, chub_utc_show, chub_utc_store),
	__ATTR(ipc_test, 0220, NULL, chub_ipc_store),
	__ATTR(wakeup, 0220, NULL, chub_wakeup_store),
};

void *chub_dbg_get_memory(enum dbg_dump_area area)
{
	void *addr;
	int size;

	pr_info("%s: chub_rmem: %p\n", __func__, chub_rmem);

	if (!chub_rmem)
		return NULL;

	if (area == DBG_NANOHUB_DD_AREA) {
		addr = &p_dbg_dump->chub;
		size = sizeof(p_dbg_dump->chub);
	} else {
		return NULL;
	}

	memset(addr, 0, size);

	return addr;
}

int chub_dbg_init(struct device *dev)
{
	int i, ret = 0;
	enum dbg_dump_area area;

	if (!chub_rmem)
		return -EINVAL;

	bin_attr_chub_bin_sram.size = ipc_get_chub_mem_size();
	bin_attr_chub_bin_sram.private = ipc_get_base(IPC_REG_DUMP);

	bin_attr_chub_bin_dram.size = sizeof(struct dbg_dump);
	bin_attr_chub_bin_dram.private = p_dbg_dump;

	if (chub_rmem->size < get_dbg_dump_size())
		dev_err(dev,
			"rmem size (%u) should be bigger than dump size(%u)\n",
			(u32)chub_rmem->size, get_dbg_dump_size());

	for (i = 0; i < ARRAY_SIZE(chub_bin_attrs); i++) {
		struct bin_attribute *battr = chub_bin_attrs[i];

		ret = device_create_bin_file(dev, battr);
		if (ret < 0)
			dev_warn(dev, "Failed to create file: %s\n",
				 battr->attr.name);
	}

	for (i = 0, ret = 0; i < ARRAY_SIZE(attributes); i++) {
		ret = device_create_file(dev, &attributes[i]);
		if (ret)
			dev_warn(dev, "Failed to create file: %s\n",
				 attributes[i].attr.name);
	}

	area = DBG_IPC_AREA;
	strncpy(p_dbg_dump->info[area].name, "ipc_map", AREA_NAME_MAX);
	p_dbg_dump->info[area].offset =
	    (void *)p_dbg_dump->ipc_addr - (void *)p_dbg_dump;
	p_dbg_dump->info[area].size = sizeof(struct ipc_area) * IPC_REG_MAX;

	area = DBG_NANOHUB_DD_AREA;
	strncpy(p_dbg_dump->info[area].name, "nano_dd", AREA_NAME_MAX);
	p_dbg_dump->info[area].offset =
	    (void *)&p_dbg_dump->chub - (void *)p_dbg_dump;
	p_dbg_dump->info[area].size = sizeof(struct contexthub_ipc_info);

	area = DBG_GPR_AREA;
	strncpy(p_dbg_dump->info[area].name, "gpr", AREA_NAME_MAX);
	p_dbg_dump->info[area].offset =
	    (void *)p_dbg_dump->gpr - (void *)p_dbg_dump;
	p_dbg_dump->info[area].size = sizeof(u32) * NUM_OF_GPR;

	area = DBG_SRAM_AREA;
	/* align the chub sram dump base address on rmem into SRAM_ALIN */
	p_dbg_dump->sram_start = SRAM_ALIGN - bin_attr_chub_bin_dram.size;
	if (p_dbg_dump->sram_start < 0) {
		dev_warn(dev,
			 "increase SRAM_ALIGN from %d to %d to align on ramdump.\n",
			 SRAM_ALIGN, (u32)bin_attr_chub_bin_dram.size);
		p_dbg_dump->sram_start = 0;
	}
	strncpy(p_dbg_dump->info[area].name, "sram", AREA_NAME_MAX);
	p_dbg_dump->info[area].offset =
	    (void *)&p_dbg_dump->sram[p_dbg_dump->sram_start] -
	    (void *)p_dbg_dump;
	p_dbg_dump->info[area].size = bin_attr_chub_bin_sram.size;

	dev_dbg(dev,
		"%s(%pa) is mapped on %p (sram %p) with size of %u, dump size %u\n",
		"dump buffer", &chub_rmem->base, phys_to_virt(chub_rmem->base),
		&p_dbg_dump->sram[p_dbg_dump->sram_start],
		(u32)chub_rmem->size, get_dbg_dump_size());

	return ret;
}

static int __init contexthub_rmem_setup(struct reserved_mem *rmem)
{
	pr_info("%s: base=%pa, size=%pa\n", __func__, &rmem->base, &rmem->size);

	chub_rmem = rmem;
	p_dbg_dump = phys_to_virt(rmem->base);
	return 0;
}
RESERVEDMEM_OF_DECLARE(chub_rmem, "exynos,chub_rmem", contexthub_rmem_setup);