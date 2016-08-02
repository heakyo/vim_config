#ifndef __SHANNON_TOOL_H__
#define	__SHANNON_TOOL_H__
/*-----------------------------------------------------------------------------------------------------------------------------*/
#include <getopt.h>
#include <time.h>
#include <sys/time.h>
#include <signal.h>
#include "list.h"
#include "graphics.h"

#if DMA_ADDR_LENGTH == 4
typedef unsigned int dma_addr_t;
#define	U64_LOW_32(x)	((__u32)((x)&0xFFFFFFFF))	//NOTE: U64_LOW_32/U64_HIGH_32 only used for dma address
#define	U64_HIGH_32(x)	0
#elif DMA_ADDR_LENGTH == 8
typedef unsigned long long dma_addr_t;
#define	U64_LOW_32(x)	((__u32)((x)&0xFFFFFFFF))
#define	U64_HIGH_32(x)	((__u32)((x)>>32))
#else
#error Error DMA_ADDR_LENGTH
#endif

#ifndef __AARCH64__
typedef unsigned char __u8;
typedef unsigned short __u16;
typedef unsigned int __u32;
typedef unsigned long __u64;
#endif

typedef unsigned long ulong;

typedef __u64 u64;
typedef __u32 u32;
typedef __u16 u16;
typedef __u8 u8;

#include "both.h"
#include "shannon-mbr.h"
#include "dev-type.h"

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

#define	OK			0
#define	ERR			-1
#define	NO_CMDQUEUE_ROOM	0x80
#define	FAILED_FLASH_STATUS	0x81
#define	OVERFLOW_ADDR		0x82
#define	ALLOCMEM_FAILED		0x83

#define	DW_SIZE			4
#define	QW_SIZE			8

#define	PAGE_SIZE		4096
#define	DEFAULT_DEVNAME		"/dev/shannon_cdev"

#define	METADATA_SIZE		8

#define	HEAD_MASK		0x0F
#define	HEAD0			0
#define	HEAD1			1
#define	INDEP_HEAD		2	/* independed head whithout raid sync */

#define	IFMODE_MASK		0x0000000F
#define	IFMODE_ASYNC		0
#define	IFMODE_SYNC		3
#define	IFMODE_TOGGLE		1

#define	FLASH_IFMODE_ASYNC	0x2012
#define	FLASH_IFMODE_SYNC	0x2013
#define	FLASH_IFMODE_TOGGLE	0x2014

#define	ECCMODE_ENABLE			0	/* enable ecc, crc and scrambler */
#define	ECCMODE_DISABLE			1	/* bypass ecc, crc and scrambler */
#define	ECCMODE_BYPASS_SCRAMBLER_ONLY	2	/* only bypass scrambler */

#define	RAIDMODE_DISABLE		0
#define	RAIDMODE_ENABLE			1

#define	RUNCMDQ_US_TIMEOUT		8000

#define	FLASH_SUCCESS_MASK	0x41
#define	FLASH_SUCCESS_STATUS	0x40

#define	BITMAP_BBT_MAGIC	0xF2F5F7F1F6F3F8F4UL
#define	MAX_BADBLK_COUNT	4090
/*-----------------------------------------------------------------------------------------------------------------------------*/
struct memory {
	int size;
	void *kernel_addr;
	dma_addr_t dma_addr;
	struct list_head list;
	struct list_head glist;
};

/*-----------------------------------------------------------------------------------------------------------------------------*/
struct shannon_thread {
	int phythread_idx;
	int cmdhead;		/* byte unit */
	int cmdempty;
	int req_count;
	struct thread_mem cmdmem;
	struct thread_mem cmpmem;
	struct list_head req_listhead;
};

struct shannon_lun {
	int head;		/* first cmd head of this lun */
	int tail;		/* last cmd tail of this lun */

	int channel;
	int phylun;
	int loglun;
	struct shannon_thread *thread;

