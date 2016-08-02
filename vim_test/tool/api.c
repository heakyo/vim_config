#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>

#include "tool.h"

/*-----------------------------------------------------------------------------------------------------------*/
static char *cmd_string(enum shannon_cmd cmd)
{
	if (sh_write_cmd == cmd)
		return "Write";
	else if (sh_preread_cmd == cmd)
		return "Preread";
	else if (sh_erase_cmd == cmd)
		return "Erase";
	else if (sh_reset_cmd == cmd)
		return "Reset";
	else if (sh_bufwrite_cmd == cmd)
		return "Bufwrite";
	else
		return "Unknown";
}

static int __check_req_status(struct shannon_request *req, int pr)
{
	int is_multi_plane = 0;
	int n, failed;
	char status[32];
	struct shannon_request *tmp;
	struct shannon_dev *dev = req->dev;

	/* pre-process */
	if (sh_bufwrite_cmd == req->opcode) {
		if (req->status != ~0llu) {
			return 0;
		} else {
			if (pr)
				printf("%s : failed lun=%02d phylun=%02d block=%d page=%d sector=%d\n",
					cmd_string(req->opcode), req->lun, log2phy_lun(dev, req->lun), req->chunk_block, req->page, req->bsector);
			return FAILED_FLASH_STATUS;
		}
	}

	if (!list_empty(&req->chunk_list))
		is_multi_plane = 1;

	//chunk is only single plane
	if (!is_multi_plane) {
		if (check_status(dev, req->opcode, req->status)) {
			return 0;
		} else {
			if (pr) {
				printf("%s: failed status %02X. lun=%02d phylun=%02d", cmd_string(req->opcode), (__u8)req->status, req->lun, log2phy_lun(dev, req->lun));

				if (req->opcode == sh_write_cmd ||
					req->opcode == sh_preread_cmd ||
					req->opcode == sh_erase_cmd)
					printf(" block=%d", req->block);

				if (req->opcode == sh_write_cmd ||
					req->opcode == sh_preread_cmd)
					printf(" page=%d", req->page);

				printf("\n");
			}
			return FAILED_FLASH_STATUS;
		}
	} 

	// chunk is multi plane
	n = 0;
	failed = 0;
	memset(status, 0, sizeof(status));

	n += sprintf(status + n, " %02X", (__u8)req->status);
	if (!check_status(dev, req->opcode, req->status))
		failed = 1;

	list_for_each_entry(tmp, &req->chunk_list, chunk_list) {
		n += sprintf(status + n, " %02X", (__u8)tmp->status);
		if (!check_status(dev, req->opcode, tmp->status))
			failed = 1;
	}

	if (!failed) {
		return 0;
	} else {
		if (pr) {	// just write, pre-read and earse have multi-plane requests
			printf("MP-%s: failed status%s. lun=%d phylun=%d block=%d",
				cmd_string(req->opcode), status, req->lun, log2phy_lun(dev, req->lun), req->chunk_block);

			if (req->opcode != sh_erase_cmd)
				printf(" page=%d", req->page);

			printf("\n");
		}
		return FAILED_FLASH_STATUS;
	}
}

int check_req_status(struct shannon_request *req)
{
	return __check_req_status(req, 1);
}

int check_req_status_silent(struct shannon_request *req)
{
	return __check_req_status(req, 0);
}

/*-----------------------------------------------------------------------------------------------------------*/
struct wr_flash_regs {
	__u8 cmd;
	__u8 addr;

	int has_secondary_cmd;
	__u8 secondary_cmd;

	int ndata;
	__u8 data[16];

	int vendory_mode;
};

static int write_flash_registers(struct shannon_dev *dev, int lun, struct wr_flash_regs *wd)
{
	int i;
	struct shannon_request *req;

	req = alloc_request(dev, sh_writereg_cmd, lun, 0, 0, 0, 0);
	if (NULL == req) {
		printf("%s() %d: malloc fail\n", __func__, __LINE__);
		exit(EXIT_FAILURE);
	}
	req->wr_flash_reg_nbyte = wd->ndata;

	memset(req->direct_cmdqueue, 0x00, sizeof(req->direct_cmdqueue));
	req->direct_cmdqueue[0] = sh_writereg_cmd;	// opcode
	req->direct_cmdqueue[1] = wd->secondary_cmd;	// 2nd flash command
	req->direct_cmdqueue[2] = wd->data[0];
	if (wd->vendory_mode)
		req->direct_cmdqueue[3] |= 0x20;
	if (wd->has_secondary_cmd)
		req->direct_cmdqueue[3] |= 0x40;
	req->direct_cmdqueue[4] = wd->addr;		// flash address
	req->direct_cmdqueue[5] = wd->ndata;		// nbyte to transfer
	req->direct_cmdqueue[6] = wd->cmd;		// 1st flash command
	req->direct_cmdqueue[7] = log2phy_lun(dev, lun);// lun number
	if (wd->ndata > 1) {
		for (i = 0; i < wd->ndata; i++)
			req->direct_cmdqueue[8 + i] = wd->data[i];	// data to write
	}

	if (dev->submit_request(req)) {
		printf("%s() %d: submit fail\n", __func__, __LINE__);
		exit(EXIT_FAILURE);
	}

	if (update_poll_cmdqueue(dev, lun)) {
		printf("%s() %d: poll completion fail\n", __func__, __LINE__);
		exit(EXIT_FAILURE);
	}

	/* if ((req->status & dev->flash->success_mask) != dev->flash->success_status) {
		printf("%s() %d: status check fail 0x%02X\n", __func__, __LINE__, (__u8)req->status);
		exit(EXIT_FAILURE);
	} */

	free_request(req);
	return 0;
}

static int __softbitread(struct shannon_dev *dev, int lun, int block, int page, int nplane, int constant, char *filename)
{
	struct shannon_request *chunk_head_req, *req, *tmp;
	struct list_head req_head;
	int i, j, k, bs, ns, remain_ns;
	int head, plane, last_cacheread = 1;
	FILE *fp;

	fp = fopen(filename, "w");
	if (NULL == fp) {
		printf("create file %s fail\n", filename);
		exit(EXIT_FAILURE);
	}

	head = 0;
	INIT_LIST_HEAD(&req_head);

	for (; constant > 0; constant--, page++) {
		/* preread cmdq */
		chunk_head_req = alloc_request(dev, sh_preread_cmd, lun, block * nplane * dev->flash->npage + page, head, 0, 0);
		if (NULL == chunk_head_req) {
			printf("%s() %d: malloc fail\n", __func__, __LINE__);
			exit(EXIT_FAILURE);
		}
		list_add_tail(&chunk_head_req->list, &req_head);

		for (plane = 1; plane < nplane; plane++) {
			req = alloc_request(dev, sh_preread_cmd, lun, (block * nplane + plane) * dev->flash->npage + page, head, 0, 0);
			if (NULL == req) {
				printf("%s() %d: malloc fail\n", __func__, __LINE__);
				exit(EXIT_FAILURE);
			}
			list_add_tail(&req->chunk_list, &chunk_head_req->chunk_list);
		}

		/* cacheread cmdq */
		for (plane = 0; plane < nplane; plane++) {
			bs = 0;
			remain_ns = dev->config->page_nsector;

			while (remain_ns) {
				ns = (remain_ns >= 8) ? 8 : remain_ns;

				req = alloc_request(dev, sh_cacheread_cmd, lun, (block * nplane + plane) * dev->flash->npage + page, head, bs, ns);
				if (NULL == req) {
					printf("%s() %d: malloc fail\n", __func__, __LINE__);
					exit(EXIT_FAILURE);
				}
				list_add_tail(&req->list, &req_head);

				bs += ns;
				remain_ns -= ns;
			}
		}
		req->last_cacheread = last_cacheread;

		list_for_each_entry(req, &req_head, list) {
			if (dev->submit_request(req)) {
				printf("%s() %d: submit fail\n", __func__, __LINE__);
				exit(EXIT_FAILURE);
			}
		}

		/* update command head and execute */
		if (update_poll_cmdqueue(dev, lun)) {
			printf("%s() %d: poll fail\n", __func__, __LINE__);
			exit(EXIT_FAILURE);
		}

		/* check status */
		req = list_first_entry(&req_head, struct shannon_request, list);
		if (check_req_status(req)) {
			printf("%s() %d: check status fail\n", __func__, __LINE__);
			exit(EXIT_FAILURE);
		}
		list_del(&req->list);	// del chunk preread request
		free_request(req);

		/* save data */
		list_for_each_entry(req, &req_head, list) {
			__u16 *ptmp;

			for (i = 0; i < req->nsector; i++) {
				ptmp = (__u16 *)(&req->data[i * dev->config->sector_size]);
				for (j = 0; j < dev->config->sector_size/2; j++) {
						for (k = 0; k < 16; k++)
							fprintf(fp, "%d", (ptmp[j] & (0x8000 >> k)) ? 1 : 0);
						fprintf(fp, "\n");
				}

				ptmp = (__u16 *)(req->metadata + i);
				for (j = 0; j < sizeof(*req->metadata)/2; j++) {
					for (k = 0; k < 16; k++)
						fprintf(fp, "%d", (ptmp[j] & (0x8000 >> k)) ? 1 : 0);
					fprintf(fp, "\n");
				}
			}
		}

		list_for_each_entry_safe(req, tmp, &req_head, list) {
			list_del(&req->list);
			free_request(req);
		}
	}

	fclose(fp);
	return 0;
}

