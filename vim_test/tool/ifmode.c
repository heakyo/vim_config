#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>

#include "tool.h"

/*-----------------------------------------------------------------------------------------------------------*/
static __u8 *get_stepwr(struct shannon_dev *dev, struct shannon_request *wrhead, int off, int type)
{
	int idx, planeidx, planeoff;
	struct shannon_request *tmp;

	idx = 0;

	if (!type) {	// data
		planeidx = off / dev->config->ndata;
		planeoff = off % dev->config->ndata;
	} else {	// metadata
		planeidx = off / dev->config->nmeta;
		planeoff = off % dev->config->nmeta;
	}

	// printf("%s(): planeidx=%d, planeoff=%d\n", __func__, planeidx, planeoff);
	if (0 == planeidx)
		return (!type) ? (__u8 *)wrhead->data + planeoff : (__u8 *)wrhead->metadata + planeoff;

	list_for_each_entry(tmp, &wrhead->chunk_list, chunk_list) {
		if (++idx == planeidx)
			break;
	}
	if (tmp == wrhead) {
		printf("%s() %d BUG!!!\n", __func__, __LINE__);
		exit(EXIT_FAILURE);
	}

	return (!type) ? (__u8 *)tmp->data + planeoff : (__u8 *)tmp->metadata + planeoff;
}

static __u8 *get_steprd(struct shannon_dev *dev, struct shannon_request *rdhead, int off, int type)
{
	int lun, idx, planeidx, planeoff, reqidx, reqoff;
	struct shannon_request *tmp;

	idx = 0;
	lun = rdhead->lun;

	if (!type) {	// data
		planeidx = off / dev->config->ndata;
		planeoff = off % dev->config->ndata;
		reqidx = planeoff / (dev->config->sector_size * 8) + planeidx * ((dev->config->page_nsector + 8 - 1) / 8);
		reqoff = planeoff % (dev->config->sector_size * 8);
	} else {	// metadata
		planeidx = off / dev->config->nmeta;
		planeoff = off % dev->config->nmeta;
		reqidx = planeoff / (8 * 8) + planeidx * ((dev->config->page_nsector + 8 -1) / 8);
		reqoff = planeoff % (8 * 8);
	}

	// printf("%s(): reqidx=%d, reqoff=%d\n", __func__, reqidx, reqoff);
	if (0 == reqidx)
		return (!type) ? (__u8 *)rdhead->data + reqoff : (__u8 *)rdhead->metadata + reqoff;

	list_for_each_entry(tmp, &rdhead->list, list) {
		if (++idx == reqidx)
			break;
	}
	if (tmp == rdhead || tmp->lun != lun) {
		printf("%s() %d BUG!!!\n", __func__, __LINE__);
		exit(EXIT_FAILURE);
	}

	return (!type) ? (__u8 *)tmp->data + reqoff : (__u8 *)tmp->metadata + reqoff;
}

static int byte_diff_number(__u8 x, __u8 y)
{
	int i, cnt;
	__u8 mask = 1;

	for (cnt = 0, i = 0; i < 8; i++) {
		if ( (x & (mask << i)) != (y & (mask << i)) )
			cnt++;
	}

	return cnt;
}

static void compare_noecc_rdwr_requests(struct shannon_dev *dev, struct shannon_request *wrhead, struct shannon_request *rdhead, long **lun_ecc_statistics)
{
	int i, off, step, nl;
	__u8 *stepwr, *steprd;

	/* check data */
	nl = 0;
	step = 16;

	for (off = 0; off < dev->config->chunk_ndata; off += step) {
		stepwr = get_stepwr(dev, wrhead, off, 0);
		steprd = get_steprd(dev, rdhead, off, 0);

#if 0 //debug
		if (0 == random() % (65536 * 128))
			stepwr[random() % 16] = random();
#endif

		if (memcmp(stepwr, steprd, step)) {
			if (0 == nl)
				printf("\n\033[0;35m--> DATA (sb=%d lun=%d page=%d)\033[0m:\n", wrhead->chunk_block, wrhead->lun, wrhead->page);
			nl++;
			printf("0x%04X:  ", off);

			for (i = 0; i < step; i++) {
				if (stepwr[i] == steprd[i])
					printf("%02X ", stepwr[i]);
				else
					printf("\033[0;1;32m%02X\033[0m ", stepwr[i]);
			}
			printf(" |  ");

			for (i = 0; i < step; i++) {
				if (steprd[i] == stepwr[i]) {
					printf("%02X ", steprd[i]);
				} else {
					lun_ecc_statistics[wrhead->lun][0] += byte_diff_number(stepwr[i], steprd[i]);
					printf("\033[0;1;31m%02X\033[0m ", steprd[i]);
				}
			}
			printf("\n");
		}
	}

	/* check metadata */
	nl = 0;
	step =8;

	for (off = 0; off < dev->config->chunk_nmeta; off += step) {
		stepwr = get_stepwr(dev, wrhead, off, 1);
		steprd = get_steprd(dev, rdhead, off, 1);

#if 0 //debug
		if (0 == random() % 65536)
			stepwr[random() % 16] = random();
#endif

		if (memcmp(stepwr, steprd, step)) {
			if (0 == nl)
				printf("\n\033[0;34m--> METADATA (sb=%d lun=%d page=%d)\033[0m:\n", wrhead->chunk_block, wrhead->lun, wrhead->page);
			nl++;
			printf("0x%04X:  ", off);

			for (i = 0; i < step; i++) {
				if (stepwr[i] == steprd[i])
					printf("%02X ", stepwr[i]);
				else
					printf("\033[0;1;32m%02X\033[0m ", stepwr[i]);
			}
			printf(" |  ");

			for (i = 0; i < step; i++) {
				if (steprd[i] == stepwr[i]) {
					printf("%02X ", steprd[i]);
				} else {
					lun_ecc_statistics[wrhead->lun][0] += byte_diff_number(stepwr[i], steprd[i]);
					printf("\033[0;1;31m%02X\033[0m ", steprd[i]);
				}
			}
			printf("\n");
		}
	}
}