	struct list_head req_listhead;
};

struct shannon_bufhead {
	int cmdhead;
	int cmdempty;
	struct thread_mem cmdmem;
	struct thread_mem cmpmem;
	struct list_head req_listhead;
};

#define MAX_LUN		( 256 )
#define MAX_LUN_NLONG	( (MAX_LUN + 8 * sizeof(unsigned long) - 1) / (8 * sizeof(unsigned long)))
#define MAX_LUN_NBYTE	( MAX_LUN_NLONG * sizeof(unsigned long) )
struct shannon_bbt {
	char name[32];

	int nchannel;
	int nthread;
	int nlun;
	int nplane;

	int nblock;
	int npage;

	int size;
	int rsv[2];

	unsigned long sb_bbt[0][MAX_LUN_NLONG];
};

struct shannon_sb_luninfo {	/* for per-super-block */
	int luns;
	int ndatalun;
	int parity_lun;
	unsigned long sb_bbt[MAX_LUN_NLONG];
};

struct shannon_luninfo {	/* file total super block */
	char name[32];

	int nchannel;
	int nthread;
	int nlun;
	int nplane;

	int nblock;
	int npage;

	int size;
	int rsv[2];

	struct shannon_sb_luninfo sb_luninfo[0];
};

struct shannon_super_block {
	int idx;
	struct shannon_sb_luninfo sb_luninfo;
};

struct shannon_request {
	int opcode;

	int lun;
	int ppa;
	int head;
	int bsector;
	int nsector;

	int block;
	int page;
	int chunk_block;
	int chunk_plane;

	int advance_read;
	struct shannon_request *sibling_arreq;

	__u8 *data;
	__u64 *metadata;	/* cacheread needs */
	union {
		__u8 id[8];	/* readid cmd needs */
		__u8 ecc[64];	/* cacheread cmd needs */
		__u64 status;	/* others cmd need status */
		__u8 direct_cmdqueue[64];	/* directly construct cmdqueue, for example used for write regs */
	};
	int bfree;		/* decide whether free data/metadata mem or not */
	int rw_entire_buffer;	/* mean data and metadata are in the entire buffer if set */
	int no_dma;             /* mean the request will not generate DMA at all */
	int last_cacheread;
	int wr_flash_reg_nbyte;

	int cmdhead;
	int cmdlen;
	int bufcmd;		/* !=0 means buffer write/read request */

	struct shannon_dev *dev;
	struct list_head list;
	struct list_head run_list;
	struct list_head chunk_list;
	union {
		struct list_head lun_list;	/* link req mounted on this lun */
		struct list_head bufhead_list;
	};
	struct list_head mem_listhead;	/* head of memory recording kernel_addr and dma_addr */

	int private_int;
	int private_int_1;

	void *private_data;
	void *private_data_1;
};

#define NORFLASH_INFO_ADDR	0X1FFF000
#define	NORFLASH_INFO_MAGIC	0X646972656374696F
struct norflash_info {
	u64 magic_number;
	char service_tag[32];
	char model_id[40];
};

struct shannon_dev {
	int fd;
	char name[32];
	char domains[32];
	int init_mode;			/* 0, normal init; 1, use present hw config */
	struct usr_flash *flash;
	struct usr_flash *flash_bakup;
	struct usr_config *config;
	struct usr_config *config_bakup;

	int atomic_write;
	int prioritize_write;

	int fblocks;			/* user input flash blocks number */

	int init_done;
	int ifmode;			/* present both HW and flash interface mode */

	int advance_read;
	int has_advance_read;
	u64 mbr_version;

	int silent_config;
	int power_budget;
	int flash_ifclock;
	int interrupt_delay;
	int dma_max_read_limit;
	int max_outstanding_bios;

	struct hw_sysinfo *hw_sysinfo;	/* hareware system registers, must be synchronized with HW */
	struct hw_config *hw_config;	/* hardware config registers, must be synchronized with HW */

