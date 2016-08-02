#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/time.h>

#include "tool.h"

/*-----------------------------------------------------------------------------------------------------------*/
static void shannon_utils_usage(void)
{
#ifndef __RELEASE__
	printf("USAGE:\n");

	printf("\tphylun-mem: show allocated memory for per-phylun\n");
	printf("\tpeek-phylunregs phylun(Dec): display phylun registers\n");
	printf("\tpeek-phylunmem phylun(Dec) offset(Hex) length(Hex): display phylun memory\n");
	printf("\tpeek-regs dw-offset(Hex) dw-number(Hex): display registers\n");
	printf("\tpeek-sysregs: display system info registers\n");
	printf("\tpeek-cfgregs: display config registers\n");
	printf("\tpeek-bufregs head(Dec): display bufwrite/read control registers if have\n");
	printf("\tpeek-bufcmdq head(Dec) length(Hex): display bufwrite/read command queue if have\n");
	printf("\tpeek-bufcmpq head(Dec) length(Hex): display bufwrite/read completion queue if have\n\n");

	printf("\tpeek-lunregs lun(Dec): display logical lun registers\n");
	printf("\tpeek-luncmdq lun(Dec) length(Hex): display command queue\n");
	printf("\tpeek-luncmpq lun(Dec) length(Hex): display completion queue\n");
	printf("\tpeek-sections: display all lun and bufwrite/read control registers\n\n");

	printf("\tpoke-regs dw-offset(Hex) dw-value0(Hex) dw-value1(Hex) ... : write registers\n\n");

	printf("\tpoke-buf dw-offset(Dec) dw-number(Dec): write data buffer in BAR1 with random data then print\n");
	printf("\tpeek-buf dw-offset(Dec) dw-number(Dec): read data buffer in BAR1 then print\n\n");

	printf("\tparse-file filename: parse such as bbt file, luninfo file and so on to human read format\n");
#endif
}

/*-----------------------------------------------------------------------------------------------------------*/
#ifndef __RELEASE__
static void do_phylun_mem(struct shannon_dev *dev, int argc, char **argv)
{
	int lun;
	
	for (lun = 0; lun < dev->hw_nlun; lun++) {
		printf("phylun-%03d: ", lun);
		printf("%016lX %016lX\n", (ulong)dev->phythread_mem[lun / dev->hw_nlun].kernel_addr,
			(ulong)dev->phythread_mem[lun / dev->hw_nlun].dma_addr);
	}
}

static void do_peek_phylunregs(struct shannon_dev *dev, int argc, char **argv)
{
	__u32 *buf;
	int lun;

	if (argc != 2) {
		shannon_utils_usage();
		return;
	}

	lun = strtoul(argv[1], NULL, 10);

	buf = malloc(dev->lunreg_dwsize * DW_SIZE);
	if (NULL == buf) {
		printf("%s() malloc failed\n", __func__);
		return;
	}

	dev->multi_ioread32(dev, buf, dev->lunreg_dwoff + (lun / dev->hw_nlun) * dev->lunreg_dwsize, dev->lunreg_dwsize);
	pr_u32_array_noprefix(buf, dev->lunreg_dwsize, dev->lunreg_dwsize);
	free(buf);
}

static void do_peek_phylunmem(struct shannon_dev *dev, int argc, char **argv)
{
	__u32 *buf;
	int lun, off, num;

	if (argc != 4) {
		shannon_utils_usage();
		return;
	}

	lun = strtoul(argv[1], NULL, 10);
	off = strtoul(argv[2], NULL, 0x10);
	num = strtoul(argv[3], NULL, 0x10);

	buf = malloc(num);
	if (NULL == buf) {
		printf("%s() malloc failed\n", __func__);
		return;
	}

	dev->read_mem(dev, buf, dev->phythread_mem[lun / dev->hw_nlun].kernel_addr + off, num);
	pr_u64_array(buf, num/8, 8);
	free(buf);
}

static void do_peek_regs(struct shannon_dev *dev, int argc, char **argv)
{
	int off, num;
	__u32 *buf;

	if (argc != 3) {
		shannon_utils_usage();
		return;
	}

	off = strtoul(argv[1], NULL, 0x10);
	num = strtoul(argv[2], NULL, 0x10);

	buf = malloc(num * DW_SIZE);
	if (NULL == buf) {
		printf("%s() malloc failed\n", __func__);
		return;
	}

	dev->multi_ioread32(dev, buf, off, num);
	pr_u32_array(buf, num, 8);
	free(buf);
}

