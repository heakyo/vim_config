#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>

#include "tool.h"

static __u32 raw_readl(struct shannon_dev *dev, int dwoff)
{
	__u32 reg;

	assert(0 != dev->fd);

	if (lseek(dev->fd, dwoff * DW_SIZE, SEEK_SET) < 0)
		perror_exit("%s() lseek failed", __func__);

	if (read(dev->fd, &reg, DW_SIZE) != DW_SIZE)
		perror_exit("%s() read failed", __func__);

	return reg;
}

static __u32 ioread32(struct shannon_dev *dev, int dwoff)
{
	return le32_to_cpu(raw_readl(dev, dwoff));
}

static void raw_writel(struct shannon_dev *dev, __u32 value, int dwoff)
{
	assert(0 != dev->fd);

	if (lseek(dev->fd, dwoff * DW_SIZE, SEEK_SET) < 0)
		perror_exit("%s() lseek failed", __func__);

	if (write(dev->fd, &value, DW_SIZE) != DW_SIZE)
		perror_exit("%s() write failed", __func__);
}

static void iowrite32(struct shannon_dev *dev, __u32 value, int dwoff)
{
	__u32 v = cpu_to_le32(value);
	raw_writel(dev, v, dwoff);
}

static void multi_raw_readl(struct shannon_dev *dev, __u32 *des, int dwoff, int dwlen)
{
	assert(0 != dev->fd);

	if (lseek(dev->fd, dwoff * DW_SIZE, SEEK_SET) < 0)
		perror_exit("%s() lseek failed", __func__);

	if (read(dev->fd, des, dwlen * DW_SIZE) != dwlen * DW_SIZE)
		perror_exit("%s() multi read failed", __func__);
}

static void multi_ioread32(struct shannon_dev *dev, __u32 *des, int dwoff, int dwlen)
{
	int i;

	assert(0 != dev->fd);

	if (lseek(dev->fd, dwoff * DW_SIZE, SEEK_SET) < 0)
		perror_exit("%s() lseek failed", __func__);

	if (read(dev->fd, des, dwlen * DW_SIZE) != dwlen * DW_SIZE)
		perror_exit("%s() multi read failed", __func__);

	for (i = 0; i < dwlen; i++)
		le32_to_cpus(&des[i]);
}

static void multi_raw_writel(struct shannon_dev *dev, __u32 *src, int dwoff, int dwlen)
{
	assert(0 != dev->fd);

	if (lseek(dev->fd, dwoff * DW_SIZE, SEEK_SET) < 0)
		perror_exit("%s() lseek failed", __func__);

	if (write(dev->fd, src, dwlen * DW_SIZE) != dwlen * DW_SIZE)
		perror_exit("%s() multi write failed", __func__);
}

static void multi_iowrite32(struct shannon_dev *dev, __u32 *src, int dwoff, int dwlen)
{
	int i;
	__u32 *v = malloc(dwlen * DW_SIZE);

	assert(v != NULL);
	assert(0 != dev->fd);

	if (lseek(dev->fd, dwoff * DW_SIZE, SEEK_SET) < 0)
		perror_exit("%s() lseek failed", __func__);

	for (i = 0; i < dwlen; i++)
		v[i] = cpu_to_le32(src[i]);

	if (write(dev->fd, v, dwlen * DW_SIZE) != dwlen * DW_SIZE)
		perror_exit("%s() multi write failed", __func__);

	free(v);
}

static void reset(struct shannon_dev *dev)
{
	__u32 reg;

	reg = dev->ioread32(dev, dev->cfgreg_dwoff + HW_cfg_flash);
	reg |= (1u << 24);
	dev->iowrite32(dev, reg, dev->cfgreg_dwoff + HW_cfg_flash);
	usleep(1000);
	reg &= (~(1u << 24));
	dev->iowrite32(dev, reg, dev->cfgreg_dwoff + HW_cfg_flash);
}

static __u32 ioread_lunreg(struct shannon_dev *dev, int lun, enum HW_lunreg dwoff)
{
	__u32 reg;
	int phylun = log2phy_lun(dev, lun);

	assert(0 != dev->fd);

	if (lseek(dev->fd, (dev->lunreg_dwoff + (phylun / dev->hw_nlun) * dev->lunreg_dwsize + dwoff) * DW_SIZE, SEEK_SET) < 0)
		perror_exit("%s() lseek failed", __func__);

	if (read(dev->fd, &reg, DW_SIZE) != DW_SIZE)
		perror_exit("%s() read failed", __func__);

	return le32_to_cpu(reg);
}

static void iowrite_lunreg(struct shannon_dev *dev, __u32 value, int lun, enum HW_lunreg dwoff)
{
	__u32 v = cpu_to_le32(value);
	int phylun = log2phy_lun(dev, lun);

	assert(0 != dev->fd);

	if (lseek(dev->fd, (dev->lunreg_dwoff + (phylun / dev->hw_nlun) * dev->lunreg_dwsize + dwoff) * DW_SIZE, SEEK_SET) < 0)
		perror_exit("%s() lseek failed", __func__);

	if (write(dev->fd, &v, DW_SIZE) != DW_SIZE)
		perror_exit("%s() read failed", __func__);
}