	struct list_head mem_glisthead;
	struct memory dummy_mem;

	int iowidth;			/* 1, 8bit; 2, 16bit */
	int tmode;
	int mmode;
	int maxplanes;			/* maxplanes supported for the flash attached */
	int targetluns_support;

	int manual_nplane;
	char subsystemid[8];

	int hw_nchannel;
	int hw_nthread;
	int hw_nlun;
	int hw_threads;
	int hw_luns;

	int loops;
	int bad_blocks;
	unsigned int *bb_count;
	FILE *recordfp;
	int scan_loops;
	int print_burnin_ecc_histogram;

	float max_controller_temp;
	float max_flash_temp;
	float max_board_temp;

	int newlunmap;

	int group_raid_num;
	int group_raid_luns;

	int sysreg_dwoff;
	int cfgreg_dwoff;
	int lunreg_dwoff;
	int lunreg_dwsize;
	int bar_dwlen[2];

	int timeout_silent;		/* 1 don`t print cmdqueue timeout information */
	int valid_luns;
	int invalid_blocks;
	unsigned long lun_bitmap[32];	/* invalid lun bitmap: 0 valid; 1 invalid*/
	unsigned long lun_bitmap_backup[32];
	int sorting_ecc_limit;

	unsigned long absent_lun_bitmap[32];
	int present_luns;
	int absent_luns;
	int check_bad_luns;

	struct shannon_super_block *sb;
	struct thread_mem *phythread_mem;

	struct shannon_thread *thread;	/* per-thread manage structure */
	struct shannon_lun *lun;	/* per-lun manage structure */
	struct shannon_bufhead *bufhead;/* per-bufhead manage structure */

	__u32 **badblk_bmp;		/* bad blk bitmap per-lun */

	int unsafe_mbrblock[4][512];

	void *padding_buffer;

	FILE *exitlog;

	int ctrl_temp_threshold;
	int flash_temp_threshold;
	int board_temp_threshold;

	struct norflash_info norinfo;

	char sorting_print_string[256];
	time_t mpt_begintime;
	char mpt_timetook[256];
	int disable_ecc;

#define	TEMPERATURE_LIMIT	0x2026
	int exitcode;

	/* hardware read/write operations */
	void (*iowrite32)(struct shannon_dev *dev, __u32 value, int reg_dw_off);
	__u32 (*ioread32)(struct shannon_dev *dev, int reg_dw_off);

	void (*raw_writel)(struct shannon_dev *dev, __u32 value, int reg_dw_off);
	__u32 (*raw_readl)(struct shannon_dev *dev, int reg_dw_off);

	void (*multi_ioread32)(struct shannon_dev *dev, __u32 *des, int dwoff, int dwlen);
	void (*multi_iowrite32)(struct shannon_dev *dev, __u32 *src, int dwoff, int dwlen);

	void (*multi_raw_readl)(struct shannon_dev *dev, __u32 *des, int dwoff, int dwlen);
	void (*multi_raw_writel)(struct shannon_dev *dev, __u32 *src, int dwoff, int dwlen);

	void (*iowrite_lunreg)(struct shannon_dev *dev, __u32 value, int lun, enum HW_lunreg lunreg_dw_off);
	__u32 (*ioread_lunreg)(struct shannon_dev *dev, int lun, enum HW_lunreg lunreg_dw_off);

	void (*iowrite_buflunreg)(struct shannon_dev *dev, __u32 value, int head, enum HW_lunreg lunreg_dw_off);
	__u32 (*ioread_buflunreg)(struct shannon_dev *dev, int head, enum HW_lunreg lunreg_dw_off);

	void (*ioread_config)(struct shannon_dev *dev);
	void (*iowrite_config)(struct shannon_dev *dev);

	void (*reset)(struct shannon_dev *dev);