static void do_peek_bufregs(struct shannon_dev *dev, int argc, char **argv)
{
	int i, head;
	__u32 *buf;

	if (!(dev->hw_sysinfo->hw_wrbuf_support & 0x0F)) {
		printf("Hardware don`t support bufwrite/bufread\n");
		return;
	}

	if (argc != 2) {
		shannon_utils_usage();
		return;
	}

	head = strtoul(argv[1], NULL, 10);
	assert(head < 2);

	buf = malloc(dev->lunreg_dwsize * DW_SIZE);
	if (NULL == buf) {
		printf("%s() malloc failed\n", __func__);
		return;
	}

	for (i = 0; i < dev->lunreg_dwsize; i++)
		buf[i] = dev->ioread_buflunreg(dev, head, i);

	pr_u32_array_noprefix(buf, dev->lunreg_dwsize, dev->lunreg_dwsize);
	free(buf);
}

static void do_peek_bufcmdq(struct shannon_dev *dev, int argc, char **argv)
{
	int head, len;
	void *buf;

	if (!(dev->hw_sysinfo->hw_wrbuf_support & 0x0F)) {
		printf("Hardware don`t support bufwrite/bufread\n");
		return;
	}

	if (argc != 3) {
		shannon_utils_usage();
		return;
	}

	head = strtoul(argv[1], NULL, 10);
	len = strtoul(argv[2], NULL, 0x10);
	assert(head < 2 && len <= 0x1000);

	buf = malloc(len);
	if (NULL == buf) {
		printf("%s() malloc failed\n", __func__);
		return;
	}

	dev->read_mem(dev, buf, dev->phythread_mem[head].kernel_addr + 2 * PAGE_SIZE, len);
	pr_u64_array(buf, len/8, 8);
	free(buf);
}

static void do_peek_bufcmpq(struct shannon_dev *dev, int argc, char **argv)
{
	int head, len;
	void *buf;

	if (!(dev->hw_sysinfo->hw_wrbuf_support & 0x0F)) {
		printf("Hardware don`t support bufwrite/bufread\n");
		return;
	}

	if (argc != 3) {
		shannon_utils_usage();
		return;
	}

	head = strtoul(argv[1], NULL, 10);
	len = strtoul(argv[2], NULL, 0x10);
	assert(head < 2 && len <= 0x1000);

	buf = malloc(len);
	if (NULL == buf) {
		printf("%s() malloc failed\n", __func__);
		return;
	}

	dev->read_mem(dev, buf, dev->phythread_mem[head].kernel_addr + 3 * PAGE_SIZE, len);
	pr_u64_array(buf, len/8, 8);
	free(buf);
}

static void do_peek_lunregs(struct shannon_dev *dev, int argc, char **argv)
{
	int i, lun;
	__u32 *buf;

	if (argc != 2) {
		shannon_utils_usage();
		return;
	}

	lun = strtoul(argv[1], NULL, 10);

	buf = malloc(dev->lunreg_dwsize * DW_SIZE);
	if (NULL == buf) {
		printf("%s() malloc failed\n", __func__);
		return;
	}

	for (i = 0; i < dev->lunreg_dwsize; i++)
		buf[i] = dev->ioread_lunreg(dev, lun, i);

	pr_u32_array_noprefix(buf, dev->lunreg_dwsize, dev->lunreg_dwsize);
	free(buf);
}

static void do_peek_luncmdq(struct shannon_dev *dev, int argc, char **argv)
{
	__u64 *buf;
	__u64 __attribute__((unused))*p;
	int i, lun, len;

	if (argc != 3) {
		shannon_utils_usage();
		return;
	}

	lun = strtoul(argv[1], NULL, 10);
	len = strtoul(argv[2], NULL, 0x10);
	assert(lun < dev->config->luns && len <= 0x1000);

	buf = malloc(len);
	if (NULL == buf) {
		printf("%s() malloc failed\n", __func__);
		return;
	}
	p = (__u64 *)buf;

	dev->read_mem(dev, buf, dev->phythread_mem[log2phy_lun(dev, lun) / dev->hw_nlun].kernel_addr, len);
	for (i = 0; i < len/sizeof(__u64); i++)
		le64_to_cpus(&p[i]);
	pr_u64_array(buf, len/8, 8);
	free(buf);
}