static __u32 ioread_buflunreg(struct shannon_dev *dev, int head, enum HW_lunreg dwoff)
{
	__u32 reg;

	assert(0 != dev->fd && head < 2);

	if (lseek(dev->fd, (dev->lunreg_dwoff + dev->hw_threads * dev->lunreg_dwsize +
				head * dev->lunreg_dwsize + dwoff) * DW_SIZE, SEEK_SET) < 0)
		perror_exit("%s() lseek failed", __func__);

	if (read(dev->fd, &reg, DW_SIZE) != DW_SIZE)
		perror_exit("%s() read failed", __func__);

	return le32_to_cpu(reg);
}

static void iowrite_buflunreg(struct shannon_dev *dev, __u32 value, int head, enum HW_lunreg dwoff)
{
	__u32 v = cpu_to_le32(value);
	assert(0 != dev->fd && head < 2);

	if (lseek(dev->fd, (dev->lunreg_dwoff + dev->hw_threads * dev->lunreg_dwsize +
				head * dev->lunreg_dwsize + dwoff) * DW_SIZE, SEEK_SET) < 0)
		perror_exit("%s() lseek failed", __func__);

	if (write(dev->fd, &v, DW_SIZE) != DW_SIZE)
		perror_exit("%s() read failed", __func__);
}

static void ioread_config(struct shannon_dev *dev)
{
	assert(0 != dev->fd);
	assert(NULL != dev->hw_config);

	if (lseek(dev->fd, dev->cfgreg_dwoff * DW_SIZE, SEEK_SET) < 0)
		perror_exit("%s() lseek failed", __func__);

	if (read(dev->fd, dev->hw_config, sizeof(*dev->hw_config)) != sizeof(*dev->hw_config))
		perror_exit("%s() read failed", __func__);

	le16_to_cpus(&dev->hw_config->hw_full_sector_nbyte);
	le16_to_cpus(&dev->hw_config->hw_full_page_nbyte);
	le16_to_cpus(&dev->hw_config->hw_codeword_nbyte);
	le32_to_cpus(&dev->hw_config->hw_dw6_srv);
}

static void iowrite_config(struct shannon_dev *dev)
{
	struct hw_config hwcfg;

	assert(0 != dev->fd);
	assert(NULL != dev->hw_config);

	if (lseek(dev->fd, dev->cfgreg_dwoff * DW_SIZE, SEEK_SET) < 0)
		perror_exit("%s() lseek failed", __func__);

	memcpy(&hwcfg, dev->hw_config, sizeof(hwcfg));
	cpu_to_le16s(&hwcfg.hw_full_sector_nbyte);
	cpu_to_le16s(&hwcfg.hw_full_page_nbyte);
	cpu_to_le16s(&hwcfg.hw_codeword_nbyte);
	cpu_to_le32s(&hwcfg.hw_dw6_srv);

	if (write(dev->fd, &hwcfg, sizeof(hwcfg)) != sizeof(hwcfg))
		perror_exit("%s() read failed", __func__);
}

static void get_mem(struct shannon_dev *dev, struct memory *mem)
{
	struct shannon_ioctl ioctl_data;

	assert(0 != dev->fd);

	INIT_LIST_HEAD(&mem->list);
	INIT_LIST_HEAD(&mem->glist);

	ioctl_data.size = mem->size;
	if (ioctl(dev->fd, SHANNONC_IOC_GM, &ioctl_data))
		perror_exit("%s() failed", __func__);

	mem->kernel_addr = ioctl_data.kernel_addr;
	mem->dma_addr = ioctl_data.dma_addr;

	list_add_tail(&mem->glist, &dev->mem_glisthead);
}

static void free_mem(struct shannon_dev *dev, struct memory *mem)
{
	struct shannon_ioctl ioctl_data;

	assert(0 != dev->fd);

	ioctl_data.size = mem->size;
	ioctl_data.kernel_addr = mem->kernel_addr;
	ioctl_data.dma_addr = mem->dma_addr;
	if (ioctl(dev->fd, SHANNONC_IOC_FM, &ioctl_data))
		perror_exit("%s() failed", __func__);

	list_del(&mem->glist);
}

static void read_mem(struct shannon_dev *dev, void *user_addr, void *kernel_addr, int size)
{
	struct shannon_ioctl ioctl_data;

	assert(0 != dev->fd);

	ioctl_data.size = size;
	ioctl_data.kernel_addr = kernel_addr;
	ioctl_data.user_addr = user_addr;
	if (ioctl(dev->fd, SHANNONC_IOC_RM, &ioctl_data))
		perror_exit("%s() failed", __func__);
	
}