static void shannon_softbitread_usage(void)
{
	printf("Description:\n");
	printf("\tToshiba softbitread\n\n");

	printf("Usage:\n");
	printf("\tread [option] lun block page na nb nc pa pb pc\n\n");

	printf("Option:\n");
	printf("\t-C, --chunk\n"
		"\t\tread chunk, NOTE: block is count by chunk block\n\n");
	printf("\t-a, --skip-normal-read\n"
		"\t\tskip normal read\n\n");
	printf("\t-b, --skip-negative-read\n"
		"\t\tskip negative read\n\n");
	printf("\t-c, --skip-positive-read\n"
		"\t\tskip positive read\n\n");
	printf("\t-h, --help\n"
		"\t\tdisplay this help and exit\n");
}

int shannon_softbitread(struct shannon_dev *dev, int argc, char **argv)
{
	struct option longopts[] = {
		{"chunk", no_argument, NULL, 'C'},
		{"help", no_argument, NULL, 'h'},
		{"skip-normal-read", no_argument, NULL, 'a'},
		{"skip-negative-read", no_argument, NULL, 'b'},
		{"skip-positive-read", no_argument, NULL, 'c'},
		{0, 0, 0, 0},
	};
	int opt;
	int lun, block, page, nplane = 1;
	int na, nb, nc, pa, pb, pc;
	struct wr_flash_regs wd;
	int skipa = 0, skipb = 0, skipc = 0;

	while ((opt = getopt_long(argc, argv, "hCabc", longopts, NULL)) != -1) {
		switch (opt) {
		case 'C':
			nplane = dev->config->nplane;
			break;
		case 'a':
			skipa = 1;
			break;
		case 'b':
			skipb = 1;
			break;
		case 'c':
			skipc = 1;
			break;
		case 'h':
			shannon_softbitread_usage();
			return 0;
		default:
			shannon_softbitread_usage();
			return ERR;
		}
	}

	if ((argc - optind) != 9) {
		shannon_softbitread_usage();
		exit(EXIT_FAILURE);
	}

	lun = atoi(argv[optind]);
	block = atoi(argv[optind + 1]);
	page = atoi(argv[optind + 2]);

	na = atoi(argv[optind + 3]);
	nb = atoi(argv[optind + 4]);
	nc = atoi(argv[optind + 5]);

	pa = atoi(argv[optind + 6]);
	pb = atoi(argv[optind + 7]);
	pc = atoi(argv[optind + 8]);

	printf("shift vale=%d %d %d %d %d %d\n", na, nb, nc, pa, pb, pc);

	/* pre condition */
	memset(&wd, 0x00, sizeof(wd));
	wd.cmd = 0x5C;
	wd.has_secondary_cmd = 1;
	wd.secondary_cmd = 0xC5;
	wd.vendory_mode = 1;
	wd.ndata = 0;
	if (write_flash_registers(dev, lun, &wd)) {
		printf("%s() %d: pre condition fail\n", __func__, __LINE__);
		exit(EXIT_FAILURE);
	}

	/* normal read */
	if (skipa)
		goto negative_read;
	__softbitread(dev, lun, block, page, nplane, 1, "softrd_normal");

	/* set shift value for SBn then read */
negative_read:
	if (skipb)
		goto positive_read;
	memset(&wd, 0x00, sizeof(wd));
	wd.cmd = 0x55;
	wd.vendory_mode = 1;
	wd.ndata = 1;
	wd.data[0] = na;
	wd.addr = 5;
	if (write_flash_registers(dev, lun, &wd)) {
		printf("%s() %d: pre condition fail\n", __func__, __LINE__);
		exit(EXIT_FAILURE);
	}

	memset(&wd, 0x00, sizeof(wd));
	wd.cmd = 0x55;
	wd.vendory_mode = 1;
	wd.ndata = 1;
	wd.data[0] = nb;
	wd.addr = 6;
	if (write_flash_registers(dev, lun, &wd)) {
		printf("%s() %d: pre condition fail\n", __func__, __LINE__);
		exit(EXIT_FAILURE);
	}

	memset(&wd, 0x00, sizeof(wd));
	wd.cmd = 0x55;
	wd.vendory_mode = 1;
	wd.ndata = 1;
	wd.data[0] = nc;
	wd.addr = 7;
	if (write_flash_registers(dev, lun, &wd)) {
		printf("%s() %d: pre condition fail\n", __func__, __LINE__);
		exit(EXIT_FAILURE);
	}

	memset(&wd, 0x00, sizeof(wd));
	wd.cmd = 0x5D;
	wd.vendory_mode = 1;
	wd.ndata = 0;
	if (write_flash_registers(dev, lun, &wd)) {
		printf("%s() %d: pre condition fail\n", __func__, __LINE__);
		exit(EXIT_FAILURE);
	}

	__softbitread(dev, lun, block, page, nplane, 1, "softrd_sbn");

	/* set shift value for SBp then read */
positive_read:
	if (skipc)
		goto softbit_readend;
	memset(&wd, 0x00, sizeof(wd));
	wd.cmd = 0x55;
	wd.vendory_mode = 1;
	wd.ndata = 1;
	wd.data[0] = pa;
	wd.addr = 5;
	if (write_flash_registers(dev, lun, &wd)) {
		printf("%s() %d: pre condition fail\n", __func__, __LINE__);
		exit(EXIT_FAILURE);
	}

	memset(&wd, 0x00, sizeof(wd));
	wd.cmd = 0x55;
	wd.vendory_mode = 1;
	wd.ndata = 1;
	wd.data[0] = pb;
	wd.addr = 6;
	if (write_flash_registers(dev, lun, &wd)) {
		printf("%s() %d: pre condition fail\n", __func__, __LINE__);
		exit(EXIT_FAILURE);
	}

	memset(&wd, 0x00, sizeof(wd));
	wd.cmd = 0x55;
	wd.vendory_mode = 1;
	wd.ndata = 1;
	wd.data[0] = pc;
	wd.addr = 7;
	if (write_flash_registers(dev, lun, &wd)) {
		printf("%s() %d: pre condition fail\n", __func__, __LINE__);
		exit(EXIT_FAILURE);
	}

	memset(&wd, 0x00, sizeof(wd));
	wd.cmd = 0x5D;
	wd.vendory_mode = 1;
	wd.ndata = 0;
	if (write_flash_registers(dev, lun, &wd)) {
		printf("%s() %d: pre condition fail\n", __func__, __LINE__);
		exit(EXIT_FAILURE);
	}

	__softbitread(dev, lun, block, page, nplane, 1, "softrd_sbp");

	/* terminate soft bit read */
softbit_readend:
	memset(&wd, 0x00, sizeof(wd));
	wd.cmd = 0x55;
	wd.vendory_mode = 1;
	wd.ndata = 1;
	wd.data[0] = 0;
	wd.addr = 5;
	if (write_flash_registers(dev, lun, &wd)) {
		printf("%s() %d: pre condition fail\n", __func__, __LINE__);
		exit(EXIT_FAILURE);
	}

	memset(&wd, 0x00, sizeof(wd));
	wd.cmd = 0x55;
	wd.vendory_mode = 1;
	wd.ndata = 1;
	wd.data[0] = 0;
	wd.addr = 6;
	if (write_flash_registers(dev, lun, &wd)) {
		printf("%s() %d: pre condition fail\n", __func__, __LINE__);
		exit(EXIT_FAILURE);
	}

	memset(&wd, 0x00, sizeof(wd));
	wd.cmd = 0x55;
	wd.vendory_mode = 1;
	wd.ndata = 1;
	wd.data[0] = 0;
	wd.addr = 7;
	if (write_flash_registers(dev, lun, &wd)) {
		printf("%s() %d: pre condition fail\n", __func__, __LINE__);
		exit(EXIT_FAILURE);
	}

	return 0;
}