static void do_peek_luncmpq(struct shannon_dev *dev, int argc, char **argv)
{
	__u64 *buf;
	__u64 __attribute__((unused))*p;
	int i, lun, len;

	if (argc != 3) {
		shannon_utils_usage();
		return;
	}

	lun = strtoul(argv[1], NULL, 10);
	len = strtoul(argv[2], NULL, 0x10);
	assert(lun < dev->config->luns && len <= 0x1000);

	buf = malloc(len);
	if (NULL == buf) {
		printf("%s() malloc failed\n", __func__);
		return;
	}
	p = (__u64 *)buf;

	dev->read_mem(dev, buf, dev->phythread_mem[log2phy_lun(dev, lun) / dev->hw_nlun].kernel_addr + PAGE_SIZE, len);
	for (i = 0; i < len/sizeof(__u64); i++)
		le64_to_cpus(&p[i]);
	pr_u64_array(buf, len/8, 8);
	free(buf);
}

static void do_peek_sections(struct shannon_dev *dev, int argc, char **argv)
{
	int i;
	char *v[2], v0[8], v1[8];

	v[0] = v0;
	v[1] = v1;

	for (i = 0; i < dev->config->luns; i++) {
		memset(v1, 0x00, sizeof(v1));
		sprintf(v1, "%d", i);
		// printf("lun %s: ", v[1]);
		printf("lun %-3d: ", i);
		do_peek_lunregs(dev, 2, v);
	}

	if (dev->hw_sysinfo->hw_wrbuf_support & 0x0F) {
		for (i = 0; i < 2; i++) {
			memset(v1, 0x00, sizeof(v1));
			sprintf(v1, "%d", i);
			// printf("bufhead %s: ", v[1]);
			printf("bufhead %d: ", i);
			do_peek_bufregs(dev, 2, v);
		}
	}
}

/*-----------------------------------------------------------------------------------------------------------*/
static void do_poke_regs(struct shannon_dev *dev, int argc, char **argv)
{
	__u32 *buf;
	int i, len, off;

	if (argc < 3) {
		shannon_utils_usage();
		return;
	}

	len = argc - 2;
	off = strtoul(argv[1], NULL, 0x10);

	buf = malloc(len * DW_SIZE);
	if (NULL == buf) {
		printf("malloc failed\n");
		return;
	}

	for (i = 0; i < len; i++)
		buf[i] = strtoul(argv[i + 2], NULL, 0x10);
	// printf("0x%X %d: ", off, len);
	// pr_u32_array_noprefix(buf, len, 8);

	dev->multi_iowrite32(dev, buf, off, len);
	free(buf);
}

static void do_poke_buf(struct shannon_dev *dev, int argc, char **argv)
{
	struct timeval tv;
	int i, off, len;
	__u32 *buf;

	if (0 == dev->bar_dwlen[1]) {
		printf("Hardware has no BAR0!\n");
		return;
	}

	if (argc != 3) {
		shannon_utils_usage();
		return;
	}

	off = strtoul(argv[1], NULL, 10);
	len = strtoul(argv[2], NULL, 10);

	if (off + len > 256) {
		printf("Out of buffer address\n");
		return;
	}

	buf = malloc(len * DW_SIZE);
	if (NULL == buf) {
		printf("malloc failed\n");
		return;
	}

	if (gettimeofday(&tv, NULL)) {
		printf("Get random seed failed\n");
		free(buf);
		return;
	}
	srand(tv.tv_usec);
	pad_rand(buf, len * DW_SIZE);
	pr_u8_array_noprefix(buf, len * DW_SIZE, 32);

	for (i = 0; i < len; i++)
		dev->iowrite32(dev, buf[i], dev->bar_dwlen[0] + i);

	free(buf);
}

static void do_peek_buf(struct shannon_dev *dev, int argc, char **argv)
{
	int i, off, len;
	__u32 *buf;

	if (0 == dev->bar_dwlen[1]) {
		printf("Hardware has no BAR0!\n");
		return;
	}

	if (argc != 3) {
		shannon_utils_usage();
		return;
	}

	off = strtoul(argv[1], NULL, 10);
	len = strtoul(argv[2], NULL, 10);

	if (off + len > 256) {
		printf("Out of buffer address\n");
		return;
	}

	buf = malloc(len * DW_SIZE);
	if (NULL == buf) {
		printf("malloc failed\n");
		return;
	}

	for (i = 0; i < len; i++)
		buf[i] = dev->ioread32(dev, dev->bar_dwlen[0] + i);
	pr_u8_array_noprefix(buf, len * DW_SIZE, 32);

	free(buf);
}