static void write_mem(struct shannon_dev *dev, void *kernel_addr, void *user_addr, int size)
{
	struct shannon_ioctl ioctl_data;

	assert(0 != dev->fd);

	ioctl_data.size = size;
	ioctl_data.kernel_addr = kernel_addr;
	ioctl_data.user_addr = user_addr;
	if (ioctl(dev->fd, SHANNONC_IOC_WM, &ioctl_data))
		perror_exit("%s() failed", __func__);
}

static void deliver_userdata(struct shannon_dev *dev)
{
	struct shannon_ioctl ioctl_data;

	assert(0 != dev->fd);
	assert(1 == dev->init_done);

	ioctl_data.type = TYPE_FLASH;
	ioctl_data.user_addr = dev->flash;
	if (ioctl(dev->fd, SHANNONC_IOC_DK, &ioctl_data))
		perror_exit("%s() devliver flash to kernel failed", __func__);

	ioctl_data.type = TYPE_CONFIG;
	ioctl_data.user_addr = dev->config;
	if (ioctl(dev->fd, SHANNONC_IOC_DK, &ioctl_data))
		perror_exit("%s() devliver config to kernel failed", __func__);
}

static void do_direct_io(struct shannon_dev *dev, struct direct_io *dio)
{
	struct shannon_ioctl ioctl_data;

	assert(0 != dev->fd);
	assert(1 == dev->init_done);
	assert(NULL != dio);

	ioctl_data.user_addr = dio;
	if (ioctl(dev->fd, SHANNONC_IOC_DI, &ioctl_data))
		perror_exit("%s() failed", __func__);
}

static void get_dev_domains(struct shannon_dev *dev)
{
	struct shannon_ioctl ioctl_data;

	assert(0 != dev->fd);

	ioctl_data.user_addr = dev->domains;
	ioctl_data.size = sizeof(dev->domains);
	if (ioctl(dev->fd, SHANNONC_IOC_GD, &ioctl_data))
		perror_exit("%s() failed", __func__);
}
/*----------------------------------------------------------------------------------------------------------------------------------*/
/* misc functions */
static void clear_queue(struct shannon_dev *dev)
{
	void *buf;
	int lun, size;

	size = (dev->hw_sysinfo->hw_wrbuf_support & 0x0F) ? 4 * PAGE_SIZE : 2 * PAGE_SIZE;
	buf = zmalloc(size);
	if (NULL == buf) {
		printf("%s() malloc failed\n", __func__);
		return;
	}

	for (lun = 0; lun < dev->hw_luns / dev->hw_nlun; lun++)
		dev->write_mem(dev, dev->phythread_mem[lun].kernel_addr, buf, size);

	free(buf);
}

static void config_hardware(struct shannon_dev *dev)
{
	if (check_ifmode_match(dev))
		exit(EXIT_FAILURE);

	dev->ioread_config(dev);

	dev->hw_config->hw_ifmode_and_timing = dev->config->ifmode | (dev->config->async_cycles << 4);
	dev->hw_config->hw_page_nsector = dev->config->page_nsector - 1;
	dev->hw_config->hw_blk_npage = (dev->flash->npage / 64) -1;

	dev->hw_config->hw_sector_nbyte = dev->config->sector_size / 512;
	dev->hw_config->hw_sector_ncodeword = dev->config->sector_ncodeword;
	dev->hw_config->hw_full_sector_nbyte = dev->config->codeword_size * dev->config->sector_ncodeword;

	dev->hw_config->hw_plane_mask = (dev->config->nplane > 1) ? (dev->flash->plane_mask >> 6) : 0;
	dev->hw_config->hw_lun_mask = dev->config->lun_mask;
	dev->hw_config->hw_full_page_nbyte = dev->config->page_nsector * dev->config->sector_ncodeword * dev->config->codeword_size;

	dev->hw_config->hw_raid_enable &= ~0x01;
	dev->hw_config->hw_raid_enable |= (dev->config->raid_mode & 0x01);
	dev->hw_config->hw_chunk_nsector = dev->config->chunk_nsector - 1;
	dev->hw_config->hw_chunk_nbyte = dev->config->chunk_nsector * dev->config->sector_size / 512 - 1;

	dev->hw_config->hw_ecc_mode = dev->config->ecc_mode;
	dev->hw_config->hw_codeword_nbyte = dev->config->codeword_size;	// now it`s readonly register

	if (dev->power_budget)
		dev->hw_config->hw_power_budget = dev->power_budget;

	dev->iowrite_config(dev);
	usleep(1000);
	dev->ioread_config(dev);
}