static void shannon_softbitread_a19_usage(void)
{
	printf("Description:\n");
	printf("\tToshiba A19nm softbitread\n\n");

	printf("Usage:\n");
	printf("\tread [option] lun block page a0 a1 b0 b1 c0 c1\n\n");

	printf("Option:\n");
	printf("\t-C, --chunk\n"
		"\t\tread chunk, NOTE: block is count by chunk block\n\n");
	printf("\t-h, --help\n"
		"\t\tdisplay this help and exit\n");
}

int shannon_softbitread_a19(struct shannon_dev *dev, int argc, char **argv)
{
	struct option longopts[] = {
		{"chunk", no_argument, NULL, 'C'},
		{"help", no_argument, NULL, 'h'},
		{0, 0, 0, 0},
	};
	int opt;
	int lun, block, page, nplane = 1;
	int a0, a1, b0, b1, c0, c1;
	struct wr_flash_regs wd;

	while ((opt = getopt_long(argc, argv, "hC", longopts, NULL)) != -1) {
		switch (opt) {
		case 'C':
			nplane = dev->config->nplane;
			break;
		case 'h':
			shannon_softbitread_usage();
			return 0;
		default:
			shannon_softbitread_a19_usage();
			return ERR;
		}
	}

	if ((argc - optind) != 9) {
		shannon_softbitread_a19_usage();
		exit(EXIT_FAILURE);
	}

	lun = atoi(argv[optind]);
	block = atoi(argv[optind + 1]);
	page = atoi(argv[optind + 2]);

	a0 = atoi(argv[optind + 3]);
	a1 = atoi(argv[optind + 4]);

	b0 = atoi(argv[optind + 5]);
	b1 = atoi(argv[optind + 6]);

	c0 = atoi(argv[optind + 7]);
	c1 = atoi(argv[optind + 8]);

	printf("shift vale=%d %d %d %d %d %d\n", a0, a1, b0, b1, c0, c1);

	/* pre condition */
	memset(&wd, 0x00, sizeof(wd));
	wd.cmd = 0x5C;
	wd.has_secondary_cmd = 1;
	wd.secondary_cmd = 0xC5;
	wd.vendory_mode = 1;
	wd.ndata = 0;
	if (write_flash_registers(dev, lun, &wd)) {
		printf("%s() %d: pre condition fail\n", __func__, __LINE__);
		exit(EXIT_FAILURE);
	}
	__softbitread(dev, lun, block, page, nplane, 1, "softrd_a19_normal");

	/* set shift value */
	memset(&wd, 0x00, sizeof(wd));
	wd.cmd = 0x55;
	wd.vendory_mode = 1;
	wd.ndata = 1;
	wd.data[0] = a0;
	wd.addr = 0x04;
	if (write_flash_registers(dev, lun, &wd)) {
		printf("%s() %d: pre condition fail\n", __func__, __LINE__);
		exit(EXIT_FAILURE);
	}
	memset(&wd, 0x00, sizeof(wd));
	wd.cmd = 0x55;
	wd.vendory_mode = 1;
	wd.ndata = 1;
	wd.data[0] = a1;
	wd.addr = 0x08;
	if (write_flash_registers(dev, lun, &wd)) {
		printf("%s() %d: pre condition fail\n", __func__, __LINE__);
		exit(EXIT_FAILURE);
	}

	memset(&wd, 0x00, sizeof(wd));
	wd.cmd = 0x55;
	wd.vendory_mode = 1;
	wd.ndata = 1;
	wd.data[0] = b0;
	wd.addr = 0x09;
	if (write_flash_registers(dev, lun, &wd)) {
		printf("%s() %d: pre condition fail\n", __func__, __LINE__);
		exit(EXIT_FAILURE);
	}
	memset(&wd, 0x00, sizeof(wd));
	wd.cmd = 0x55;
	wd.vendory_mode = 1;
	wd.ndata = 1;
	wd.data[0] = b1;
	wd.addr = 0x0A;
	if (write_flash_registers(dev, lun, &wd)) {
		printf("%s() %d: pre condition fail\n", __func__, __LINE__);
		exit(EXIT_FAILURE);
	}

	memset(&wd, 0x00, sizeof(wd));
	wd.cmd = 0x55;
	wd.vendory_mode = 1;
	wd.ndata = 1;
	wd.data[0] = c0;
	wd.addr = 0x0b;
	if (write_flash_registers(dev, lun, &wd)) {
		printf("%s() %d: pre condition fail\n", __func__, __LINE__);
		exit(EXIT_FAILURE);
	}
	memset(&wd, 0x00, sizeof(wd));
	wd.cmd = 0x55;
	wd.vendory_mode = 1;
	wd.ndata = 1;
	wd.data[0] = c1;
	wd.addr = 0x0c;
	if (write_flash_registers(dev, lun, &wd)) {
		printf("%s() %d: pre condition fail\n", __func__, __LINE__);
		exit(EXIT_FAILURE);
	}

	/* sb0 read */
	memset(&wd, 0x00, sizeof(wd));
	wd.cmd = 0xC0;
	wd.vendory_mode = 1;
	wd.ndata = 0;
	if (write_flash_registers(dev, lun, &wd)) {
		printf("%s() %d: pre condition fail\n", __func__, __LINE__);
		exit(EXIT_FAILURE);
	}
	__softbitread(dev, lun, block, page, nplane, 1, "softrd_a19_sb0");

	/* sb1 read */
	memset(&wd, 0x00, sizeof(wd));
	wd.cmd = 0xC1;
	wd.vendory_mode = 1;
	wd.ndata = 0;
	if (write_flash_registers(dev, lun, &wd)) {
		printf("%s() %d: pre condition fail\n", __func__, __LINE__);
		exit(EXIT_FAILURE);
	}
	__softbitread(dev, lun, block, page, nplane, 1, "softrd_a19_sb1");

	return 0;
}