/*-----------------------------------------------------------------------------------------------------------*/
static void do_parse_file(struct shannon_dev *dev, int argc, char **argv)
{
	int fd;
	char tag[32];

	if (argc != 2) {
		shannon_utils_usage();
		return;
	}

	if ((fd = open(argv[1], O_RDONLY)) < 0) {
		perror("Open file failed:");
		return;
	}

	if (read(fd, tag, 32) != 32) {
		perror("Read file failed:");
		close(fd);
		return;
	}

	if (!strcmp("shannon-bbt", tag))
		human_bbt_info(argv[1]);
	else if (!strcmp("shannon-luninfo", tag))
		human_luninfo_info(argv[1]);
	else
		printf("Can`t parse this type file\n");

	close(fd);
}

static int check_nlun(struct shannon_dev *dev)
{

	if (dev->config->luns && dev->config->nchannel != 0xFF)
		return 0;

	printf("FAILED! I can`t know nchannel/nthread/nlun by any way. You have two choices:\n"
		"\t1, tell me by argv '-c nchannel -t nthread -l nlun';\n"
		"\t2, give me file 'config'. If you have, maybe file is broken\n"
		"Otherwise I will refuse to work!\n");
	return ERR;

}
#endif

static void do_peek_sysregs(struct shannon_dev *dev, int argc, char **argv)
{
	struct hw_sysinfo hwsys;

	dev->multi_ioread32(dev, (__u32 *)&hwsys, dev->sysreg_dwoff, sizeof(hwsys)/DW_SIZE);
	pr_u32_array_noprefix(&hwsys, sizeof(hwsys)/sizeof(__u32), sizeof(hwsys)/sizeof(__u32));
}

static void do_peek_cfgregs(struct shannon_dev *dev, int argc, char **argv)
{
	struct hw_config hwcfg;

	dev->multi_ioread32(dev, (__u32 *)&hwcfg, dev->cfgreg_dwoff, sizeof(hwcfg)/DW_SIZE);
	pr_u32_array_noprefix(&hwcfg, sizeof(hwcfg)/sizeof(__u32), sizeof(hwcfg)/sizeof(__u32));
}

/*-----------------------------------------------------------------------------------------------------------*/
#define	SUBUTILS_HEAD()			if (0) ;
#define SUBUTILS(subutils, func)	else if (!strcmp(subutils, argv[optind])) func(dev, argc - optind, &argv[optind]);
#define	SUBUTILS_CKLUN(subutils, func)	else if (!strcmp(subutils, argv[optind])) { if (check_nlun(dev)) return ERR; else func(dev, argc - optind, &argv[optind]);}
#define	SUBUTILS_TAIL()			else { shannon_utils_usage(); return ERR; }