static void restore_default_config(struct shannon_dev *dev)
{
	/* set registers to default value */
	dev->hw_config->hw_ifmode_and_timing = (4 << 4) | IFMODE_ASYNC;
	dev->hw_config->hw_page_nsector = 6;
	dev->hw_config->hw_blk_npage = 0;
	dev->hw_config->hw_reset &= ~0x01;

	dev->hw_config->hw_sector_nbyte = 1;
	dev->hw_config->hw_sector_ncodeword = 1;
	dev->hw_config->hw_full_sector_nbyte = 0x0228;

	dev->hw_config->hw_plane_mask = 0;
	dev->hw_config->hw_lun_mask = 0;
	dev->hw_config->hw_full_page_nbyte = 0x0F18;

	dev->hw_config->hw_raid_enable = ((dev->per_byte_disable << 4) | RAIDMODE_DISABLE);
	dev->hw_config->hw_chunk_nsector = 6;
	dev->hw_config->hw_chunk_nbyte = 6;

	dev->hw_config->hw_ecc_mode = ECCMODE_DISABLE;
	dev->hw_config->hw_codeword_nbyte = 0x0228;

	dev->hw_config->hw_int_delay = 5;
	dev->hw_config->max_ecc_limit = 0xF0;

	/* write to HW */
	dev->clear_queue(dev);
	dev->reset(dev);
	dev->iowrite_config(dev);
	usleep(1000);
	dev->ioread_config(dev);
}

/*----------------------------------------------------------------------------------------------------------------------------------*/
static int mixed_flash_check(struct shannon_dev *dev, int lun, struct usr_flash *flash)
{
	if (flash->vendor_id != dev->flash->vendor_id ||
			/* flash->nblk != dev->flash->nblk || */
			flash->npage != dev->flash->npage ||
			flash->page_size != dev->flash->page_size ||
			flash->oob_size != dev->flash->oob_size ||
			flash->nplane != dev->flash->nplane ||
			flash->plane_mask != dev->flash->plane_mask ||
			flash->ifmode != dev->flash->ifmode ||
			memcmp(&flash->factory_ivb, &dev->flash->factory_ivb, sizeof(flash->factory_ivb)) ||
			memcmp(&flash->drvsetting, &dev->flash->drvsetting, sizeof(flash->drvsetting))) {
		printf("mixed flash failure\n");
		printf("FlashID: "); pr_u8_array_noprefix(dev->flash->id.byteid, 8, 8);
		printf("FlashID: "); pr_u8_array_noprefix(flash->id.byteid, 8, 8);

		return 1;
	}

	if (flash->nblk > dev->flash->nblk) {
		printf("Lun 0 should be bigger capacity flash\n");
		return 1;
	} else if (flash->nblk < dev->flash->nblk) {
		dev->targetlun[lun].blk_hole_begin = flash->nblk;
		dev->targetlun[lun].blk_hole_count = dev->flash->nblk - flash->nblk;
		// printf("lun=%d hole_begin=%d hole_count=%d\n", lun, dev->targetlun[lun].blk_hole_begin, dev->targetlun[lun].blk_hole_count);
	}

	return 0;
}

static void check_target_lun(struct shannon_dev *dev)
{
	int lun;
	struct shannon_request *req, *tmp;
	struct list_head req_head;

	struct target_lun *tgtlun;;
	struct usr_flash flash;

	INIT_LIST_HEAD(&req_head);

	bzero(dev->lun_bitmap, sizeof(dev->lun_bitmap));

	for_dev_each_lun(dev, lun) {
		req = alloc_request(dev, sh_readid_cmd, lun, 0, 0, 0, 0);	// read flash id after reset flash
		if (NULL == req)
			malloc_failed_exit();
		list_add_tail(&req->list, &req_head);
	}

	list_for_each_entry(req, &req_head, list) {
		if (dev->submit_request(req))
			submit_failed_exit(req->lun);
	}

	for_dev_each_lun(dev, lun)
		update_cmdqueue(dev, lun);

	for_dev_each_lun(dev, lun) {
		if (poll_cmdqueue(dev, lun))
			poll_failed_exit(lun);
	}

	list_for_each_entry(req, &req_head, list) {
		tgtlun = &dev->targetlun[req->lun];

		tgtlun->loglun = req->lun;
		tgtlun->phylun = log2phy_lun(dev, tgtlun->loglun);
		memcpy(tgtlun->id.byteid, req->id, sizeof(tgtlun->id.byteid));

		bzero(&flash, sizeof(flash));
		if (flash_info(dev, tgtlun->id, &flash))
			continue;

		if (1 == dev->targetluns_support) {
			if (0x00 != flash.lun_mask) {
				printf("FATAL: FPGA firmware, 1-lun/CE; Flash, 2-luns/CE. FlashID: %02X %02X %02X\n",
					flash.vendor_id, flash.device_id, flash.feature_id);
				exit(EXIT_FAILURE);
			}
		} else if (2 == dev->targetluns_support) {
			if (0x00 == flash.lun_mask) {
				if (tgtlun->phylun % 2) {
					tgtlun->shadow = 1;
					memset(tgtlun->id.byteid, 0xAB, sizeof(tgtlun->id.byteid));
					set_bit(tgtlun->loglun, dev->lun_bitmap);
				}
			}
		} else {
			printf("invalid targetluns_support: %d\n", dev->targetluns_support);
			exit(EXIT_FAILURE);
		}

		// printf("lun-%d shadow-%d %08X %08X %08X\n", tgtlun->loglun, tgtlun->shadow, tgtlun->id[0], tgtlun->id[1], tgtlun->id[2]);

		if (mixed_flash_check(dev, req->lun, &flash))
			exit(EXIT_FAILURE);

		if (dev->config->lun_mask < flash.lun_mask)
			dev->config->lun_mask = flash.lun_mask;
	}

	list_for_each_entry_safe(req, tmp, &req_head, list) {
		list_del(&req->list);
		free_request(req);
	}
}