/*-----------------------------------------------------------------------------------------------------------*/
int shannon_reset(struct shannon_dev *dev, int argc, char **argv)
{
	int lun, rc = ERR;
	struct shannon_request *req;

	if (2 != argc) {
		printf("Usage:\n\treset lun\n");
		return ERR;
	}
	lun = strtoul(argv[1], NULL, 10);

	if (lun > dev->config->luns) {
		printf("Overflow address\n");
		return OVERFLOW_ADDR;
	}

	req = alloc_request(dev, sh_reset_cmd, lun, 0, 0, 0, 0);
	if (NULL == req)
		goto out;

	rc = dev->submit_request(req);
	if (rc)
		goto free_req_out;
	rc = update_poll_cmdqueue(dev, lun);
	if (rc)
		goto free_req_out;

	if ((req->status & dev->flash->success_mask) != dev->flash->success_status) {
		printf("Reset: failed status: 0x%02X. lun=%d\n", (__u8)req->status, lun);
		rc = FAILED_FLASH_STATUS;
		goto free_req_out;
	}

	rc = 0;
free_req_out:
	free_request(req);
out:
	return rc;
}

int shannon_readid(struct shannon_dev *dev, int argc, char **argv)
{
	int lun;
	struct shannon_request *req;

	if (2 != argc) {
		printf("Description:\n\tRead single lun id\n\n");
		printf("Usage:\n\tReadid lun\n");
		return ERR;
	}
	lun = strtoul(argv[1], NULL, 10);

	if (lun > dev->config->luns) {
		printf("Overflow address\n");
		return OVERFLOW_ADDR;
	}

	if (dev->targetlun[lun].shadow) {
		pr_u8_array_noprefix(dev->targetlun[lun].id.byteid, 8, 8);
		return 0;
	}

	req = alloc_request(dev, sh_readid_cmd, lun, 0, 0, 0, 0);
	if (NULL == req)
		malloc_failed_exit();

	if (dev->submit_request(req))
		submit_failed_exit(lun);

	if (update_poll_cmdqueue(dev, lun))
		poll_failed_exit(lun);

	pr_u8_array_noprefix(req->id, 8, 8);
	free_request(req);
	return 0;
}

static void shannon_erase_usage(void)
{
	printf("Description:\n");
	printf("\tErase single lun block\n\n");

	printf("Usage:\n");
	printf("\terase [option] lun block\n\n");

	printf("Option:\n");
	printf("\t-C, --chunk\n"
		"\t\terase chunk, NOTE: block is count by chunk block\n\n");
	printf("\t-h, --help\n"
		"\t\tdisplay this help and exit\n");
}

int shannon_erase(struct shannon_dev *dev, int argc, char **argv)
{
	struct option longopts [] = {
		{"chunk", no_argument, NULL, 'C'},
		{"help", no_argument, NULL, 'h'},
		{0, 0, 0, 0},
	};
	int opt;
	int lun, block;
	int plane, nplane;
	int rc = 0;
	int head;
	struct shannon_request *chunk_head_req, *req;

	/* analyse argument */
	head = INDEP_HEAD;
	nplane = 1;

	while ((opt = getopt_long(argc, argv, "hC", longopts, NULL)) != -1) {
		switch (opt) {
		case 'C':
			nplane = dev->config->nplane;
			break;
		case 'h':
			shannon_erase_usage();
			return 0;
		default:
			shannon_erase_usage();
			return ERR;
		}
	}

	if ((argc - optind) != 2) {
		shannon_erase_usage();
		return ERR;
	}

	lun = strtoul(argv[optind], NULL, 10);
	block = strtoul(argv[optind + 1], NULL, 10);
	if (nplane > 1)
		head |= (1 << SH_ERASE_PLANE_SHIFT);

	if (lun >= dev->config->luns || block >= dev->flash->nblk / nplane) {
		printf("Overflow address\n");
		return OVERFLOW_ADDR;
	}

	/* chunk head request first */
	chunk_head_req = alloc_request(dev, sh_erase_cmd, lun, block * nplane * dev->flash->npage, head, 0, 0);
	if (NULL == chunk_head_req) {
		rc = ALLOCMEM_FAILED;
		goto out;
	}

	/* other chunk request if have */
	for (plane = 1; plane < nplane; plane++) {
		req = alloc_request(dev, sh_erase_cmd, lun, (block * nplane + plane) * dev->flash->npage, head, 0, 0);
		if (NULL == req) {
			rc = ALLOCMEM_FAILED;
			goto free_req_out;
		}
		list_add_tail(&req->chunk_list, &chunk_head_req->chunk_list);
	}

	/* execute */
	if ((rc = dev->submit_request(chunk_head_req)))
		goto free_req_out;

	if ((rc = update_poll_cmdqueue(dev, lun)))
		goto free_req_out;

	if ((rc = check_req_status(chunk_head_req)))
		goto free_req_out;

free_req_out:
	free_request(chunk_head_req);
out:
	if (rc != FAILED_FLASH_STATUS && rc != 0)
		printf("Erase: encounter some error\n");
	return rc;
}

static void fill_meta_data(struct shannon_dev *dev, struct shannon_request *head, FILE *mdata_fp)
{
	int i;
	char line[512];
	struct shannon_request *req;

	req = list_entry(&head->chunk_list, struct shannon_request, chunk_list);
	for (i = 0; i < dev->config->page_nsector; i++) {
		fgets(line, sizeof(line), mdata_fp);
		sscanf(line, "%lX", &req->metadata[i]);
	}

	return;
}

static void fill_user_data(struct shannon_dev *dev, struct shannon_request *head, FILE *udata_fp)
{
	int i;
	char line[512];
	struct shannon_request *req;

	req = list_entry(&head->chunk_list, struct shannon_request, chunk_list);
	for (i = 0; i < dev->config->ndata; i += 32) {

		fgets(line, sizeof(line), udata_fp);
		sscanf(line, "%X %X %X %X %X %X %X %X %X %X %X %X %X %X %X %X %X %X %X %X %X %X %X %X %X %X %X %X %X %X %X %X",
			(uint *)&req->data[i+0],(uint *)&req->data[i+1],(uint *)&req->data[i+2],(uint *)&req->data[i+3],
			(uint *)&req->data[i+4],(uint *)&req->data[i+5],(uint *)&req->data[i+6],(uint *)&req->data[i+7],
			(uint *)&req->data[i+8],(uint *)&req->data[i+9],(uint *)&req->data[i+10],(uint *)&req->data[i+11],
			(uint *)&req->data[i+12],(uint *)&req->data[i+13],(uint *)&req->data[i+14],(uint *)&req->data[i+15],
			(uint *)&req->data[i+16],(uint *)&req->data[i+17],(uint *)&req->data[i+18],(uint *)&req->data[i+19],
			(uint *)&req->data[i+20],(uint *)&req->data[i+21],(uint *)&req->data[i+22],(uint *)&req->data[i+23],
			(uint *)&req->data[i+24],(uint *)&req->data[i+25],(uint *)&req->data[i+26],(uint *)&req->data[i+27],
			(uint *)&req->data[i+28],(uint *)&req->data[i+29],(uint *)&req->data[i+30],(uint *)&req->data[i+31]);
	}

	return;
}

static void shannon_write_usage(void)
{
	printf("Description:\n");
	printf("\tWrite single lun page\n\n");

	printf("Usage:\n");
	printf("\twrite [option] lun ppa\n");
	printf("\twrite [option] lun block page\n\n");

	printf("Option:\n");
	printf("\t-e, --seed=SEED\n"
		"\t\tset the seed of generate data, default use gettimeofday.tv_usec\n\n");
	printf("\t-p, --print-option=[D/M]\n"
		"\t\tprint the write data/metadata. default print no\n\n");
	printf("\t-H, --head=HEAD\n"
		"\t\tselect head, default is head 2\n\n");
	printf("\t-C, --chunk\n"
		"\t\twrite chunk, NOTE: block is count by chunk block\n\n");
	printf("\t-n, --constant-pages=NUM\n"
		"\t\twrite NUM pages from current page but should be within one block\n\n");
	printf("\t-d, --datafile=[filename]\n"
			"\t\tread meta data and user data respectively from the file filename_meta and filename_user\n\n");
	printf("\t-s, --skip-check-status\n"
		"\t\tskip check flash write status\n\n");
	printf("\t-h, --help\n"
		"\t\tdisplay this help and exit\n");
}