int shannon_utils(struct shannon_dev *dev, int argc, char **argv)
{
	static struct option longopts[] = {
		{"nchannel", required_argument, NULL, 'c'},
		{"nthread", required_argument, NULL, 't'},
		{"nlun", required_argument, NULL, 'l'},
 		{0, 0, 0, 0}
	};
	int opt;
	FILE *fp = NULL;
	char line[256], *pm, *pv;

	dev->config->luns = 0;

	/* get nchannel and nlun_per_channel from argv */
	while ((opt = getopt_long(argc, argv, ":c:t:l:", longopts, NULL)) != -1) {
		switch(opt) {
		case 'c':
			dev->config->nchannel = strtoul(optarg, NULL, 10);
			break;
		case 't':
			dev->config->nthread = strtoul(optarg, NULL, 10);
			break;
		case 'l':
			dev->config->nlun = strtoul(optarg, NULL, 10);
			break;
		default:
			shannon_utils_usage();
			return ERR;
		}
	}

	dev->config->luns = dev->config->nchannel * dev->config->nthread * dev->config->nlun;
	if (0 != dev->config->luns)
		goto nchannel_nlun_known;

	/* get nchannel and nlun_per_channel from 'config' file */
	if (dev->unsafe_cfgable) {
		fp = fopen("config", "r");
		if (NULL == fp)
			goto nchannel_nlun_known;

		while (fgets(line, sizeof(line), fp) != NULL) {
			if ('#' == line[0] || '\n' == line[0] || '\n' == line[0])
				continue;

			pm = pv = line;
			while(*pv != '=')
				pv++;
			*pv = '\0';
			pv++;
			if (!strcmp("nchannel", pm))
				dev->config->nchannel = strtoul(pv, NULL, 10);
			else if (!strcmp("nthread", pm))
				dev->config->nthread = strtoul(pv, NULL, 10);
			else if (!strcmp("nlun", pm))
				dev->config->nlun = strtoul(pv, NULL, 10);

			dev->config->luns = dev->config->nchannel * dev->config->nthread * dev->config->nlun;
			if (0 != dev->config->luns)
				break;
		}

		fclose(fp);
	}
	else
	{
		dev->config->nchannel = dev->hw_nchannel;
		dev->config->nthread = dev->hw_nthread;
		dev->config->nlun = dev->hw_nlun;
	}
	dev->config->luns = dev->config->nchannel * dev->config->nthread * dev->config->nlun;

	/* utils work */
nchannel_nlun_known:
	if (argc - optind == 0) {
		shannon_utils_usage();
		return ERR;
	}

	// printf("nchannel=%d, nlun_per_channel=%d, nlun=%d\n", dev->config->nchannel, dev->config->nlun_per_channel, dev->config->nlun);
	// printf("%s\n", argv[optind]);

	SUBUTILS_HEAD()
	SUBUTILS("peek-sysregs", do_peek_sysregs)
	SUBUTILS("peek-cfgregs", do_peek_cfgregs)
#ifndef __RELEASE__
	SUBUTILS("phylun-mem", do_phylun_mem)
	SUBUTILS("peek-phylunregs", do_peek_phylunregs)
	SUBUTILS("peek-phylunmem", do_peek_phylunmem)
	SUBUTILS("peek-regs", do_peek_regs)
	SUBUTILS("peek-bufregs", do_peek_bufregs)
	SUBUTILS("peek-bufcmdq", do_peek_bufcmdq)
	SUBUTILS("peek-bufcmpq", do_peek_bufcmpq)
	SUBUTILS("parse-file", do_parse_file)
	SUBUTILS_CKLUN("peek-lunregs", do_peek_lunregs)
	SUBUTILS_CKLUN("peek-luncmdq", do_peek_luncmdq)
	SUBUTILS_CKLUN("peek-luncmpq", do_peek_luncmpq)
	SUBUTILS_CKLUN("peek-sections", do_peek_sections)
	SUBUTILS("poke-regs", do_poke_regs)
	SUBUTILS("poke-buf", do_poke_buf)
	SUBUTILS("peek-buf", do_peek_buf)
#endif
	SUBUTILS_TAIL()

	return 0;
}

/*-----------------------------------------------------------------------------------------------------------*/
int shannon_info(struct shannon_dev *dev, int argc, char **argv)
{
	printf("PCI_domains: %s\n", strchr(dev->domains, ':') + 1);

	printf("HW_nchannel: %d\n", dev->hw_nchannel);
	printf("HW_nthread: %d\n", dev->hw_nthread);
	printf("HW_nlun: %d\n", dev->hw_nlun);
	printf("HW_buffer_write_supported: %s\n", (dev->hw_sysinfo->hw_wrbuf_support & 0x0F) ? "Yes" : "No");

	printf("Flash_blocks: %d\n", dev->flash->nblk);
	printf("Flash_pages: %d\n", dev->flash->npage);
	printf("Flash_pagesize: %d\n", dev->flash->page_size);
	printf("Flash_oobsize: %d\n", dev->flash->oob_size);

	printf("CFG_nplane: %d\n", dev->config->nplane);
	printf("CFG_codeword_size: %d\n", dev->config->codeword_size);
	printf("CFG_sector_ncodeword: %d\n", dev->config->sector_ncodeword);
	printf("CFG_page_nsector: %d\n", dev->config->page_nsector);
	printf("CFG_chunk_nsector: %d\n", dev->config->chunk_nsector);

	printf("Advanced_read_supported: %s\n", dev->has_advance_read ? "Yes" : "No");
	/* wait for perfect */

	return 0;
}

int shannon_hwinfo(struct shannon_dev *dev, int argc, char **argv)
{
	printf("PCI_domains: %s\n", strchr(dev->domains, ':') + 1);
#ifndef __RELEASE__
	printf("HW_nchannel: %d\n", dev->hw_nchannel);
	printf("HW_nthread: %d\n", dev->hw_nthread);
	printf("HW_nlun: %d\n", dev->hw_nlun);
	printf("HW_iowidth: %d\n", 8 * dev->iowidth);
	printf("HW_ecc_mmode: %d\n", dev->mmode);
	printf("HW_ecc_tmode: %d\n", dev->tmode);
	printf("Firmware: %08X\n", dev->hw_sysinfo->firmware_tag);
#endif
	printf("SerialNumber: %s\n", dev->norinfo.service_tag);
	printf("ModelID: %s\n", dev->norinfo.model_id);
	printf("NodeName: %s\n", dev->name);

	return 0;
}
/*-----------------------------------------------------------------------------------------------------------*/