	void (*get_mem)(struct shannon_dev *dev, struct memory *mem);
	void (*free_mem)(struct shannon_dev *dev, struct memory *mem);
	void (*read_mem)(struct shannon_dev *dev, void *user_addr, void *kernel_addr, int size);
	void (*write_mem)(struct shannon_dev *dev, void *kernel_addr, void *user_addr, int size);

	void (*deliver_userdata)(struct shannon_dev *dev);

	void (*do_direct_io)(struct shannon_dev *dev, struct direct_io *dio);

	void (*clear_queue)(struct shannon_dev *dev);
	void (*config_hardware)(struct shannon_dev *dev);

	int (*submit_request)(struct shannon_request *req);

	struct shannon_mbr *mbr;
	struct shannon_mbr *inherent_mbr;
	int inherent_mbr_sector_size;

	int private_int;
	void *private_data;
	int active_block[8];

	int unsafe_cfgable;

	unsigned long mpt_set_capacity;

	int bm_bbt;
	int mbr_by_4k;

	int per_byte_disable;
	int valid_8bit;

	int dev_type;

	struct target_lun {
		union flash_id id;
		int use;
		int shadow;
		int loglun;
		int phylun;

		int blk_hole_begin;
		int blk_hole_count;
	}*targetlun;
};

struct live_context {
	char id[32];
	struct shannon_dev *dev;

	__u32 *reg;
	__u64 **cmdq;
	__u64 **cmpq;
};

/*-----------------------------------------------------------------------------------------------------------------------------*/
#define	print(x...)		do { printf(x); fflush(stdout); } while (0)
#define	perror_exit(x...)	do { print(x); perror(" "); exit(9); } while (0)
#define	malloc_failed_exit()	do { printf("%s() %d malloc failed\n", __func__, __LINE__); exit(EXIT_FAILURE); } while (0)
#define	submit_failed_exit(lun)	do { printf("%s() %d lun-%d submit request failed\n", __func__, __LINE__, lun); exit(EXIT_FAILURE); } while (0)
#define	poll_failed_exit(lun)	do { printf("%s() %d lun-%d poll request failed\n", __func__, __LINE__, lun); exit(EXIT_FAILURE); } while (0)

extern struct shannon_dev *thisdev;
#define	exitlog_withstatus(status, x...)	do { \
							printf("%s() in %s line %d: ", __func__, __FILE__, __LINE__); \
							printf(x); \
							if (thisdev != NULL && thisdev->exitlog != NULL) { \
								fprintf(thisdev->exitlog, "%s() in %s line %d: ", __func__, __FILE__, __LINE__); \
								fprintf(thisdev->exitlog, x); \
							} \
							exit(status); \
						} while (0)
#define	exitlog(x...)				exitlog_withstatus(EXIT_FAILURE, x)

/* print xdigit array */
#define	pr_array(addr, type, cnt, align, prefix)		\
	do {							\
		int ixxi;					\
		typeof(type) *p = (typeof(type)*)(addr);	\
		char format[32] = "%04X ";			\
								\
		format[2] = 2 * sizeof(type) + '0';		\
		if (8 == sizeof(type))				\
			strcpy(format, "%016llX ");		\
		for (ixxi = 0; ixxi < (cnt); ixxi++) {		\
			if (0 == ixxi%(align) && ixxi != 0)	\
				printf("\n");			\
			if (0 == ixxi%(align) && prefix)	\
				printf("0x%04X: ", (int)(ixxi * sizeof(*p)));\
			printf(format, p[ixxi]);		\
		}						\
		printf("\n");					\
	} while (0)

#define	pr_u8_array(addr, cnt, align)	pr_array(addr, __u8, cnt, align, 1)
#define	pr_u16_array(addr, cnt, align)	pr_array(addr, __u16, cnt, align, 1)
#define	pr_u32_array(addr, cnt, align)	pr_array(addr, __u32, cnt, align, 1)
#define	pr_u64_array(addr, cnt, align)	pr_array(addr, __u64, cnt, align, 1)