int shannon_write(struct shannon_dev *dev, int argc, char **argv)
{
	struct option longopts [] = {
		{"seed", required_argument, NULL, 'e'},
		{"print-option", required_argument, NULL, 'p'},
		{"head", required_argument, NULL, 'H'},
		{"chunk", no_argument, NULL, 'C'},
		{"constant-pages", required_argument, NULL, 'n'},
		{"datafile", required_argument, NULL, 'd'},
		{"skip-check-status", no_argument, NULL, 's'},
		{"help", no_argument, NULL, 'h'},
		{0, 0, 0, 0},
	};
	int i, opt;
	int lun, block, page;
	int seed, pr_data, pr_meta, head;
	int plane, nplane, constant;
	int rc = 0;
	struct shannon_request *chunk_head_req, *req;
	int fromfile = 0;
	int skip_check_status = 0;
	char meta_name[256], user_name[256];
	FILE *fp_user = NULL, *fp_meta = NULL;

	/* analyse argument */
	seed = getseed(0);
	head = INDEP_HEAD;
	nplane = 1;
	constant = 1;
	pr_data = pr_meta = 0;

	while ((opt = getopt_long(argc, argv, "e:p:H:Cn:d:sh", longopts, NULL)) != -1) {
		switch (opt) {
		case 'e':
			seed = strtoul(optarg, NULL, 10);
			break;
		case 'p':
			for (i = 0; i < strlen(optarg); i++) {
				switch (optarg[i]) {
				case 'D':
					pr_data = 1;
					break;
				case 'M':
					pr_meta = 1;
					break;
				default:
					shannon_write_usage();
					return ERR;
				}
			}
			break;
		case 'H':
			head = strtoul(optarg, NULL, 10);
			if (HEAD0 != head && HEAD1 != head && INDEP_HEAD != head) {
				printf("Head choices: 0 1 2\n");
				return ERR;
			}
			break;
		case 'C':
			nplane = dev->config->nplane;
			break;
		case 'n':
			constant = strtoul(optarg, NULL, 10);
			break;
		case 'd':
			fromfile = 1;
			sprintf(meta_name, "%s_meta", optarg);
			sprintf(user_name, "%s_user", optarg);
			break;
		case 's':
			skip_check_status = 1;
			break;
		case 'h':
			shannon_write_usage();
			return 0;
		default:
			shannon_write_usage();
			return ERR;
		}
	}

	if (3 == (argc - optind)) {
		lun = strtoul(argv[optind], NULL, 10);
		block = strtoul(argv[optind + 1], NULL, 10);
		page = strtoul(argv[optind + 2], NULL, 10);
	} else if (2 == (argc - optind)) {
		if (nplane > 1) {
			printf("Format of 'write lun ppa' doesn`t support multi-plane write. Please use 'write lun block page'\n");
			return ERR;
		}
		lun = strtoul(argv[optind], NULL, 10);
		block = strtoul(argv[optind + 1], NULL, 10) / dev->flash->npage;
		page = strtoul(argv[optind + 1], NULL, 10) % dev->flash->npage;
	} else {
		shannon_write_usage();
		return ERR;
	}

	if (nplane > 1)
		head |= (1 << SH_WRITE_PLANE_SHIFT);

	if (lun >= dev->config->luns || block >= dev->flash->nblk / nplane ||
		page >= dev->flash->npage || page + constant > dev->flash->npage) {
		printf("Overflow address\n");
		return OVERFLOW_ADDR;
	}

	if (fromfile) {

		fp_meta = fopen(meta_name, "r");
		if (NULL == fp_meta) {
			perror("can`t create meta data file");
			exit(EXIT_FAILURE);
		}
		fp_user = fopen(user_name, "r");
		if (NULL == fp_user) {
			perror("can`t create user data file");
			exit(EXIT_FAILURE);
		}
	}

	srand(seed);

	for (; constant > 0; constant--, page++) {
		/* chunk head request first */
		chunk_head_req = alloc_request(dev, sh_write_cmd, lun, block * nplane * dev->flash->npage + page, head, 0, dev->config->page_nsector);
		if (NULL == chunk_head_req) {
			rc = ALLOCMEM_FAILED;
			goto out;
		}

		if (!fromfile) {
			pad_rand(chunk_head_req->data, dev->config->ndata);
			pad_rand(chunk_head_req->metadata, dev->config->nmeta);
		} else {
			fill_meta_data(dev, chunk_head_req, fp_meta);
			fill_user_data(dev, chunk_head_req, fp_user);
		}

		/* other chunk request if have */
		for (plane = 1; plane < nplane; plane++) {
			req = alloc_request(dev, sh_write_cmd, lun, (block * nplane + plane) * dev->flash->npage + page, head, 0, dev->config->page_nsector);
			if (NULL == req) {
				rc = ALLOCMEM_FAILED;
				goto free_req_out;
			}

			if (!fromfile) {
				pad_rand(req->data, dev->config->ndata);
				pad_rand(req->metadata, dev->config->nmeta);
			} else {
				fill_meta_data(dev, req, fp_meta);
				fill_user_data(dev, req, fp_user);
			}

			list_add_tail(&req->chunk_list, &chunk_head_req->chunk_list);
		}

		/* execute */
		if ((rc = dev->submit_request(chunk_head_req)))
			goto free_req_out;

		if ((rc = update_poll_cmdqueue(dev, lun)))
			goto free_req_out;

		if (!skip_check_status && (rc = check_req_status(chunk_head_req)))
			goto free_req_out;

		/* printf data if need */
		if (pr_meta) {
			printf("#meta(lun=%d,block=%d,page=%d): ", lun, block, page);	// metadata
			for (i = 0; i < dev->config->page_nsector; i++)
				printf("%016lX ", chunk_head_req->metadata[i]);
			list_for_each_entry(req, &chunk_head_req->chunk_list, chunk_list) {
				for (i = 0; i < dev->config->page_nsector; i++)
					printf("%016lX ", req->metadata[i]);
			}
			printf("\n");
		}

		if (pr_data) {
			for (i = 0; i < dev->config->page_nsector; i++) {	// data
				printf("#sector(lun=%d,block=%d,page=%d): %d\n", lun, block, page, i);
				pr_u8_array_noprefix(chunk_head_req->data + i * dev->config->sector_size, dev->config->sector_size, 32);
			}

			list_for_each_entry(req, &chunk_head_req->chunk_list, chunk_list) {
				for (i = 0; i < dev->config->page_nsector; i++) {
					printf("#sector(lun=%d,block=%d,page=%d): %d\n", lun, block, page, (nplane > 1) ? i + req->chunk_plane * dev->config->page_nsector : i);
					pr_u8_array_noprefix(req->data + i * dev->config->sector_size, dev->config->sector_size, 32);
				}
			}
		}

		free_request(chunk_head_req);
	}

	if (fromfile) {
		fclose(fp_meta);
		fclose(fp_user);
	}

	return 0;

free_req_out:
	free_request(chunk_head_req);
out:
	if (rc != FAILED_FLASH_STATUS && rc != 0)
		printf("Write: encounter some error\n");

	if (fromfile) {
		fclose(fp_meta);
		fclose(fp_user);
	}

	return rc;
}

static void shannon_read_usage(void)
{
	printf("Description:\n");
	printf("\tRead single lun page\n\n");

	printf("Usage:\n");
	printf("\tread [option] lun ppa\n");
	printf("\tread [option] -b lun pba\n");
	printf("\tread [option] lun block page\n\n");

	printf("Option:\n");
	printf("\t-p, --print-option=[D/M/E/N]\n"
		"\t\tonly print data/metadata/ECC/NONE or their association. if no set this option, default print all\n\n");
	printf("\t-C, --chunk\n"
		"\t\tread chunk, NOTE: block is count by chunk block\n\n");
	printf("\t-n, --constant-pages=NUM\n"
		"\t\tread NUM pages from current page but should be within one block\n\n");
	printf("\t-D, --disable-last-cacheread\n"
		"\t\tDisable use last_cacheread command\n\n");
	printf("\t-l, --loops=N\n"
		"\t\tSpecifies the number of iterations\n\n");
	printf("\t-E, --totalecc\n"
		"\t\tOutput total ECC value\n\n");
	printf("\t-d, --datafile=[filename]\n"
		"\t\twrite meta data and user data respectively to the file filename_meta and filename_user\n\n");
	printf("\t-h, --help\n"
		"\t\tdisplay this help and exit\n");
}

