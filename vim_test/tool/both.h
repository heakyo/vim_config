#ifndef __BOTH_H__
#define __BOTH_H__

// XXX: driver and app both use this file

/*-----------------------------------------------------------------------------------------------------------------------------*/
#define	SHANNONC_IOC_MAGIC	'S'
#define	SHANNONC_IOC_MAXNR	16

#define	SHANNONC_IOC_GM	_IO(SHANNONC_IOC_MAGIC, 1)	/* get mem */
#define	SHANNONC_IOC_FM	_IO(SHANNONC_IOC_MAGIC, 2)	/* free mem */
#define	SHANNONC_IOC_RM	_IO(SHANNONC_IOC_MAGIC, 3)	/* read mem */
#define	SHANNONC_IOC_WM	_IO(SHANNONC_IOC_MAGIC, 4)	/* write mem */
#define	SHANNONC_IOC_GF	_IO(SHANNONC_IOC_MAGIC, 5)	/* get fixed lun_mem */
#define	SHANNONC_IOC_DK	_IO(SHANNONC_IOC_MAGIC, 6)	/* deliver data to kernel space from user: flash and config struct */
#define	SHANNONC_IOC_DI	_IO(SHANNONC_IOC_MAGIC, 7)	/* direct IO: start/poll/stop erase, write or read */
#define	SHANNONC_IOC_GB	_IO(SHANNONC_IOC_MAGIC, 8)	/* get bar len */
#define	SHANNONC_IOC_GD	_IO(SHANNONC_IOC_MAGIC, 9)	/* get device domains info */

#define	DIRECT_IO_START	0x10
#define	DIRECT_IO_POLL	0x11
#define	DIRECT_IO_STOP	0x12

/*-----------------------------------------------------------------------------------------------------------------------------*/
// Hareware registers
enum HW_sysreg {
	HW_sys_general_info,
	HW_sys_raid_info,
	HW_sys_ecc_info,
	HW_sys_wrbuf_info,
};

enum HW_cfgreg {
	HW_cfg_flash,
	HW_cfg_secto,
	HW_cfg_chunk,
	HW_cfg_raid,
	HW_cfg_ecc,
	HW_cfg_misc,
	HW_cfg_seed,
	HW_cfg_mask,
};

enum HW_lunreg {
	HW_cmdq_pte_lo,
	HW_cmdq_pte_hi,		// command queue pte hi32
	HW_cmpq_pte_lo,		// completion queue pte lo32
	HW_cmpq_pte_hi,
	HW_cmdq_head,		// command queue head
	HW_cmpq_head,		// completion queue head, readonly
	HW_cmdq_tail,		// command queue tail, readonly
	HW_control_staus,	// lun control and status
};

struct hw_sysinfo {
	/* dw 0 */
	__u8 hw_version;		// hareware version
#define	CE_NLUN_MASK	0xF0
#define	CE_NLUN_SHIFT	4
	__u8 hw_if_support;		// which ifmode supported
	__u8 hw_nchannel;		// number of channels supported
	__u8 hw_nthread_nlun;		// number of threads[0:3] and number of luns minus 1[4:7]

	/* dw 1 */
	__u8 hw_raid_support;		// raid support
	__u8 hw_nraid_head;		// number of concurrent heads in RAID
	__u8 hw_dbuf_raid_support;	// double buffer in RAID parity
	__u8 hw_misc_1;

	/* dw 2 */
	__u8 hw_ecc_mode;		// ecc mode
	__u8 hw_ecc_tmode;		// ecc mode
	__u8 hw_dw2_rsv[2];

	/* dw 3 */
	__u8 hw_wrbuf_support;		// write buffer support
	__u8 hw_nwrbuf;			// number of buffers
	__u8 hw_dw3_rsv[2];

	/* dw 4-dw 6 */
	__u32 dw_rsv[3];

	/* dw 7 */
	__u32 firmware_tag;
};

struct hw_config {
	/* dw 0 */
	__u8 hw_ifmode_and_timing;	// [0:3]ifmode and [7:4]timing cycles
	__u8 hw_page_nsector;		// number of sectors per page minus 1
	__u8 hw_blk_npage;		// number of page per block divided by 64 minus 1
	__u8 hw_reset;			// reset hw