#define	pr_u8_array_noprefix(addr, cnt, align)	pr_array(addr, __u8, cnt, align, 0)
#define	pr_u16_array_noprefix(addr, cnt, align)	pr_array(addr, __u16, cnt, align, 0)
#define	pr_u32_array_noprefix(addr, cnt, align)	pr_array(addr, __u32, cnt, align, 0)
#define	pr_u64_array_noprefix(addr, cnt, align)	pr_array(addr, __u64, cnt, align, 0)

static inline void pad_rand(void *buf, int count)
{
	int i;

	for (i = 0; i < count; i++)
		((__u8*)buf)[i] = rand();
}

static inline void pad_inc_u8(void *buf, int count, __u8 init_value)
{
	int i;

	for (i = 0; i < count/sizeof(__u8); i++)
		((__u8*)buf)[i] = init_value + i;
}

static inline void pad_inc_u16(void *buf, int count, __u16 init_value)
{
	int i;

	for (i = 0; i < count/sizeof(__u16); i++)
		((__u16*)buf)[i] = init_value + i;
}

static inline void pad_inc_u32(void *buf, int count, __u32 init_value)
{
	int i;

	for (i = 0; i < count/sizeof(__u32); i++)
		((__u32*)buf)[i] = init_value + i;
}

static inline void pad_same_u32(void *buf, int count, __u32 value)
{
	int i;

	for (i = 0; i < count/sizeof(__u32); i++)
		((__u32*)buf)[i] = value;
}

static inline void memxor(void *dst, void *src, int cnt)
{
	int i;

	for (i = 0; i < cnt; i++)
		((__u8 *)dst)[i] ^= ((__u8 *)src)[i];
}
/*-----------------------------------------------------------------------------------------------------------------------------*/
// init.c
extern struct shannon_dev *alloc_device(char *devname);
extern int init_device(struct shannon_dev *dev);
extern int re_init_device(struct shannon_dev *dev);
extern void free_device(struct shannon_dev *dev);

// parse.c
extern int parse_flash(struct shannon_dev *dev);
extern int parse_config(struct shannon_dev *dev);
extern int calculate_indirect_config(struct shannon_dev *dev);
extern int flash_info(struct shannon_dev *dev, union flash_id fid, struct usr_flash *flash);

// utils.c
extern int shannon_utils(struct shannon_dev *dev, int argc, char **argv);
extern int shannon_info(struct shannon_dev *dev, int argc, char **argv);
extern int shannon_hwinfo(struct shannon_dev *dev, int argc, char **argv);

// req.c
extern struct shannon_request *alloc_request(struct shannon_dev *dev, enum shannon_cmd opcode,
			int lun, int ppa, int head, int bsector, int nsector);
extern struct shannon_request *alloc_request_no_dma(struct shannon_dev *dev, enum shannon_cmd opcode,
						   int lun, int ppa, int head, int bsector, int nsector, int no_dma);
extern void free_request(struct shannon_request *req);
extern int submit_request(struct shannon_request *req);
extern void submit_polling_loop(struct shannon_dev *dev, struct list_head *req_head);

extern int __poll_cmdqueue(struct shannon_dev *dev, int lun, int wait);
extern int __poll_bufcmdqueue(struct shannon_dev *dev, int head, int wait);

// api.c
extern int check_req_status(struct shannon_request *req);
extern int check_req_status_silent(struct shannon_request *req);

extern int shannon_reset(struct shannon_dev *dev, int argc, char **argv);
extern int shannon_readid(struct shannon_dev *dev, int argc, char **argv);
extern int shannon_erase(struct shannon_dev *dev, int argc, char **argv);
extern int shannon_write(struct shannon_dev *dev, int argc, char **argv);
extern int shannon_read(struct shannon_dev *dev, int argc, char **argv);
extern int shannon_copy(struct shannon_dev *dev, int argc, char **argv);
extern int shannon_rwloop(struct shannon_dev *dev, int argc, char **argv);