int shannon_read(struct shannon_dev *dev, int argc, char **argv)
{
	struct option longopts [] = {
		{"print-option", required_argument, NULL, 'p'},
		{"chunk", no_argument, NULL, 'C'},
		{"constant-pages", required_argument, NULL, 'n'},
		{"disable-last-cacheread", no_argument, NULL, 'D'},
		{"pba", no_argument, NULL, 'b'},
		{"loops", required_argument, NULL, 'l'},
		{"totalecc", no_argument, NULL, 'E'},
		{"datafile", required_argument, NULL, 'd'},
		{"help", no_argument, NULL, 'h'},
		{0, 0, 0, 0},
	};
	int opt;
	int pr_split, pr_only_data, pr_only_meta, pr_only_ecc, pr_ecc_total;
	int lun, block, page;
	int head;
	int plane, nplane, constant;
	int rc = 0;
	struct shannon_request *chunk_head_req, *req, *tmp;
	struct list_head req_head;
	int bs, ns, remain_ns;
	int i, j;
	int last_cacheread = 1;
	int ispba, sectoroff;
	char sectoroff_str[32];
	int spage, loops, sconstant;
	int quiet = 0;
	long total_ecc = 0;
	int tofile = 0;
	char meta_name[256], user_name[256];
	FILE *fp_meta = NULL;
	FILE *fp_user = NULL;

	/* analyse argument */
	head = 0;		/* read has no head */
	nplane = 1;
	sconstant = constant = 1;
	ispba = 0;
	loops = 1;

	pr_split = pr_only_data = pr_only_meta = pr_only_ecc = pr_ecc_total = 0;

	while ((opt = getopt_long(argc, argv, "p:Cn:Dbl:Ed:h", longopts, NULL)) != -1) {
		switch (opt) {
		case 'p':
			pr_split = 1;

			for (i = 0; i < strlen(optarg); i++) {
				switch (optarg[i]) {
				case 'D':
					pr_only_data = 1;
					break;
				case 'M':
					pr_only_meta = 1;
					break;
				case 'E':
					pr_only_ecc = 1;
					break;
				case 'N':
					quiet = 1;
					break;
				default:
					shannon_read_usage();
					return ERR;
				}
			}
			break;
		case 'C':
			nplane = dev->config->nplane;
			break;
		case 'n':
			constant = strtoul(optarg, NULL, 10);
			break;
		case 'D':
			last_cacheread = 0;
			break;
		case 'b':
			ispba = 1;
			break;
		case 'l':
			loops = atoi(optarg);
			if (loops <= 0)
				loops = 1;
			break;
		case 'E':
			pr_ecc_total = 1;
			break;
		case 'd':
			tofile = 1;
			sprintf(meta_name, "%s_meta", optarg);
			sprintf(user_name, "%s_user", optarg);
			break;
		case 'h':
			shannon_read_usage();
			return 0;
		default:
			shannon_read_usage();
			return ERR;
		}
	}

	if (3 == (argc - optind)) {
		lun = strtoul(argv[optind], NULL, 10);
		block = strtoul(argv[optind + 1], NULL, 10);
		page = strtoul(argv[optind + 2], NULL, 10);
	} else if (2 == (argc - optind)) {
		lun = strtoul(argv[optind], NULL, 10);
		if (ispba) {
			block = (strtoul(argv[optind + 1], NULL, 10) / dev->config->page_nsector) / (dev->flash->npage * nplane);
			page = (strtoul(argv[optind + 1], NULL, 10) / dev->config->page_nsector) % dev->flash->npage;
			sectoroff = strtoul(argv[optind + 1], NULL, 10) % dev->config->page_nsector +
				(((strtoul(argv[optind + 1], NULL, 10) / dev->config->page_nsector) / dev->flash->npage) % nplane) * dev->config->page_nsector;
			sprintf(sectoroff_str, ",sector=%d", sectoroff);
		} else {
			block = strtoul(argv[optind + 1], NULL, 10) / (dev->flash->npage * nplane);
			page = strtoul(argv[optind + 1], NULL, 10) % dev->flash->npage;
		}
	} else {
		shannon_read_usage();
		return ERR;
	}

	if (nplane > 1)
		head |= (1 << SH_READ_PLANE_SHIFT);

	if (lun >= dev->config->luns || block >= dev->flash->nblk / nplane ||
		page >= dev->flash->npage || page + constant > dev->flash->npage) {
		printf("Overflow address\n");
		return OVERFLOW_ADDR;
	}

	INIT_LIST_HEAD(&req_head);

	if (quiet) {
		pr_split = 1;
		pr_only_ecc = pr_only_meta = pr_only_data = 0;
	}

	if (tofile) {

		fp_meta = fopen(meta_name, "w+");
		if (NULL == fp_meta) {
			perror("can`t create meta data file");
			exit(EXIT_FAILURE);
		}
		fp_user = fopen(user_name, "w+");
		if (NULL == fp_user) {
			perror("can`t create user data file");
			exit(EXIT_FAILURE);
		}
	}

	spage = page;
	sconstant = constant;
next_loop:
	for (; constant > 0; constant--, page++) {
		/* preread cmdq */
		chunk_head_req = alloc_request(dev, sh_preread_cmd, lun, block * nplane * dev->flash->npage + page, head, 0, 0);
		if (NULL == chunk_head_req) {
			rc = ALLOCMEM_FAILED;
			goto out;
		}
		list_add_tail(&chunk_head_req->list, &req_head);

		for (plane = 1; plane < nplane; plane++) {
			req = alloc_request(dev, sh_preread_cmd, lun, (block * nplane + plane) * dev->flash->npage + page, head, 0, 0);
			if (NULL == req) {
				rc = ALLOCMEM_FAILED;
				goto free_req_out;
			}
			list_add_tail(&req->chunk_list, &chunk_head_req->chunk_list);
		}

		/* cacheread cmdq */
		for (plane = 0; plane < nplane; plane++) {
			bs = 0;
			remain_ns = dev->config->page_nsector;

			while (remain_ns) {
				ns = (remain_ns >= 8) ? 8 : remain_ns;

				req = alloc_request(dev, sh_cacheread_cmd, lun, (block * nplane + plane) * dev->flash->npage + page, head, bs, ns);
				if (NULL == req) {
					rc = ALLOCMEM_FAILED;
					goto free_req_out;
				}
				list_add_tail(&req->list, &req_head);

				bs += ns;
				remain_ns -= ns;
			}
		}
		req->last_cacheread = last_cacheread;

		list_for_each_entry(req, &req_head, list) {
			if ((rc = dev->submit_request(req)))
				goto free_req_out;
		}

		/* update command head and execute */
		if ((rc = update_poll_cmdqueue(dev, lun)))
			goto free_req_out;

		/* check status */
		req = list_first_entry(&req_head, struct shannon_request, list);
		if ((rc = check_req_status(req)))
			goto free_req_out;
		list_del(&req->list);		// del chunk preread request
		free_request(req);

		/* print read data */
		if (!pr_split || pr_only_ecc)
			printf("#ECC(lun=%d,block=%d,page=%d%s): ", lun, block, page, ispba ? sectoroff_str : "");

		if (!pr_split || pr_only_ecc || pr_ecc_total) {
			list_for_each_entry(req, &req_head, list) {
				int nib;

				for (nib = 0; nib < req->nsector; nib++) {
					if (!pr_split || pr_only_ecc)
						printf("%02X ", req->ecc[nib]);
					total_ecc += req->ecc[nib];
				}
			}

			if (!pr_split || pr_only_ecc)
				printf("\n");
		}

		if (!pr_split || pr_only_meta) {
			printf("#meta(lun=%d,block=%d,page=%d%s): ", lun, block, page, ispba ? sectoroff_str : "");
			list_for_each_entry(req, &req_head, list) {
				for (i = 0; i < req->nsector; i++)
					printf("%016lX ", req->metadata[i]);
			}
			printf("\n");
		}

		if (tofile) {
			list_for_each_entry(req, &req_head, list) {
				for (i = 0; i < req->nsector; i++)
					fprintf(fp_meta, "%016lX\n", req->metadata[i]);
			}
		}

		if (!pr_split || pr_only_data) {
			list_for_each_entry(req, &req_head, list) {
				for (i = 0; i < req->nsector; i++) {
					printf("#sector(lun=%d,block=%d,page=%d): %d\n", lun, block, page, (nplane > 1) ? i + req->chunk_plane * dev->config->page_nsector + req->bsector : i + req->bsector);
					pr_u8_array_noprefix(req->data + i * dev->config->sector_size, dev->config->sector_size, 32/(dev->valid_8bit+1));
				}
			}
		}

		if (tofile) {
			list_for_each_entry(req, &req_head, list) {
				for (i = 0; i < req->nsector; i++) {
					for (j = 0; j < dev->config->sector_size; j++) {
						if (0 == j%32 && j != 0)
							fprintf(fp_user, "\n");
						fprintf(fp_user, "%02X ", (req->data + i * dev->config->sector_size)[j]);
					}
					fprintf(fp_user, "\n");
				}
			}
		}

		list_for_each_entry_safe(req, tmp, &req_head, list) {
			list_del(&req->list);
			free_request(req);
		}
	}

	if (--loops) {
		constant = sconstant;
		page = spage;
		goto next_loop;
	}

	if (pr_ecc_total)
		printf("TotalEccNum=%ld\n", total_ecc);

	if (tofile) {
		fclose(fp_meta);
		fclose(fp_user);
	}

	return 0;

free_req_out:
	list_for_each_entry_safe(req, tmp, &req_head, list) {
		list_del(&req->list);
		free_request(req);
	}
out:
	if (rc != FAILED_FLASH_STATUS && rc != 0)
		printf("Read: encounter some error\n");

	if (tofile) {
		fclose(fp_meta);
		fclose(fp_user);
	}

	return rc;
}