/*----------------------------------------------------------------------------------------------------------------------------------*/
struct shannon_dev *alloc_device(char *devname)
{
	int bar;
	struct shannon_dev *dev;
	struct shannon_ioctl ioctl_data;

	dev = zmalloc(sizeof(*dev));
	if (NULL == dev)
		goto out;
	dev->newlunmap = 1;
	INIT_LIST_HEAD(&dev->mem_glisthead);

	dev->sysreg_dwoff = 0;
	dev->cfgreg_dwoff = 0xc0;
	dev->lunreg_dwoff = 0x100;
	dev->lunreg_dwsize = 8;

	/* set dev internal functions */
	dev->ioread32		= ioread32;
	dev->iowrite32		= iowrite32;

	dev->raw_readl		= raw_readl;
	dev->raw_writel		= raw_writel;

	dev->multi_ioread32	= multi_ioread32;
	dev->multi_iowrite32	= multi_iowrite32;

	dev->multi_raw_readl	= multi_raw_readl;
	dev->multi_raw_writel	= multi_raw_writel;

	dev->ioread_lunreg	= ioread_lunreg;
	dev->iowrite_lunreg	= iowrite_lunreg;

	dev->ioread_buflunreg	= ioread_buflunreg;
	dev->iowrite_buflunreg	= iowrite_buflunreg;

	dev->ioread_config	= ioread_config;
	dev->iowrite_config	= iowrite_config;

	dev->get_mem		= get_mem;
	dev->free_mem		= free_mem;
	dev->read_mem		= read_mem;
	dev->write_mem		= write_mem;

	dev->deliver_userdata	= deliver_userdata;
	dev->do_direct_io	= do_direct_io;

	dev->reset		= reset;

	dev->clear_queue	= clear_queue;
	dev->config_hardware	= config_hardware;
	dev->submit_request	= submit_request;

	/* open device handle */
	strcpy(dev->name, devname);	// printf("device=%s\n", devname);
	dev->fd = open(dev->name, O_RDWR, 0666);
	if (dev->fd < 0) {
		print("Open device %s error", dev->name);
		perror(" ");
		goto free_dev_out;
	}

	/* get bar length */
	for (bar = 0; bar < 2; bar++) {
		ioctl_data.bar = bar;
		if (ioctl(dev->fd, SHANNONC_IOC_GB, &ioctl_data))
			perror_exit("%s() failed", __func__);
		dev->bar_dwlen[bar] = ioctl_data.size / DW_SIZE;
		// printf("BAR%d length: %d dw\n", bar, dev->bar_dwlen[bar]);
	}

	/* alloc and read hw_sysinfo registers */
	dev->hw_sysinfo = malloc(sizeof(*dev->hw_sysinfo));
	if (NULL == dev->hw_sysinfo)
		goto close_fd_out;
	if (lseek(dev->fd, dev->sysreg_dwoff * DW_SIZE, SEEK_SET) < 0)
		perror_exit("%s() read-hw_sysinfo lseek failed", __func__);
	if (read(dev->fd, dev->hw_sysinfo, sizeof(*dev->hw_sysinfo)) != sizeof(*dev->hw_sysinfo))
		perror_exit("%s() read-hw_sysinfo read failed", __func__);
	le32_to_cpus(&dev->hw_sysinfo->firmware_tag);
	le32_to_cpus(&dev->hw_sysinfo->dw_rsv[0]);
	le32_to_cpus(&dev->hw_sysinfo->dw_rsv[1]);
	le32_to_cpus(&dev->hw_sysinfo->dw_rsv[2]);
	dev->hw_nchannel = dev->hw_sysinfo->hw_nchannel;
	dev->hw_nthread = dev->hw_sysinfo->hw_nthread_nlun & 0x0F;
	dev->hw_nlun = ((dev->hw_sysinfo->hw_nthread_nlun >> 4) & 0x0F) + 1;
	dev->hw_threads = dev->hw_nchannel * dev->hw_nthread;
	dev->hw_luns = dev->hw_threads * dev->hw_nlun;
	dev->iowidth = ((dev->hw_sysinfo->hw_misc_1 >> 4) & 0x0F) + 1;

	if (dev->hw_sysinfo->hw_ecc_tmode & 0x02) {
		dev->tmode = 16;
	} else if (dev->hw_sysinfo->hw_ecc_tmode & 0x04) {
		dev->tmode = 35;
	} else if (dev->hw_sysinfo->hw_ecc_tmode & 0x08) {
		dev->tmode = 50;
	} else if (dev->hw_sysinfo->hw_ecc_tmode & 0x10) {
		dev->tmode = 58;
	} else {
		printf("Don't support this ECC Tmode: 0x%02X\n", dev->hw_sysinfo->hw_ecc_tmode);
		exit(EXIT_FAILURE);
	}