extern int shannon_softbitread(struct shannon_dev *dev, int argc, char **argv);
extern int shannon_softbitread_a19(struct shannon_dev *dev, int argc, char **argv);

// super.c
extern void present_absent_luns(struct shannon_dev *dev, char *value, int type);
extern int shannon_super_reset(struct shannon_dev *dev);
extern int shannon_super_readid(struct shannon_dev *dev, int argc, char **argv);
extern int shannon_super_async(struct shannon_dev *dev);
extern int shannon_super_sync(struct shannon_dev *dev);
extern int shannon_super_toggle(struct shannon_dev *dev);
extern int super_set_feature(struct shannon_dev *dev, u8 cmd, u8 addr, u8 *value, int nbyte);

extern int shannon_super_erase(struct shannon_dev *dev, int argc, char **argv);
extern int shannon_super_write(struct shannon_dev *dev, int argc, char **argv);
extern int shannon_super_read(struct shannon_dev *dev, int argc, char **argv);

extern int construct_sb_luninfo(struct shannon_dev *dev, char *luninfo_file);

// bbt.c
extern int shannon_bbt_ops(struct shannon_dev *dev, int argc, char **argv);
extern int shannon_luninfo_ops(struct shannon_dev *dev, int argc, char **argv);
extern int check_bbt(struct shannon_dev *dev, struct shannon_bbt *bbt);
extern int check_luninfo(struct shannon_dev *dev, struct shannon_luninfo *luninfo);
extern int erase_scan(struct shannon_dev *dev, struct shannon_bbt *bbt);
extern int flagbyte_scan(struct shannon_dev *dev, struct shannon_bbt *bbt);

extern void human_bbt_info(char *bbtfile);
extern void human_luninfo_info(char *luninfo_file);

// reqlist.c
extern void destory_reqlist(struct list_head *req_head);
extern struct list_head *make_shead_reqlist(struct shannon_dev *dev, int lun, int block, int head, int seed);

// ecc.c
extern int shannon_fake_ecc(struct shannon_dev *dev, int argc, char **argv);
extern int shannon_rmw_fake_ecc(struct shannon_dev *dev, int argc, char **argv);

// mpt.c
extern int shannon_mpt(struct shannon_dev *dev, int argc, char **argv);
extern int shannon_mpt_readbbt(struct shannon_dev *dev, int check_only);

// ifmode.c
extern int shannon_ifmode(struct shannon_dev *dev, int argc, char **argv);

// bufwrite.c
extern int shannon_bufwrite(struct shannon_dev *dev, int argc, char **argv);

// dio.c
extern int shannon_dio(struct shannon_dev *dev, int argc, char **argv);

// nor.c
extern int shannon_nor(struct shannon_dev *dev, int argc, char **argv);
extern int shannon_read_nor(struct shannon_dev *dev, void *buf, int start, int len);

// help.c
extern void init_nrand(int num, int seed);
extern void exit_nrand(void);
extern int nrand(void);
extern int get_nrand(int idx);
extern void uniq_rand(int num, int seed, int uniq_buffer[]);

// inline fuctions
static inline int poll_cmdqueue(struct shannon_dev *dev, int lun)
{
	return __poll_cmdqueue(dev, lun, 1);
}

static inline int poll_cmdqueue_nowait(struct shannon_dev *dev, int lun)
{
	return __poll_cmdqueue(dev, lun, 0);
}

static inline int poll_bufcmdqueue(struct shannon_dev *dev, int head)
{
	return __poll_bufcmdqueue(dev, head, 1);
}

static inline int poll_bufcmdqueue_nowait(struct shannon_dev *dev, int head)
{
	return __poll_bufcmdqueue(dev, head, 0);
}

static inline void update_cmdqueue(struct shannon_dev *dev, int lun)
{
	dev->iowrite_lunreg(dev, dev->lun[lun].thread->cmdhead, lun, HW_cmdq_head);
}