/*-----------------------------------------------------------------------------------------------------------*/
#define	pexit(x...) do { printf(x); exit(EXIT_FAILURE); } while (0)

static int get_group_address(struct shannon_dev *dev, char *s, int *lun, int *block, int *page)
{
	int tmp;
	char *p, *endptr;

	p = s;
	*lun = strtoul(p, &endptr, 10);
	if (*endptr != ',') {
		printf("Invalid group address: %s\n", s);
		return 1;
	}

	p = endptr + 1;
	tmp = strtoul(p, &endptr, 10);
	if (*endptr == ',') {
		*block = tmp;
		*page = strtoul(endptr + 1, NULL, 10);
	} else {
		*block = tmp / dev->flash->npage;
		*page = tmp % dev->flash->npage;
	}

	// printf("%s() lun=%d block=%d page=%d\n", __func__, *lun, *block, *page);
	return 0;
}

static void shannon_copy_usage(void)
{
	printf("Description:\n");
	printf("\tcopy page to another page based on single lun\n\n");

	printf("Usage:\n");
	printf("\tcopy [option] --from=lun,block,page/lun,ppa --to=lun,block,page/lun,ppa\n\n");

	printf("Option:\n");
	printf("\t--from=lun,block,page/lun,ppa\n"
		"\t\tcopy from this page\n\n");
	printf("\t--to=lun,block,page/lun,ppa\n"
		"\t\tcopy to this page\n\n");
	printf("\t-C, --chunk\n"
		"\t\tcopy chunk, NOTE: block is count by chunk block\n\n");
	printf("\t-n, --constant-pages=NUM\n"
		"\t\tcopy NUM pages from current page but should be within one block\n\n");
	printf("\t-D, --disable-last-cacheread\n"
		"\t\tDisable use last_cacheread command\n\n");
	printf("\t-s, --silent\n"
		"\t\tdon`t output from and to address\n\n");
	printf("\t-h, --help\n"
		"\t\tdisplay this help and exit\n");
}
int shannon_copy(struct shannon_dev *dev, int argc, char **argv)
{
	struct option longopts [] = {
		{"from", required_argument, NULL, 'O'},
		{"to", required_argument, NULL, 'I'},
		{"chunk", no_argument, NULL, 'C'},
		{"constant-pages", required_argument, NULL, 'n'},
		{"disable-last-cacheread", no_argument, NULL, 'D'},
		{"silent", no_argument, NULL, 's'},
		{"help", no_argument, NULL, 'h'},
		{0, 0, 0, 0},
	};
	int opt;
	int from, to, fromlun, fromblock, frompage, tolun, toblock, topage;
	int head, plane, nplane, constant;
	struct shannon_request *chunk_head_req, *req, *tmp;
	struct list_head req_head;
	int bs, ns, remain_ns;
	int last_cacheread = 1;
	int silent;
	__u8 *data, *meta;

	/* analyse argument */
	head = INDEP_HEAD;
	nplane = 1;
	constant = 1;
	from = to = 0;
	silent = 0;

	while ((opt = getopt_long(argc, argv, "O:I:Cn:Dsh", longopts, NULL)) != -1) {
		switch (opt) {
		case 'O':
			if (get_group_address(dev, optarg, &fromlun, &fromblock, &frompage))
				return EXIT_FAILURE;
			from = 1;
			break;
		case 'I':
			if (get_group_address(dev, optarg, &tolun, &toblock, &topage))
				return EXIT_FAILURE;
			to = 1;
			break;
		case 'C':
			nplane = dev->config->nplane;
			break;
		case 'n':
			constant = strtoul(optarg, NULL, 10);
			break;
		case 'D':
			last_cacheread = 0;
			break;
		case 's':
			silent = 1;
			break;
		case 'h':
			shannon_copy_usage();
			return 0;
		default:
			shannon_copy_usage();
			return EXIT_FAILURE;
		}
	}

	if (!from) {
		printf("missing option --from\n");
		shannon_copy_usage();
		exit(EXIT_FAILURE);
	}

	if (!to) {
		printf("missing option --to\n");
		shannon_copy_usage();
		exit(EXIT_FAILURE);
	}

	if (!silent) {
		printf("FROM: lun=%d block=%d page=%d\n", fromlun, fromblock, frompage);
		printf("TO:   lun=%d block=%d page=%d\n", tolun, toblock, topage);
	}

	if (nplane > 1)
		head |= (1 << SH_READ_PLANE_SHIFT);

	if (fromlun >= dev->config->luns || fromblock >= dev->flash->nblk / nplane ||
		frompage >= dev->flash->npage || frompage + constant > dev->flash->npage) {
		pexit("Overflow address [FROM]\n");
	}

	if (tolun >= dev->config->luns || toblock >= dev->flash->nblk / nplane ||
		topage >= dev->flash->npage || topage + constant > dev->flash->npage) {
		pexit("Overflow address [TO]\n");
	}

	data = malloc(dev->config->ndata * nplane);
	meta = malloc(dev->config->nmeta * nplane);
	if (NULL == data || NULL == meta)
		pexit("malloc failed");

	INIT_LIST_HEAD(&req_head);

	for (; constant > 0; constant--, frompage++, topage++) {
		/*------------------ copy read ----------------*/
		/* preread cmdq */
		chunk_head_req = alloc_request(dev, sh_preread_cmd, fromlun, fromblock * nplane * dev->flash->npage + frompage, head, 0, 0);
		if (NULL == chunk_head_req)
			pexit("alloc_request failed\n");
		list_add_tail(&chunk_head_req->list, &req_head);

		for (plane = 1; plane < nplane; plane++) {
			req = alloc_request(dev, sh_preread_cmd, fromlun, (fromblock * nplane + plane) * dev->flash->npage + frompage, head, 0, 0);
			if (NULL == req)
				pexit("copy-read: alloc_request failed\n");
			list_add_tail(&req->chunk_list, &chunk_head_req->chunk_list);
		}

		/* cacheread cmdq */
		for (plane = 0; plane < nplane; plane++) {
			bs = 0;
			remain_ns = dev->config->page_nsector;

			while (remain_ns) {
				ns = (remain_ns >= 8) ? 8 : remain_ns;

				req = alloc_request(dev, sh_cacheread_cmd, fromlun, (fromblock * nplane + plane) * dev->flash->npage + frompage, head, bs, 0);
				if (NULL == req)
					pexit("copy-read: alloc_request failed\n");
				req->data = data + dev->config->ndata * plane + bs * dev->config->sector_size;
				req->metadata = (__u64 *)(meta + dev->config->nmeta * plane + bs * 8);
				req->nsector = ns;
				req->bfree = 0;
				list_add_tail(&req->list, &req_head);

				bs += ns;
				remain_ns -= ns;
			}
		}
		req->last_cacheread = last_cacheread;

		list_for_each_entry(req, &req_head, list) {
			if (dev->submit_request(req))
				pexit("copy-read: submit_request failed\n");
		}

		/* update command head and execute */
		if (update_poll_cmdqueue(dev, fromlun))
			pexit("copy-read: update_poll_cmdqueue failed\n");

		/* check status */
		req = list_first_entry(&req_head, struct shannon_request, list);
		if (check_req_status(req))
			pexit("copy-read: check_req_status failed\n");
		list_del(&req->list);
		free_request(req);

		list_for_each_entry_safe(req, tmp, &req_head, list) {
			list_del(&req->list);
			free_request(req);
		}

		/*------------------ copy write ----------------*/
		/* chunk head request first */
		chunk_head_req = alloc_request(dev, sh_write_cmd, tolun, toblock * nplane * dev->flash->npage + topage, head, 0, 0);
		if (NULL == chunk_head_req)
			pexit("copy-write: alloc_request failed\n");
		chunk_head_req->data = data;
		chunk_head_req->metadata = (__u64 *)meta;
		chunk_head_req->nsector = dev->config->page_nsector;
		chunk_head_req->bfree = 0;

		/* other chunk request if have */
		for (plane = 1; plane < nplane; plane++) {
			req = alloc_request(dev, sh_write_cmd, tolun, (toblock * nplane + plane) * dev->flash->npage + topage, head, 0, 0);
			if (NULL == req)
				pexit("copy-write: alloc_request failed\n");
			req->data = data + dev->config->ndata * plane;
			req->metadata = (__u64 *)(meta + dev->config->nmeta * plane);
			req->nsector = dev->config->page_nsector;
			req->bfree = 0;
			list_add_tail(&req->chunk_list, &chunk_head_req->chunk_list);
		}

		/* execute */
		if (dev->submit_request(chunk_head_req))
			pexit("copy-write: submit_request failed\n");

		if (update_poll_cmdqueue(dev, tolun))
			pexit("copy-write: update_poll_cmdqueue failed\n");

		if (check_req_status(chunk_head_req))
			pexit("copy-write: check_req_status failed\n");

		free_request(chunk_head_req);
	}

	free(data);
	free(meta);
	return 0;
}