	if (dev->hw_sysinfo->hw_ecc_mode & 0x01) {
		dev->mmode = 13;
	} else if (dev->hw_sysinfo->hw_ecc_mode & 0x02) {
		dev->mmode = 14;
	} else {
		printf("Don't support this ECC mode: 0x%02X\n", dev->hw_sysinfo->hw_ecc_mode);
		exit(EXIT_FAILURE);
	}

	dev->targetluns_support = ((dev->hw_sysinfo->hw_if_support & CE_NLUN_MASK) >> CE_NLUN_SHIFT) + 1;

	/* alloc and read lun_mem */
	dev->phythread_mem = malloc(sizeof(*dev->phythread_mem) * dev->hw_threads);
	if (NULL == dev->phythread_mem)
		goto free_sys_out;
	ioctl_data.size = sizeof(*dev->phythread_mem) * dev->hw_threads;
	ioctl_data.user_addr = dev->phythread_mem;
	if (ioctl(dev->fd, SHANNONC_IOC_GF, &ioctl_data))
		perror_exit("%s() failed", __func__);

	/* alloc hw_config registers */
	dev->hw_config = malloc(sizeof(*dev->hw_config));
	if (NULL == dev->hw_config)
		goto free_threadmem_out;
	dev->ioread_config(dev);

	/* alloc flash and config buffer */
	dev->flash = zmalloc(sizeof(*dev->flash));
	if (NULL == dev->flash)
		goto free_cfg_out;
	dev->flash->success_mask = FLASH_SUCCESS_MASK;
	dev->flash->success_status = FLASH_SUCCESS_STATUS;

	dev->config = zmalloc(sizeof(*dev->config));
	if (NULL == dev->config)
		goto free_flash_out;

	/* alloc bakup for flash and flash */
	dev->flash_bakup = zmalloc(sizeof(*dev->flash_bakup));
	if (NULL == dev->flash_bakup)
		goto free_config_out;

	dev->config_bakup = zmalloc(sizeof(*dev->config_bakup));
	if (NULL == dev->config_bakup)
		goto free_flash_bakup_out;

	/* alloc MBR buffer */
	dev->mbr = zmalloc(sizeof(*dev->mbr));
	if (NULL == dev->mbr)
		goto free_config_bakup_out;

	dev->inherent_mbr = zmalloc(sizeof(*dev->mbr));
	if (NULL == dev->inherent_mbr)
		goto free_mbr_out;

	get_dev_domains(dev);

	/* get serial number */
	shannon_read_nor(dev, &dev->norinfo, NORFLASH_INFO_ADDR, sizeof(dev->norinfo));
	if (dev->norinfo.magic_number != NORFLASH_INFO_MAGIC) {
		sprintf(dev->norinfo.service_tag, "missing");
		sprintf(dev->norinfo.model_id, "missing");
	}

	/* return OK */
	return dev;

free_mbr_out:
	free(dev->mbr);
free_config_bakup_out:
	free(dev->config_bakup);
free_flash_bakup_out:
	free(dev->flash_bakup);
free_config_out:
	free(dev->config);
free_flash_out:
	free(dev->flash);
free_cfg_out:
	free(dev->hw_config);
free_threadmem_out:
	free(dev->phythread_mem);
free_sys_out:
	free(dev->hw_sysinfo);
close_fd_out:
	close(dev->fd);
free_dev_out:
	free(dev);
out:
	return NULL;
}

