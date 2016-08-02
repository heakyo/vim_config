/* Definitions for Shannon hardware/software interface.
 * Copyright (c) 2012, Shannon technology
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef SHANNON_MBR_H_
#define SHANNON_MBR_H_

#define MBR_EBLOCKS		4           /* reserved MBR erase blocks. */
#define MBR_WATERMARK		0xa5a5a5a5
#define MBR_LOGICB_SHIFT	9
#define CURRENT_MBR_VERSION	0x5100

struct shannon_mbr {
	char id[32];
	u64 mbr_version;
	u64 hardware_version;
	u64 software_version;
	u64 nand_manufacture;
	u64 nand_id;
	u64 capacity;

	u32 lun_amount;
	u32 eblocks_in_lun;
	u32 pages_in_eblock;
	u32 nand_page_shift;
	u32 oob_size;
	u32 logicb_shift;
	u32 plane_order;
	u32 cfg_nchannel;	// cfg_channels
	u32 cfg_nthread;	// lunset_in_channel
	u32 cfg_nlun;		// cfg_lun_in_lunset

	u32 init_hot_sblk;
	u32 init_cold_sblk;

	u16 interrupt_delay;	/* default to 1 */
	u8 ecc_codewords_in_logicb;
	u8 ecc_correction_power;
	u32 history_erase_count;

	u64 power_cycle_count;
	u64 power_on_seconds;	/* updated in each reformat progress */
	u64 host_write_sectors;
	u64 total_write_sectors;
	u64 host_read_sectors;

	u32 flash_drvmode;	/* output_drive_strength + 1; so 0 is invalid. */
	u8 luns_per_ce_mask;
	u8 lun_map_mode;
	u16 raid_stripes;

	u64 bad_phy_lun_map[8];
	u32 max_pages_in_eblock;
	u32 user_logicb_shift;

#define PRIORITIZE_WRITE        0x0001
#define ATOMIC_WRITE            0x0002
	u64 feature_flags;
	u8 power_budget;

	u8 dma_max_read_limit;
	u16 clk;
	u32 max_outstanding_bios;
};

#endif /* SHANNON_MBR_H_ */