	/* dw 1 */
	__u8 hw_sector_nbyte;		// bytes per sector, 1-512 2-1024 4-2048 8-4096
	__u8 hw_sector_ncodeword;	// number of ecc codeword per sector
	__u16 hw_full_sector_nbyte;	// number of byte per full sector

	/* dw 2 */
	__u8 hw_plane_mask;		// plane mask shifted by 6
	__u8 hw_lun_mask;
	__u16 hw_full_page_nbyte;	// number of byte per full page

	/* dw 3 */
	__u8 hw_raid_enable;		// 0-enable, 1-disable
	__u8 hw_chunk_nsector;		// number of sector per chunk minus 1
	__u8 hw_chunk_nbyte;		// number of byte per data chunk divided by 512 minus 1
	__u8 hw_dw3_rsv;

	/* dw 4 */
	__u8 hw_ecc_mode;		// 0-no bypass; 1-bypass ecc/crc/scrambler; 2-bypass scrambler only
	__u8 hw_ecc_power;		// ecc power of 1st stage
	__u16 hw_codeword_nbyte;	// number of bytes per ecc codeword, readonly

	/* dw 5 */
	__u8 hw_int_delay;		// interrput delay in us
	__u8 hw_dw5_rsv;
	__u8 hw_power_budget;
	__u8 hw_seed_in3432;

	/* dw 6 */
	__u32 hw_dw6_srv;

	/* dw 7 */
	__u8 user_mask;
	__u8 throttle_limit;
	__u8 max_ecc_limit;
	__u8 dam_read_limit;
};

/*-----------------------------------------------------------------------------------------------------------------------------*/
union flash_id {
	struct {
		__u8 vendor_id;
		__u8 device_id;
		__u8 feature_id;
		__u8 others[5];
	};
	__u8 byteid[8];
	__u64 longid;
};

struct usr_flash {
	char name[32];
	union flash_id id;
	int vendor_id;
	int device_id;
	int feature_id;

	int nblk;		// block number of flash
	int npage;		// page number per block
	int page_size_shift;
	int page_size;
	int oob_size;
	int entire_page_size;	// page_size + oob_size

	int lun_mask;
	int nplane;		// plane number
	int plane_mask;

	int ifmode;		// flash ifmode

	struct __factory_ivb {
		int row;	// factory invalid block mark`s row off in block
		int lo_col;	// factory invalid block mark`s low and high column off in page
		int hi_col;
	} factory_ivb[8];

	struct __drvsetting {
		__u8 data[16];
		__u8 datanum;
	} drvsetting;

	int success_status;	// successful status after manipulate
	int success_mask;
};

struct usr_config {
	/* directly get */
	int nchannel;		// cfg channel number
	int nthread;		// cfg thread number per channel
	int nlun;		// cfg lun number per thread

	int sector_size_shift;
	int sector_ncodeword;	// ecc codeword num per sector

	int optimal_ncodeword;
	int optimal_codeword_size;
	int optimal_page_nsector;

	int user_sector_size_shift;

	int nplane;		// configed plane number, must <= flash->nplane
	int nplane_used;	// maybe hw configed as two plane but we use single plane to manipulate flash

	int lun_mask;		// the value write to hw_config
	u32 drvmode;		// set falsh drive mode feature
	int ifmode;		// interface mode, 0:async ONFI; 1:toggle; 2:unused; 3:sync ONFI
	int async_cycles;
	int ecc_mode;
	int raid_mode;
	int overprovision_rate;

	int vendor_id;
	u32 hardware_version;
	u32 software_version;

	/* indirectly calculate */
	int threads;		// nchannel * nthread
	int luns;		// nchannel * nthread * nlun;
	int sector_size;	// 1 << sector_size_shit
	int codeword_size;
	int full_sector_size;
	int page_nsector;
	int chunk_nsector;

	int ndata;		// page data and metadata nbyte
	int nmeta;
	int chunk_ndata;	// chunk data and metadata nbyte
	int chunk_nmeta;
};