static inline void update_bufcmdqueue(struct shannon_dev *dev, int head)
{
	dev->iowrite_buflunreg(dev, dev->bufhead[head].cmdhead, head, HW_cmdq_head);
}

static inline int update_poll_cmdqueue(struct shannon_dev *dev, int lun)
{
	update_cmdqueue(dev, lun);
	return poll_cmdqueue(dev, lun);
}

static inline int update_poll_bufcmdqueue(struct shannon_dev *dev, int head)
{
	update_bufcmdqueue(dev, head);
	return poll_bufcmdqueue(dev, head);
}

static inline int check_status(struct shannon_dev *dev, enum shannon_cmd cmd, __u64 status)	// return 1: success; 0. failed.
{
	assert(0 != dev->flash->success_mask);

	if (0xFE == status)		// timeout
		return 0;

	if (sh_preread_cmd == cmd)	// skip check preread status
		return 1;

	return (status & dev->flash->success_mask) == dev->flash->success_status;
}

/* coordinate of phylun is (get_phychannel, get_phythread, get_phylun) */
static inline int get_phychannel(struct shannon_dev *dev, int loglun)
{
	return loglun % dev->config->nchannel;
}

static inline int get_phythread(struct shannon_dev *dev, int loglun)
{
	if (dev->newlunmap)
		return (loglun % (dev->config->nchannel * dev->config->nthread)) / dev->config->nchannel;
	return loglun / (dev->config->nchannel * dev->config->nlun);
}

static inline int get_phylun(struct shannon_dev *dev, int loglun)
{
	if (dev->newlunmap)
		return loglun / (dev->config->nchannel * dev->config->nthread);
	return (loglun % (dev->config->nchannel * dev->config->nlun)) / dev->config->nchannel;
}

static inline int log2phy_lun(struct shannon_dev *dev, int loglun)
{
	int ch, tr, ln;

	ln = get_phylun(dev, loglun);
	tr = get_phythread(dev, loglun);
	ch = get_phychannel(dev, loglun);

	return (ch * dev->hw_nthread + tr) * dev->hw_nlun + ln;
}

static inline int phy2log_lun(struct shannon_dev *dev, int phylun)
{
	int loglun;

	for (loglun = 0; loglun < dev->config->luns; loglun++) {
		if (phylun == log2phy_lun(dev, loglun))
			return loglun;
	}

	printf("%s() BUG\n", __func__);
	exit(EXIT_FAILURE);
}

static inline void *zmalloc(int size)
{
	void *mem;

	if (NULL != (mem = malloc(size)))
		memset(mem, 0x00, size);

	return mem;
}

static inline int is_bad_lunblock(struct shannon_dev *dev, int lun, int blk)
{
	if (!dev->targetlun[lun].blk_hole_count)
		return test_bit(lun, dev->sb[blk].sb_luninfo.sb_bbt);
	else
		return (blk*dev->config->nplane >= dev->targetlun[lun].blk_hole_begin) || test_bit(lun, dev->sb[blk].sb_luninfo.sb_bbt);
}

static inline int is_bad_superblock(struct shannon_dev *dev, int superblk)
{
	return dev->sb[superblk].sb_luninfo.luns == 0;
}

static inline int superblock_paritylun(struct shannon_dev *dev, int superblk)
{
	assert(dev->sb[superblk].sb_luninfo.luns != 0);
	return dev->sb[superblk].sb_luninfo.parity_lun;
}

static inline int superblock_nlun(struct shannon_dev *dev, int superblk)
{
	return dev->sb[superblk].sb_luninfo.luns;
}

static inline int superblock_ndatalun(struct shannon_dev *dev, int superblk)
{
	return dev->sb[superblk].sb_luninfo.ndatalun;
}

static inline int getseed(int way)
{
	struct timeval tv;

	if (0 == way) {
		if (!gettimeofday(&tv, NULL))
			return (int)(tv.tv_usec);
	}
	return (int)(time(NULL));
}