static int memcmp_hlbyte(__u8 *buf1, __u8 *buf2, unsigned int count, int HLBYTE)
{
	int i = 0;
	int status = 0;
	int startPos = 0;

	if (HLBYTE != LBYTE && HLBYTE != HBYTE)
		return memcmp(buf1, buf2, count);

	startPos = (LBYTE == HLBYTE ? 0 : 1);

	for (i = startPos; i < count; i+=2) {
		if (buf1[i] != buf2[i]) {
				status = (buf1[i] < buf2[i] ? -1 : 1);
				break;
			}
	}

	return status;
}

static void compare_noecc_rdwr_requests_hlpage(struct shannon_dev *dev, struct shannon_request *wrhead, struct shannon_request *rdhead, long **lun_ecc_statistics, int HLBYTE)
{
	int i, off, step;
	__u8 *stepwr, *steprd;
	int startPos;

	/* check data */
	step = 16;
	startPos = 0;

	if (HBYTE != HLBYTE && LBYTE != HLBYTE) {
		printf("\n%s() %d BUG!!!\n", __func__, __LINE__);
		exit(EXIT_FAILURE);
	}

	startPos = ((LBYTE == HLBYTE) ? 0 : 1);

	for (off = 0; off < dev->config->chunk_ndata; off += step) {
		stepwr = get_stepwr(dev, wrhead, off, 0);
		steprd = get_steprd(dev, rdhead, off, 0);

		if (memcmp_hlbyte(stepwr, steprd, step, HLBYTE)) {
			for (i = startPos; i < step; i += 2) {
				if (steprd[i] != stepwr[i])
					lun_ecc_statistics[wrhead->lun][0] += byte_diff_number(stepwr[i], steprd[i]);
			}
		}
	}

	/* check metadata */
	step =8;

	for (off = 0; off < dev->config->chunk_nmeta; off += step) {
		stepwr = get_stepwr(dev, wrhead, off, 1);
		steprd = get_steprd(dev, rdhead, off, 1);

		if (memcmp_hlbyte(stepwr, steprd, step, HLBYTE)) {
			for (i = startPos; i < step; i += 2) {
				if (steprd[i] != stepwr[i])
					lun_ecc_statistics[wrhead->lun][0] += byte_diff_number(stepwr[i], steprd[i]);
			}
		}
	}
}