/*-----------------------------------------------------------------------------------------------------------------------------*/
struct thread_mem {
	void *kernel_addr;
	dma_addr_t dma_addr;
};

#define	TYPE_FLASH	0x111
#define	TYPE_CONFIG	0x112

struct shannon_ioctl {
	union {
		int size;
		int type;
		int bar;
	};
	void *kernel_addr;
	dma_addr_t dma_addr;
	void *user_addr;
};

struct direct_io {
	int type;		/* DIRECT_IO_START, DIRECT_IO_POLL or DIRECT_IO_STOP */
	int opcode;
	int head;
	int nblk;
	int nplane;
	int npage;
	int nsector;
	int sector_size;
	unsigned long phylun_bitmap[8];
	unsigned long done_phylun_bitmap[8];
	unsigned long done_ppa;
	struct timeval tv;	/* deliver timeval to user space */

	/* kernel internal */
	int cmdlen;
	int ncacheread;
	int limitppa;
};

/*-----------------------------------------------------------------------------------------------------------------------------*/
enum shannon_cmd {
	sh_preread_cmd		= 0x00,
	sh_cacheread_cmd	= 0x01,
	sh_last_cacheread_cmd	= 0x02,
	sh_erase_cmd		= 0x10,
	sh_write_cmd		= 0x20,
	sh_bufwrite_cmd		= 0x30,
	sh_bufread_cmd		= 0x32,
	sh_readid_cmd		= 0x40,
	sh_writereg_cmd		= 0x50,
	sh_raidwrite_cmd	= 0x80,
	sh_raidinit_cmd		= 0x90,
	sh_reset_cmd		= 0xF0,
	sh_read_cmd		= 0xF8,	/* not hardware cmd but soft cmd*/
	sh_cacheread_adv_cmd	= 0x08,
};

struct sh_reset {
	__u8 opcode;
	__u8 rsv[6];
	__u8 lun;
};

struct sh_readid {
	__u8 opcode;
	__u8 rsv[3];
	__u8 addr;
	__u8 nbyte;
	__u8 cmd;
	__u8 lun;
};

struct sh_erase {
	__u8 opcode;
	__u8 rsv_1;
	__u8 rsv_2;
#define	SH_ERASE_PLANE_SHIFT	4
	__u8 head;

	__u32 ppa;
};

struct sh_write {
	__u8 opcode;
	__u8 rsv_1;
	__u8 rsv_2;
#define	SH_WRITE_PLANE_SHIFT	4
#define	SH_WRITE_DUMMY_SHIFT	5
	__u8 head;

	__u32 ppa;

	struct sh_write_sector {
		__u64 pte;
		__u64 metadata;
	} sector[0];
};

struct sh_preread {
	__u8 opcode;
	__u8 rsv_1;
	__u8 rsv_2;
#define	SH_READ_PLANE_SHIFT	4
	__u8 head;

	__u32 ppa;
};

struct sh_cacheread {
	__u8 opcode;
	__u8 nsector;		/* nsector -1 */
	__u8 bsector;		/* begin sector */
#define	SH_READ_PLANE_SHIFT	4
	__u8 head;

	__u32 ppa;
	__u64 pte[0];
};

struct sh_bufwrite {
	__u8 opcode;
	__u8 lun;
	__u8 bsector;
	__u8 head;

	__u32 ppa;
	__u64 pte;
	__u64 metadata;
};

struct sh_raidinit {
	__u8 opcode;
	__u8 ndatalun;
	__u8 rsv;
	__u8 head;

	__u32 ppa;
};

struct sh_raidwrite {
	__u8 opcode;
	__u8 rsv_1;
	__u8 rsv_2;
#define	SH_RAIDWRITE_PLANE_SHIFT	4
#define	SH_RAIDWRITE_DUMMY_SHIFT	5
	__u8 head;

	__u32 ppa;
};

struct sh_writereg {
        __u8 opcode;
        __u8 rsv[3];
        __u8 addr;
        __u8 nbyte;
        __u8 cmd;
        __u8 padding;
        __u8 data[0];
};
/*-----------------------------------------------------------------------------------------------------------------------------*/
#endif /* __BOTH_H__ */