static inline char *ifmode_string(int ifmode)
{
	if (FLASH_IFMODE_ASYNC == ifmode || IFMODE_ASYNC == ifmode)
		return "async";
	else if (FLASH_IFMODE_SYNC == ifmode || IFMODE_SYNC == ifmode)
		return "sync";
	else if (FLASH_IFMODE_TOGGLE == ifmode || IFMODE_TOGGLE == ifmode)
		return "toggle";
	else
		return "unknown ifmode";
}

static inline int check_ifmode_match(struct shannon_dev *dev)
{
	if ( (FLASH_IFMODE_ASYNC == dev->flash->ifmode && IFMODE_ASYNC != dev->config->ifmode) ||
	     (FLASH_IFMODE_SYNC == dev->flash->ifmode && IFMODE_SYNC != dev->config->ifmode && IFMODE_ASYNC != dev->config->ifmode) ||
	     (FLASH_IFMODE_TOGGLE == dev->flash->ifmode && IFMODE_TOGGLE != dev->config->ifmode && IFMODE_ASYNC != dev->config->ifmode) ) {
		printf("ERROR ifmode conflict: flash %s, config %s\n", ifmode_string(dev->flash->ifmode), ifmode_string(dev->config->ifmode));
		return 1;
	}

	return 0;
}

// microcode.c
void write_advanced_read_microcode(struct shannon_dev *dev);

/*-----------------------------------------------------------------------------------------------------------------------------*/
// macros 
#define	for_dev_each_lun(dev, lun)			\
	for (lun = 0; lun < dev->config->luns; lun++)	\
		if (test_bit(lun, dev->lun_bitmap))	\
			continue;			\
		else
#define	for_dev_each_block(dev, blk)			\
	for (blk = 0; blk < dev->flash->nblk / dev->config->nplane; blk++)
#define	for_dev_each_page(dev, page)			\
	for (page = 0; page < dev->flash->npage; page++)
#define	for_dev_each_bufhead(dev, ihead)		\
	for (ihead = 0; ihead < 2; ihead++)

/*-----------------------------------------------------------------------------------------------------------------------------*/
#if BE_ARCH == 1
#include <linux/byteorder/big_endian.h>

#define	cpu_to_le16(x)  __cpu_to_le16(x)
#define	cpu_to_le32(x)  __cpu_to_le32(x)
#define	cpu_to_le64(x)  __cpu_to_le64(x)

#define	cpu_to_le16s(x) __cpu_to_le16s((u16 *)(x))
#define	cpu_to_le32s(x) __cpu_to_le32s((u32 *)(x))
#define	cpu_to_le64s(x) __cpu_to_le64s((u64 *)(x))

#define	le16_to_cpu(x)  __le16_to_cpu(x)
#define	le32_to_cpu(x)  __le32_to_cpu(x)
#define	le64_to_cpu(x)  __le64_to_cpu(x)

#define	le16_to_cpus(x) __le16_to_cpus((u16 *)(x))
#define	le32_to_cpus(x) __le32_to_cpus((u32 *)(x))
#define	le64_to_cpus(x) __le64_to_cpus((u64 *)(x))
#else
#define	cpu_to_le16(x)  (x)
#define	cpu_to_le32(x) 	(x)
#define	cpu_to_le64(x)  (x)

#define	cpu_to_le16s(x)
#define	cpu_to_le32s(x)
#define	cpu_to_le64s(x)

#define	le16_to_cpu(x)  (x)
#define	le32_to_cpu(x)  (x)
#define	le64_to_cpu(x)  (x)

#define	le16_to_cpus(x)
#define	le32_to_cpus(x)
#define	le64_to_cpus(x)
#endif

/*-----------------------------------------------------------------------------------------------------------------------------*/
#endif /* __SHANNON_TOOL_H__ */