/*-----------------------------------------------------------------------------------------------------------*/
static void shannon_rwloop_usage(void)
{
	printf("Description:\n");
	printf("\tutils for test MSB and LSB\n\n");

	printf("Usage:\n");
	printf("\trwloop [option] lun block begin-page continuous-pages\n\n");

	printf("Option:\n");
	printf("\t-C, --chunk\n"
		"\t\trw chunk page, NOTE: block is count by chunk block\n\n");
	printf("\t-r, --reread=COUNT\n"
		"\t\tread COUNT times after write\n\n");
	printf("\t-n, --no-erase-first\n"
		"\t\tdo't do erase before write\n\n");
	printf("\t-h, --help\n"
		"\t\tdisplay this help and exit\n\n");

	printf("Example:\n");
	printf("\trwloop 1 25 0 100 -- write lun-1 block-25 pages from 0 to 99, read pages ahead after per-write operation\n");
}

int shannon_rwloop(struct shannon_dev *dev, int argc, char **argv)
{
	struct option longopts [] = {
		{"chunk", no_argument, NULL, 'C'},
		{"reread", required_argument, NULL, 'r'},
		{"no-erase-first", required_argument, NULL, 'n'},
		{"help", no_argument, NULL, 'h'},
		{0, 0, 0, 0},
	};
	int opt, lun, blk, page, bchunk = 0, loops, c, rcnt, rcount = 1;
	char *myargv[16], vlun[32], vblk[32], vpage[32], vnpage[32];
	int noerase = 0;

	bzero(myargv, sizeof(myargv));

	while ((opt = getopt_long(argc, argv, "Cr:nh", longopts, NULL)) != -1) {
		switch (opt) {
		case 'C':
			bchunk = 1;
			break;
		case 'r':
			rcount = strtoul(optarg, NULL, 10);
			break;
		case 'n':
			noerase = 1;
			break;
		case 'h':
			shannon_rwloop_usage();
			return 0;
		default:
			shannon_rwloop_usage();
			exit(EXIT_FAILURE);
		}
	}

	if (4 == (argc - optind)) {
		lun = strtoul(argv[optind], NULL, 10);
		blk = strtoul(argv[optind + 1], NULL, 10);
		page = strtoul(argv[optind + 2], NULL, 10);
		loops = strtoul(argv[optind + 3], NULL, 10);
	} else {
		shannon_rwloop_usage();
		exit(EXIT_FAILURE);
	}

	if (noerase)
		goto dorw;

	/* erase this block first */
	myargv[0] = "erase";
	sprintf(vlun, "%d", lun); myargv[1] = vlun;
	sprintf(vblk, "%d", blk); myargv[2] = vblk;
	myargv[3] = "--chunk";

	optind = 1;
	if (shannon_erase(dev, 3 + bchunk, myargv))
		exit(EXIT_FAILURE);

	/* write then read */
dorw:
	for (c = 0; c < loops; c++) {
		printf("======= write-page: %d, read-page: %d~%d =======\n", page + c, page, page + c);

		// write one page
		myargv[0] = "write";
		sprintf(vlun, "%d", lun); myargv[1] = vlun;
		sprintf(vblk, "%d", blk); myargv[2] = vblk;
		sprintf(vpage, "%d", page + c); myargv[3] = vpage;
		myargv[4] = "--chunk";

		optind = 1;
		if (shannon_write(dev, 4 + bchunk, myargv))
			exit(EXIT_FAILURE);

		// read front pages
		sprintf(vlun, "%d", lun);
		sprintf(vblk, "%d", blk);
		sprintf(vpage, "%d", page);
		sprintf(vnpage, "%s%d", "-n", c + 1);

		for (rcnt = 0; rcnt < rcount; rcnt++) {
			printf("*ReadCount=%d\n", rcnt + 1);

			myargv[0] = "read";
			myargv[1] = vlun;
			myargv[2] = vblk;
			myargv[3] = vpage;
			myargv[4] = vnpage;
			myargv[5] = "-pE";
			myargv[6] = "--chunk";

			optind = 1;
			if (shannon_read(dev, 6 + bchunk, myargv))
				exit(EXIT_FAILURE);
		}

		if (c != loops - 1)
			printf("\n");
	}

	return 0;
}
/*-----------------------------------------------------------------------------------------------------------*/