int init_device(struct shannon_dev *dev)
{
	int i, phytr, lun, head;
	struct shannon_thread *thread;

	assert(NULL != dev);

	/* set flash interface clock */
	if (dev->flash_ifclock) {
		dev->iowrite32(dev, 0x0, 0x18);
		dev->iowrite32(dev, 0x80000000|dev->flash_ifclock, 0x18);
		usleep(1000);
	}

	/* do some HW check */
	if (1 != dev->iowidth && 2 != dev->iowidth) {
		printf("FATAL: HARDWARE is neither 8bit nor 16bit!\n");
		exit(EXIT_FAILURE);
	}

	/* parse flash need use cmdqueue to read flash id */
	dev->config->nchannel = dev->hw_nchannel;	// for parse_flash() to use log2phy_lun()
	dev->config->nthread = dev->hw_nthread;
	dev->config->nlun = dev->hw_nlun;
	dev->config->threads = dev->config->nchannel * dev->config->nthread;
	dev->config->luns = dev->config->threads * dev->config->nlun;
	restore_default_config(dev);
	if (parse_flash(dev)) {
		printf("ERR: parse flash failed\n");
		return ERR;
	}
	memcpy(dev->flash_bakup, dev->flash, sizeof(*dev->flash_bakup));
	dev->maxplanes = (((dev->hw_sysinfo->hw_misc_1 & 0x0F) + 1) * 16 * 1024) / dev->flash->page_size;

	dev->config->nchannel = 0;			// restore default value 0
	dev->config->nthread = 0;
	dev->config->nlun = 0;
	dev->config->threads = 0;
	dev->config->luns = 0;

	/* parse config from file 'config' or interactive input */
	if (parse_config(dev)) {
		printf("parse config failed\n");
		return ERR;
	}

	dev->targetlun = malloc(dev->config->luns * sizeof(*dev->targetlun));
	if (NULL == dev->targetlun)
		exit(EXIT_FAILURE);
	bzero(dev->targetlun, dev->config->luns * sizeof(*dev->targetlun));

	if (check_ifmode_match(dev))
		exit(EXIT_FAILURE);

	/* alloc some buffer depending on dev->config. XXX: re_init_device() should realloc them */
	dev->padding_buffer = malloc(dev->config->chunk_ndata);
	if (NULL == dev->padding_buffer)
		return ALLOCMEM_FAILED;
	for (i = 0; i < dev->config->chunk_nsector; i++)
		memset(dev->padding_buffer + i * dev->config->sector_size, i, dev->config->sector_size);

	/* super block array */
	dev->sb = zmalloc(sizeof(*dev->sb) * dev->flash->nblk / dev->config->nplane);
	if (NULL == dev->sb)
		return ALLOCMEM_FAILED;
	for (i = 0; i < dev->flash->nblk / dev->config->nplane; i++) {
		dev->sb[i].idx = i;

		dev->sb[i].sb_luninfo.luns = dev->config->luns;
		dev->sb[i].sb_luninfo.ndatalun = dev->config->luns - 1;
		dev->sb[i].sb_luninfo.parity_lun = dev->config->luns - 1;
	}

	/* alloc and init dev->shannon_thread */
	dev->thread = zmalloc(dev->hw_threads * sizeof(*dev->thread));
	if (NULL == dev->thread)
		return ALLOCMEM_FAILED;

	for (phytr = 0; phytr < dev->hw_threads; phytr++) {
		thread 			= &dev->thread[phytr];
		thread->phythread_idx	= phytr;
		thread->cmdhead		= 0;
		thread->cmdempty	= PAGE_SIZE - 8;
		thread->req_count	= 0;

		thread->cmdmem.kernel_addr	= dev->phythread_mem[phytr].kernel_addr;
		thread->cmdmem.dma_addr		= dev->phythread_mem[phytr].dma_addr;
		thread->cmpmem.kernel_addr	= dev->phythread_mem[phytr].kernel_addr + PAGE_SIZE;
		thread->cmpmem.dma_addr		= dev->phythread_mem[phytr].dma_addr + PAGE_SIZE;
	}

	/* alloc and init dev->shannon_lun */
	dev->lun = zmalloc(dev->config->luns * sizeof(*dev->lun));
	if (NULL == dev->lun)
		return ALLOCMEM_FAILED;

	dev->clear_queue(dev);
	dev->reset(dev);

	for (lun = 0; lun < dev->config->luns; lun++) {
		INIT_LIST_HEAD(&dev->lun[lun].req_listhead);

		dev->lun[lun].loglun	= lun;
		dev->lun[lun].channel	= get_phychannel(dev, dev->lun[lun].loglun);
		dev->lun[lun].phylun	= log2phy_lun(dev, dev->lun[lun].loglun);
		dev->lun[lun].thread	= &dev->thread[dev->lun[lun].phylun / dev->hw_nlun];
		dev->lun[lun].head	= dev->lun[lun].tail = -1;

		dev->iowrite_lunreg(dev, U64_LOW_32(dev->lun[lun].thread->cmdmem.dma_addr), lun, HW_cmdq_pte_lo);
		dev->iowrite_lunreg(dev, U64_HIGH_32(dev->lun[lun].thread->cmdmem.dma_addr), lun, HW_cmdq_pte_hi);
		dev->iowrite_lunreg(dev, U64_LOW_32(dev->lun[lun].thread->cmpmem.dma_addr), lun, HW_cmpq_pte_lo);
		dev->iowrite_lunreg(dev, U64_HIGH_32(dev->lun[lun].thread->cmpmem.dma_addr), lun, HW_cmpq_pte_hi);
	}

	/* alloc dev->shannon_bufhead if hw support buffer write/read */
	if (dev->hw_sysinfo->hw_wrbuf_support & 0x0F) {
		dev->bufhead = zmalloc(2 * sizeof(*dev->bufhead));
		if (NULL == dev->bufhead)
			return ALLOCMEM_FAILED;

		for (head = 0; head < 2; head++) {
			dev->bufhead[head].cmdhead = 0;
			dev->bufhead[head].cmdempty = PAGE_SIZE - 8;
			dev->bufhead[head].cmdmem.kernel_addr = dev->phythread_mem[head].kernel_addr + 2 * PAGE_SIZE;
			dev->bufhead[head].cmdmem.dma_addr = dev->phythread_mem[head].dma_addr + 2 * PAGE_SIZE;
			dev->bufhead[head].cmpmem.kernel_addr = dev->phythread_mem[head].kernel_addr + 3 * PAGE_SIZE;
			dev->bufhead[head].cmpmem.dma_addr = dev->phythread_mem[head].dma_addr + 3 * PAGE_SIZE;
			INIT_LIST_HEAD(&dev->bufhead[head].req_listhead);

			dev->iowrite_buflunreg(dev, U64_LOW_32(dev->bufhead[head].cmdmem.dma_addr), head, HW_cmdq_pte_lo);
			dev->iowrite_buflunreg(dev, U64_HIGH_32(dev->bufhead[head].cmdmem.dma_addr), head, HW_cmdq_pte_hi);
			dev->iowrite_buflunreg(dev, U64_LOW_32(dev->bufhead[head].cmpmem.dma_addr), head, HW_cmpq_pte_lo);
			dev->iowrite_buflunreg(dev, U64_HIGH_32(dev->bufhead[head].cmpmem.dma_addr), head, HW_cmpq_pte_hi);
		}
	}

	check_target_lun(dev);
	memcpy(dev->config_bakup, dev->config, sizeof(*dev->config_bakup));

	/* alloc dummy memory */
	dev->dummy_mem.size = 4096;
	dev->get_mem(dev, &dev->dummy_mem);

	/*config hardware*/
	dev->ifmode = dev->config->ifmode = IFMODE_ASYNC;
	dev->config_hardware(dev);
	dev->init_done = 1;

	switch (dev->config_bakup->ifmode) {
	case IFMODE_ASYNC:
		/* do nothing */
		break;
	case IFMODE_SYNC:
		if (shannon_super_sync(dev)) {
			printf("set super sync fail\n");
			exit(EXIT_FAILURE);
		}
		break;
	case IFMODE_TOGGLE:
		if (shannon_super_toggle(dev)) {
			printf("set super toggle fail\n");
			exit(EXIT_FAILURE);
		}
		break;
	default:
		printf("Invalid flash ifmode %d\n", dev->config_bakup->ifmode);
		exit(EXIT_FAILURE);
	}

	/* sync mode set feature at async if, but toggle flash should do this at toggle if */
	if (dev->flash->drvsetting.datanum) {
		if (!strncmp(dev->subsystemid, "0040", 4) && 0x040A517A93953C45UL == dev->flash->id.longid) {
			//printf("THIS IS 8639 Sandisk 128GIR\n");
			u8 odt[4] = {0x10, 0, 0, 0};
			dev->flash->drvsetting.data[2] = 0x04;
			if (super_set_feature(dev, 0xEF, 0x02, odt, sizeof(odt))) {
				printf("Set ODT feature error\n");
				exit(EXIT_FAILURE);
			}
		}

		if (super_set_feature(dev, dev->flash->drvsetting.data[0], dev->flash->drvsetting.data[1],
			dev->flash->drvsetting.data + 2, dev->flash->drvsetting.datanum - 2)) {
			printf("Set feature drvmode failed");
			return ERR;
		}
	}

	write_advanced_read_microcode(dev);
	return 0;
}