/*-----------------------------------------------------------------------------------------------------------*/
static void shannon_ifmode_usage(void)
{
	printf("Description:\n");
	printf("\tUse dummy write/cacheread to verify hardware interface of data transmission\n\n");

	printf("Usage:\n");
	printf("\tifmode begin_chunkblock count [option]\n\n");

	printf("Options:\n");
	printf("\t-e, seed=SEED\n"
		"\t\tSet seed of random data write to flash. Default is gettimeofday.tv_usec\n\n");
	printf("\t-w, --write-ifmode=[async/sync]\n"
		"\t\tSet write interface as async or sync mode\n\n");
	printf("\t-r, --read-ifmode=[async/sync]\n"
		"\t\tSet read interface as async or sync mode\n\n");
	printf("\t-t, --absent-luns=loglun|phylun:x1-x2,x3,x4-x5...\n"
		"\t\tSpecify absent luns\n\n");
	printf("\t-T, --present-luns=loglun|phylun:x1-x2,x3,x4-x5...\n"
		"\t\tSpecify present luns\n\n");
	printf("\t-o, --print-old-format\n"
		"\t\tPrint format like old tool\n\n");
	printf("\t-N, --noprogress\n"
		"\t\tDisable show progress\n\n");
	printf("\t-b, --write-fixed-byte=BYTE(hex)\n"
		"\t\tWrite fixed value BYTE instead of random data\n\n");
	printf("\t-i, --write-inc-byte\n"
		"\t\tWrite increased BYTE instead of random data\n\n");
	printf("\t-s, --highbyte-same-lowbyte\n"
		"\t\tHigh byte and low byte are same\n\n");
	printf("\t-n, --highbyte-not-lowbyte\n"
		"\t\tHigh byte and low byte are reverse\n\n");
	printf("\t-k, --evenclock-not-oddclock\n"
		"\t\tData at evenclock and data at oddclock on the flash IO pins are reverse\n\n");
	printf("\t-l, --lun-total-ecc-limit=N\n"
		"\t\tUsed for product, if lun total ecc larger than this value, print 'ShouldBeFenced' for this lun\n\n");
	printf("\t-m, --lun-max-ecc-limit=N\n"
		"\t\tUsed for product, if lun max ecc larger than this value, print 'ShouldBeFenced' for this lun\n\n");
	printf("\t-g, --logfile=FILE\n"
		"\t\tWrite logfile\n\n");
	printf("\t-d, --draw-lun-map\n"
		"\t\tDisplay the status of all the packages on the subcard. It must be used with global option dev-type\n\n");
	printf("\t-y, --high-low-byte=N\n"
		"\t\tchoose low/high byte with ecc closed: 0->low byte, 1->high byte, 2->high-low byte\n\n");
	printf("\t-h, --help\n"
		"\t\tDisplay this help and exit\n\n");

	printf("Examples:\n");
	printf("\tifmode 0 1 -t loglun:3-6,7,9\n"
		"\t\tdo super-block dummy write/read but skip logical luns 3 4 5 6 7 9\n\n");
	printf("\tifmode 0 1 -t phylun:3,5-7,12\n"
		"\t\tdo super-block dummy write/read but skip physical luns 3 5 6 7 12\n\n");
	printf("\tifmode 0 1 -T loglun:2,7,10-12\n"
		"\t\tdo super-block dummy write/read but just for logical luns 2 7 10 11 12\n\n");

	printf("NOTE:\n\tBlock index is count by chunk\n");		
}

#define	logout(x...)	do { printf(x); if (NULL != logfp) fprintf(logfp, x); } while (0)

int get_pkg_index(int phylun, const unsigned char pkg_phylun_num[][8+PKG_ATTR], int HLBYTE)
{
	int pkg_cnt = 0;
	int pkg_nluns = 0;
	const unsigned char *pkg = NULL;

	for (pkg_cnt = 0; pkg_cnt < TOTAL_PKG_CNT; pkg_cnt++) {

		pkg = pkg_phylun_num[pkg_cnt];

		if (HLBYTE != pkg[8])
			continue;

		for (pkg_nluns = 0; pkg_nluns < PKG_LUNS; pkg_nluns++) {
			if (phylun == pkg[pkg_nluns])
				return pkg_cnt;
		}
	}

	return -1;

}

int shannon_ifmode(struct shannon_dev *dev, int argc, char **argv)
{
	struct option longopts [] = {
		{"seed", required_argument, NULL, 'e'},
		{"write-ifmode", required_argument, NULL, 'w'},
		{"read-ifmode", required_argument, NULL, 'r'},
		{"absent-luns", required_argument, NULL, 't'},
		{"present-luns", required_argument, NULL, 'T'},
		{"print-old-format", no_argument, NULL, 'o'},
		{"noprogress", no_argument, NULL, 'N'},
		{"write-fixed-byte", required_argument, NULL, 'b'},
		{"write-inc-byte", no_argument, NULL, 'i'},
		{"highbyte-same-lowbyte", no_argument, NULL, 's'},
		{"highbyte-not-lowbyte", no_argument, NULL, 'n'},
		{"evenclock-not-oddclock", no_argument, NULL, 'k'},
		{"lun-total-ecc-limit", required_argument, NULL, 'l'},
		{"lun-max-ecc-limit", required_argument, NULL, 'm'},
		{"logfile", required_argument, NULL, 'g'},
		{"draw-lun-map", no_argument, NULL, 'd'},
		{"high-low-byte", required_argument, NULL, 'y'},
		{"help", no_argument, NULL, 'h'},
		{0, 0, 0, 0},
	};
	int opt;
	int seed, wrifmode, rdifmode, pr_old_format;
	int i, rc = 0;
	int blk, lun, plane, page, ppa;
	int begin_chunkblock, count;
	int head;
	struct shannon_request *chunk_head_req, *req, *tmp;
	struct list_head req_head;
	long **lun_ecc_statistics;
	int noprogress, pre_cent, now_cent;		// used for show progress
	struct shannon_request *wrhead = NULL, *rdhead = NULL;
	int boundary, fixed;
	int hi_same_lo = 0, hi_not_lo = 0, even_not_odd = 0;
	int inc = 0;
	long lun_total_ecc_limit = 0x7FFFFFFFFFFFFFFFl;
	FILE *logfp = NULL;
	int lun_max_ecc_limit = 0xFB-1;
	int draw_lun = 0;
	int hlbyte = BYTE_TOTLE;
	int hlbyte_pbd = BYTE_TOTLE;
	const unsigned char dev_pkg_phylun_num[TOTAL_PKG_CNT][8+PKG_ATTR];
	const char *pkg_partRef_map[TOTAL_PKG_CNT];

	switch (dev->dev_type) {
	default:
	case K7F_MAP:
		memcpy(dev_pkg_phylun_num, dev_pkg_phylun_k7f, sizeof(dev_pkg_phylun_num));
		memcpy(pkg_partRef_map, pkg_partRef_k7f_map, sizeof(pkg_partRef_map));
		break;
	case K7h_DUAL_MAP:
		memcpy(dev_pkg_phylun_num, dev_pkg_phylun_k7h_dual, sizeof(dev_pkg_phylun_num));
		memcpy(pkg_partRef_map, pkg_partRef_k7h_dual_map, sizeof(pkg_partRef_map));
		break;
	case FIJI_MAP:
		memcpy(dev_pkg_phylun_num, dev_pkg_phylun_fiji, sizeof(dev_pkg_phylun_num));
		memcpy(pkg_partRef_map, pkg_partRef_fiji_map, sizeof(pkg_partRef_map));
		break;
	}

	if (1 == dev->per_byte_disable) {
		hlbyte_pbd = HBYTE;
	} else if (2 == dev->per_byte_disable) {
		hlbyte_pbd = LBYTE;
	}

	seed = getseed(0);
	wrifmode = rdifmode = dev->config->ifmode;
	pr_old_format = 0;
	noprogress = 0;
	fixed = -1;

	while ((opt = getopt_long(argc, argv, ":e:w:r:t:T:oNb:isnkl:m:g:dy:h", longopts, NULL)) != -1) {
		switch (opt) {
		case 'e':
			seed = strtoul(optarg, NULL, 10);
			break;
		case 'w':
			if (!strcmp("sync", optarg) || !strcmp("SYNC", optarg)) {
				wrifmode = IFMODE_SYNC;
			} else if (!strcmp("async", optarg) || !strcmp("ASYNC", optarg)) {
				wrifmode = IFMODE_ASYNC;
			} else {
				printf("[-w, --write-ifmode] must be 'async' or 'sync'\n");
				return ERR;
			}
			break;
		case 'r':
			if (!strcmp("sync", optarg) || !strcmp("SYNC", optarg)) {
				rdifmode = IFMODE_SYNC;
			} else if (!strcmp("async", optarg) || !strcmp("ASYNC", optarg)) {
				rdifmode = IFMODE_ASYNC;
			} else {
				printf("[-r, --read-ifmode] must be 'async' or 'sync'\n");
				return ERR;
			}
			break;
		case 't':
			present_absent_luns(dev, optarg, 1);
			break;
		case 'T':
			present_absent_luns(dev, optarg, 0);
			break;
		case 'o':
			pr_old_format = 1;
			break;
		case 'N':
			noprogress = 1;
			break;
		case 'b':
			fixed = strtoul(optarg, NULL, 16);
			break;
		case 'i':
			inc = 1;
			break;
		case 's':
			hi_same_lo = 1;
			break;
		case 'n':
			hi_not_lo = 1;
			break;
		case 'k':
			even_not_odd = 1;
			break;
		case 'l':
			lun_total_ecc_limit = atoi(optarg);
			if (0 == lun_total_ecc_limit) {
				printf("option -l, --lun-total-ecc-limit should not be 0!\n");
				exit(EXIT_FAILURE);
			}
			break;
		case 'm':
			lun_max_ecc_limit = atoi(optarg);
			if (0 == lun_max_ecc_limit) {
				printf("option -m, --lun-max-ecc-limit should not be 0!\n");
				exit(EXIT_FAILURE);
			}
			break;
		case 'g':
			logfp = strncmp(optarg, "+", 1) ? fopen(optarg, "w") : fopen(optarg + 1, "a");
			if (NULL == logfp) {
				perror("ifmode open logfile error:");
				exit(EXIT_FAILURE);
			} else {
				time_t tt;
				tt = time(NULL);
				fprintf(logfp, "[IFMODE LOG BEG %s] --> %s", dev->name, ctime(&tt));
			}
			break;
		case 'd':
			draw_lun = 1;
			break;
		case 'y':
			hlbyte = atoi(optarg);
			break;
		case 'h':
			shannon_ifmode_usage();
			return 0;
		default:
			shannon_ifmode_usage();
			return ERR;
		}
	}

	if (argc - optind != 2) {
		shannon_ifmode_usage();
		return ERR;
	}

	if (draw_lun && (DEV_TOTAL == dev->dev_type)) {
		shannon_ifmode_usage();
		return ERR;
	}

	if (hi_same_lo && hi_not_lo) {
		printf("option --highbyte-same-lowbyte and --highbyte-not-lowbyte are mutex!\n");
		exit(EXIT_FAILURE);
	}

	begin_chunkblock = strtoul(argv[optind], NULL, 10);
	count = strtoul(argv[optind + 1], NULL, 10);
	if ((begin_chunkblock + count) > dev->flash->nblk/dev->config->nplane) {
		printf("Overflow address\n");
		return OVERFLOW_ADDR;
	}

	/* statistics ecc result init */
	lun_ecc_statistics = zmalloc(dev->config->luns * sizeof(*lun_ecc_statistics));
	if (NULL == lun_ecc_statistics) {
		rc = ALLOCMEM_FAILED;
		goto out;
	}
	for_dev_each_lun(dev, lun) {
		lun_ecc_statistics[lun] = zmalloc(256 * sizeof(lun_ecc_statistics[lun]));
		if (NULL == lun_ecc_statistics[lun]) {
			rc = ALLOCMEM_FAILED;
			goto free_lun_ecc_statistics;
		}
	}

	head = INDEP_HEAD;
	if (dev->config->nplane > 1)
		head |= (1 << SH_WRITE_PLANE_SHIFT);

	if (!noprogress) {
		pre_cent = now_cent = 0;
		print("Super-ifmode in process...%%%02d", now_cent);
	}

	INIT_LIST_HEAD(&req_head);
	srand(seed);

	for (blk = begin_chunkblock; blk < begin_chunkblock + count; blk++) {
		ppa = blk * dev->flash->npage * dev->config->nplane;
		page = 0;

next_block_page:
		for_dev_each_lun(dev, lun) {
			/* dummy write requests */
			rc = (IFMODE_ASYNC == wrifmode) ? shannon_super_async(dev) : shannon_super_sync(dev);
			if (rc)
				goto free_req_out;

			chunk_head_req = alloc_request(dev, sh_write_cmd, lun, ppa + page,
						head | (1 << SH_WRITE_DUMMY_SHIFT), 0, dev->config->page_nsector);
			if (NULL == chunk_head_req) {
				rc = ALLOCMEM_FAILED;
				goto free_req_out;
			}

			if (inc) {
				for (i = 0; i < dev->config->ndata; i++)
					chunk_head_req->data[i] = i;
				for (i = 0; i < dev->config->nmeta; i++)
					((__u8 *)chunk_head_req->metadata)[i] = i;
			} else if (-1 != fixed) {
				memset(chunk_head_req->data, fixed, dev->config->ndata);
				memset(chunk_head_req->metadata, fixed, dev->config->nmeta);
			} else {
				pad_rand(chunk_head_req->data, dev->config->ndata);
				pad_rand(chunk_head_req->metadata, dev->config->nmeta);
			}

			if (hi_same_lo || hi_not_lo) {
				for (i = 1; i < dev->config->ndata; i+=2)
					chunk_head_req->data[i] = hi_same_lo ? chunk_head_req->data[i-1] : ~chunk_head_req->data[i-1];
				for (i = 1; i < dev->config->nmeta; i+=2)
					((__u8 *)chunk_head_req->metadata)[i] = hi_same_lo ? ((__u8 *)chunk_head_req->metadata)[i-1] : ~((__u8 *)chunk_head_req->metadata)[i-1];
			}

			if (even_not_odd) {
				if (2 == dev->iowidth) {
					for (i = 1; i < dev->config->ndata/2; i+=2)
						((__u16 *)chunk_head_req->data)[i] = ~((__u16 *)chunk_head_req->data)[i-1];
					for (i = 1; i < dev->config->nmeta/2; i+=2)
						((__u16 *)chunk_head_req->metadata)[i] = ~((__u16 *)chunk_head_req->metadata)[i-1];
				} else {
					for (i = 1; i < dev->config->ndata; i+=2)
						((__u8 *)chunk_head_req->data)[i] = ~((__u8 *)chunk_head_req->data)[i-1];
					for (i = 1; i < dev->config->nmeta; i+=2)
						((__u8 *)chunk_head_req->metadata)[i] = ~((__u8 *)chunk_head_req->metadata)[i-1];
				}
			}
			list_add_tail(&chunk_head_req->list, &req_head);

			for (plane = 1; plane < dev->config->nplane; plane++) {
				req = alloc_request(dev, sh_write_cmd, lun, ppa + plane * dev->flash->npage + page,
						head | (1 << SH_WRITE_DUMMY_SHIFT), 0, dev->config->page_nsector);
				if (NULL == req) {
					rc = ALLOCMEM_FAILED;
					goto free_req_out;
				}

				if (inc) {
					for (i = 0; i < dev->config->ndata; i++)
						req->data[i] = i;
					for (i = 0; i < dev->config->nmeta; i++)
						((__u8 *)req->metadata)[i] = i;
				} else if (-1 != fixed) {
					memset(req->data, fixed, dev->config->ndata);
					memset(req->metadata, fixed, dev->config->nmeta);
				} else {
					pad_rand(req->data, dev->config->ndata);
					pad_rand(req->metadata, dev->config->nmeta);
				}

				if (hi_same_lo || hi_not_lo) {
					for (i = 1; i < dev->config->ndata; i+=2)
						req->data[i] = hi_same_lo ? req->data[i-1] : ~req->data[i-1];
					for (i = 1; i < dev->config->nmeta; i+=2)
						((__u8 *)req->metadata)[i] = hi_same_lo ? ((__u8 *)req->metadata)[i-1] : ~((__u8 *)req->metadata)[i-1];
				}

				if (even_not_odd) {
					if (2 == dev->iowidth) {
						for (i = 1; i < dev->config->ndata/2; i+=2)
							((__u16 *)req->data)[i] = ~((__u16 *)req->data)[i-1];
						for (i = 1; i < dev->config->nmeta/2; i+=2)
							((__u16 *)req->metadata)[i] = ~((__u16 *)req->metadata)[i-1];
					} else {
						for (i = 1; i < dev->config->ndata; i+=2)
							((__u8 *)req->data)[i] = ~((__u8 *)req->data)[i-1];
						for (i = 1; i < dev->config->nmeta; i+=2)
							((__u8 *)req->metadata)[i] = ~((__u8 *)req->metadata)[i-1];
					}
				}
				list_add_tail(&req->chunk_list, &chunk_head_req->chunk_list);
			}

			/* cacheread requests */
			rc = (IFMODE_ASYNC == rdifmode) ? shannon_super_async(dev) : shannon_super_sync(dev);
			if (rc)
				goto free_req_out;

			for (plane = 0; plane < dev->config->nplane; plane++) {
				int ns, bs = 0;
				int remain_ns = dev->config->page_nsector;

				while (remain_ns) {
					ns = (remain_ns >= 8) ? 8 : remain_ns;

					if (ECCMODE_DISABLE == dev->config->ecc_mode)
						req = alloc_request(dev, sh_cacheread_cmd, lun, ppa + plane * dev->flash->npage + page, head, bs, ns);
					else
						req = alloc_request(dev, sh_cacheread_cmd, lun, ppa + plane * dev->flash->npage + page, head, bs, 0);
					if (NULL == req) {
						rc = ALLOCMEM_FAILED;
						goto free_req_out;
					}
					req->nsector = ns;
					list_add_tail(&req->list, &req_head);

					bs += ns;
					remain_ns -= ns;
				}
			}
		}

		/* summit and execute all request */
		list_for_each_entry(req, &req_head, list) {
			if ((rc = dev->submit_request(req)))
				goto free_req_out;
		}

		for_dev_each_lun(dev, lun)
			update_cmdqueue(dev, lun);

		for_dev_each_lun(dev, lun) {
			if ((rc = poll_cmdqueue(dev, lun)))
				goto free_req_out;
		}

		/* check ecc result */
		if (ECCMODE_DISABLE == dev->config->ecc_mode) {
			boundary = 0;

			list_for_each_entry(req, &req_head, list) {
				if (sh_write_cmd == req->opcode) {
					boundary = 1;
					wrhead = req;
					check_req_status(req);
					continue;
				}

				if (boundary) {
					boundary = 0;
					rdhead = req;	// this is 1st request cacheread for one chunk
					if (HBYTE == hlbyte || LBYTE == hlbyte) {
						compare_noecc_rdwr_requests_hlpage(dev, wrhead, rdhead, lun_ecc_statistics, hlbyte);
					} else {
						compare_noecc_rdwr_requests(dev, wrhead, rdhead, lun_ecc_statistics);
					}
				}
			}
		} else {
			list_for_each_entry(req, &req_head, list) {
				if (sh_write_cmd == req->opcode) {
					check_req_status(req);
				} else if (sh_cacheread_cmd == req->opcode){
					for (i = 0; i < req->nsector; i++)
						lun_ecc_statistics[req->lun][req->ecc[i]]++;
				}
			}
		}

		list_for_each_entry_safe(req, tmp, &req_head, list) {
			list_del(&req->list);
			free_request(req);
		}

		/* print progress if needed */
		if (!noprogress) {
			now_cent = 100 * ((blk - begin_chunkblock) * dev->flash->npage + page + 1) / (count * dev->flash->npage);

			if (now_cent > pre_cent) {
				print("\b\b\b%%%02d", now_cent);
				pre_cent = now_cent;
			}
		}

		if (++page < dev->flash->npage)
			goto next_block_page;
	}

	if (!noprogress)
		printf("\n");

	/* calculate ecc statistics */
	if (ECCMODE_DISABLE == dev->config->ecc_mode) {
		long errbits = 0;

		for_dev_each_lun(dev, lun) {
			if (0 == lun_ecc_statistics[lun][0])
				continue;

			printf("#lun-%03d phylun-%03d *hwchannel-%02d hwthread-%02d hwlun-%02d ERR BITS=%ld\n", lun, log2phy_lun(dev, lun),
				get_phychannel(dev, lun), get_phythread(dev, lun), get_phylun(dev, lun), lun_ecc_statistics[lun][0]);
			errbits += lun_ecc_statistics[lun][0];
		}
		printf("ERR BITS SUM: %ld\n", errbits);
	} else if (pr_old_format) {
		long total_bits = 0;

		for_dev_each_lun(dev, lun) {
			long lun_total = 0;
			int lun_should_fenced = 0;

			for (i = 0; i < 256; i++)
				lun_total += i * lun_ecc_statistics[lun][i];
			if (0 == lun_total)
				continue;
			total_bits += lun_total;
			logout("#lun-%03d phylun-%03d *hwchannel-%02d hwthread-%02d hwlun-%02d ECC sum=%ld", lun, log2phy_lun(dev, lun),
				get_phychannel(dev, lun), get_phythread(dev, lun), get_phylun(dev, lun), lun_total);
			for (i = 1; i < 256; i++) {
				if (lun_ecc_statistics[lun][i]) {
					logout(" [%d]=%ld", i, lun_ecc_statistics[lun][i]);
					if (i > lun_max_ecc_limit)
						lun_should_fenced = 1;
				}
			}
			printf("%s\n", ((lun_total > lun_total_ecc_limit) || lun_should_fenced) ? " \033[0;33mShouldBeFenced\033[m" : "");
			if (NULL != logfp)
				fprintf(logfp, "%s\n", ((lun_total > lun_total_ecc_limit) || lun_should_fenced) ? " ShouldBeFenced" : "");
		}
		logout("Sum of ECC bit is: %ld\n", total_bits);
	} else {
		for_dev_each_lun(dev, lun) {
			logout("#lun-%02d phylun-%02d *hwchannel-%02d hwthread-%02d hwlun-%02d ECC result:\n", lun, log2phy_lun(dev, lun),
				get_phychannel(dev, lun), get_phythread(dev, lun), get_phylun(dev, lun));

			for (i = 0; i < 256; i++) {
				if (lun_ecc_statistics[lun][i])
					logout("ecc[%2d] = %-8ld %%%.1f\n", i, lun_ecc_statistics[lun][i],
						(100.0 * lun_ecc_statistics[lun][i]) / (count * dev->flash->npage  * dev->config->chunk_nsector));
			}
			logout("\n");
		}

		logout("#Global ECC result:\n");
		for (i = 0; i < 256; i++) {
			long strip_cnt = 0;
			int vluns = 0;

			for_dev_each_lun(dev, lun) {
				strip_cnt += lun_ecc_statistics[lun][i];
				vluns++;
			}

			if (strip_cnt)
				logout("ecc[%2d] = %-8ld %%%.1f\n", i, strip_cnt,
					(100.0 * strip_cnt) / (vluns * count * dev->flash->npage  * dev->config->chunk_nsector));
		}
	}

	if (draw_lun) {
		int sx = LUN_MAP_SX, sy = LUN_MAP_SY;
		int row = SUBCARD_ROW, col = SUBCARD_COL;
		int ret_posX = 0, ret_posY = sy+2*row*(INNER_ROW_SPAN+WIDTH)+UP_DOWN_SPAN+2;

		char phy_lun_map_color[BYTE_TOTLE][256];
		char subcard_color_pkg[SUBCARD_CNT][SUBCARD_ROW*SUBCARD_COL];
		int subcard_pkg_cnt = 0;
		int subcard_cnt = 0;
		int pkg_lb_index = 0;
		int pkg_hb_index = 0;
		int pkg_index = 0;

		long lun_total = 0;
		long total_bits = 0;
		int lun_should_fenced = 0;
		int phylun = 0;

		memset(phy_lun_map_color, WHITE, sizeof(phy_lun_map_color));
		memset(subcard_color_pkg, WHITE, sizeof(subcard_color_pkg));

		if (ECCMODE_DISABLE == dev->config->ecc_mode) {
			for_dev_each_lun(dev, lun) {
				lun_total = 0;

				phylun = log2phy_lun(dev, lun);

				lun_total += lun_ecc_statistics[lun][0];

				if (lun_total <= lun_total_ecc_limit) {
					if (HBYTE == hlbyte || LBYTE == hlbyte) {
						phy_lun_map_color[hlbyte][phylun] = GREEN;
					} else {
						phy_lun_map_color[LBYTE][phylun] = GREEN;
						phy_lun_map_color[HBYTE][phylun] = GREEN;
					}
					continue;
				}

				if (lun_total > lun_total_ecc_limit) {
					if (HBYTE == hlbyte || LBYTE == hlbyte) {
						phy_lun_map_color[hlbyte][phylun] = RED;

						pkg_index = get_pkg_index(phylun, dev_pkg_phylun_num, hlbyte);
						logout("\nlun-%03d phylun-%03d partRef-%s\n", lun, log2phy_lun(dev, lun),
							pkg_partRef_map[pkg_index]);

					} else {
						phy_lun_map_color[LBYTE][phylun] = RED;
						phy_lun_map_color[HBYTE][phylun] = RED;

						pkg_lb_index = get_pkg_index(phylun, dev_pkg_phylun_num, LBYTE);
						pkg_hb_index = get_pkg_index(phylun, dev_pkg_phylun_num, HBYTE);
						logout("\nlun-%03d phylun-%03d partRef-%s %s\n", lun, log2phy_lun(dev, lun),
							pkg_partRef_map[pkg_lb_index], pkg_partRef_map[pkg_hb_index]);
					}
				}
			}
		} else {
			for_dev_each_lun(dev, lun) {
				lun_total = 0;
				lun_should_fenced = 0;

				phylun = log2phy_lun(dev, lun);

				for (i = 0; i < 256; i++)
					lun_total += i * lun_ecc_statistics[lun][i];

				if (lun_total <= lun_total_ecc_limit) {
					if (HBYTE == hlbyte_pbd || LBYTE == hlbyte_pbd) {
						phy_lun_map_color[hlbyte_pbd][phylun] = GREEN;
					} else {
						phy_lun_map_color[LBYTE][phylun] = GREEN;
						phy_lun_map_color[HBYTE][phylun] = GREEN;
					}
					continue;
				}
				total_bits += lun_total;

				for (i = 1; i < 256; i++) {
					if (lun_ecc_statistics[lun][i]) {
						if (i > lun_max_ecc_limit)
							lun_should_fenced = 1;
					}
				}

				if ((lun_total > lun_total_ecc_limit) || lun_should_fenced) {
					if (HBYTE == hlbyte_pbd || LBYTE == hlbyte_pbd) {
						phy_lun_map_color[hlbyte_pbd][phylun] = RED;

						pkg_index = get_pkg_index(phylun, dev_pkg_phylun_num, hlbyte_pbd);
						logout("\nlun-%03d phylun-%03d partRef-%s\n", lun, log2phy_lun(dev, lun),
							pkg_partRef_map[pkg_index]);

					} else {
						phy_lun_map_color[LBYTE][phylun] = RED;
						phy_lun_map_color[HBYTE][phylun] = RED;

						pkg_lb_index = get_pkg_index(phylun, dev_pkg_phylun_num, LBYTE);
						pkg_hb_index = get_pkg_index(phylun, dev_pkg_phylun_num, HBYTE);
						logout("\nlun-%03d phylun-%03d partRef-%s %s\n", lun, log2phy_lun(dev, lun),
							pkg_partRef_map[pkg_lb_index], pkg_partRef_map[pkg_hb_index]);
					}
				}
			}
		}

		for (subcard_cnt = 0; subcard_cnt < SUBCARD_CNT; subcard_cnt++)
			for (subcard_pkg_cnt = 0; subcard_pkg_cnt < SUBCARD_PKGS; subcard_pkg_cnt++)
				subcard_color_pkg[subcard_cnt][subcard_pkg_cnt] =
					get_pkg_color(dev_pkg_phylun_num[subcard_cnt * SUBCARD_PKGS + subcard_pkg_cnt], phy_lun_map_color);

		reset_cursor();
		clear_screen();

		draw_lun_map(sx, sy, row, col, pkg_partRef_map, (char *)subcard_color_pkg);

		set_cursor(ret_posX, ret_posY);
	}

	/* success return */
free_req_out:
	list_for_each_entry_safe(req, tmp, &req_head, list) {
		list_del(&req->list);
		free_request(req);
	}
free_lun_ecc_statistics:
	for_dev_each_lun(dev, lun) {
		if (NULL == lun_ecc_statistics[lun])
			break;
		free(lun_ecc_statistics[lun]);
	}
	free(lun_ecc_statistics);
out:
	if (NULL != logfp) {
		time_t tt;
		tt = time(NULL);
		fprintf(logfp, "[IFMODE LOG END %s] <-- %s", dev->name, ctime(&tt));
		fclose(logfp);
	}

	if (rc != FAILED_FLASH_STATUS && rc != 0)
		printf("Ifmode encounter some error\n");
	return rc;
}

/*-----------------------------------------------------------------------------------------------------------*/