/*
 * when change sector_size, ecc_mode, raid_mode and so on, re_init_device maybe needed.
 * XXX: this function can`t be used for change ifmode, please use shannon_super_async() or shannon_super_sync()
 */
int re_init_device(struct shannon_dev *dev)
{
	int i;

	assert(dev->init_done == 1);

	if (calculate_indirect_config(dev)) {
		memcpy(dev->flash, dev->flash_bakup, sizeof(*dev->flash));
		memcpy(dev->config, dev->config_bakup, sizeof(*dev->config));
		return ERR;
	}

	/* re-alloc some buffer needed */
	free(dev->padding_buffer);
	dev->padding_buffer = malloc(dev->config->chunk_ndata);
	if (NULL == dev->padding_buffer)
		return ALLOCMEM_FAILED;
	for (i = 0; i < dev->config->chunk_nsector; i++)
		memset(dev->padding_buffer + i * dev->config->sector_size, i, dev->config->sector_size);

	dev->config_hardware(dev);
	return 0;
}

void free_device(struct shannon_dev *dev)
{
	/* alloc not in alloc_device*/
	if (dev->bufhead) free(dev->bufhead);
	if (dev->lun) free(dev->lun);
	if (dev->sb) free(dev->sb);
	if (dev->thread) free(dev->thread);
	if (dev->padding_buffer) free(dev->padding_buffer);
	if (dev->targetlun) free(dev->targetlun);
	if (dev->dummy_mem.size) free_mem(dev, &dev->dummy_mem);
	if (dev->exitlog) fclose(dev->exitlog);

	/* alloc in alloc_device */
	free(dev->inherent_mbr);
	free(dev->mbr);
	free(dev->config_bakup);
	free(dev->flash_bakup);
	free(dev->config);
	free(dev->flash);
	free(dev->hw_config);
	free(dev->phythread_mem);
	free(dev->hw_sysinfo);
	close(dev->fd);
	free(dev);
}
/*----------------------------------------------------------------------------------------------------------------------------------*/
