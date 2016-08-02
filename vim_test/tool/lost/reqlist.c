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

enum {
	// rand_write_head0 = 0,
	// rand_write_head1,
	rand_write = 0,
	rand_read,
	rand_erase,
	rand_nops,
};

// TODO:
int is_bad_lun(struct shannon_dev *dev, int block, int lun)
{
	return 0;
}

/*-------------------------------------------------------------------------------------------------------------------------------------------------*/
static struct shannon_request *mk_rand_erase_req(struct shannon_dev *dev, int lun)
{
	int head, blk, ppa, plane;
	struct shannon_request *chunk_head_req, *req;

	head = 0;
	blk = rand() % (dev->flash->nblk * dev->config->nplane);
	if (dev->config->nplane > 1)
		head |= (1 << SH_ERASE_PLANE_SHIFT);
	ppa = blk * dev->config->nplane * dev->flash->npage;

	chunk_head_req = alloc_request(dev, sh_erase_cmd, lun, ppa, head, 0, 0);
	if (NULL == chunk_head_req)
		return NULL;

	for (plane = 1; plane < dev->config->nplane; plane++) {
		req = alloc_request(dev, sh_erase_cmd, lun, ppa + plane * dev->flash->npage, head, 0, 0);
		if (NULL == req) {
			free_request(chunk_head_req);
			return NULL;
		}
		list_add_tail(&req->chunk_list, &chunk_head_req->chunk_list);
	}

	return chunk_head_req;
}

static struct shannon_request *mk_rand_preread_req(struct shannon_dev *dev, int lun)
{
	int head, ppa, plane;
	struct shannon_request *chunk_head_req, *req;

	head = 0;
	ppa = rand() % (dev->flash->nblk * dev->flash->npage);
	if (dev->config->nplane > 1) {
		head |= (1 << SH_READ_PLANE_SHIFT);
		ppa &=  (~dev->flash->plane_mask);
	}

	chunk_head_req = alloc_request(dev, sh_preread_cmd, lun, ppa, head, 0, 0);
	if (NULL == chunk_head_req)
		return NULL;

	for (plane = 1; plane < dev->config->nplane; plane++) {
		req = alloc_request(dev, sh_preread_cmd, lun, ppa + plane * dev->flash->npage, head, 0, 0);
		if (NULL == req) {
			free_request(chunk_head_req);
			return NULL;
		}
		list_add_tail(&req->chunk_list, &chunk_head_req->chunk_list);
	}

	return chunk_head_req;
}

static struct shannon_request *mk_write_req(struct shannon_dev *dev, int lun, int ppa, int head)
{
	int blk, plane;
	struct shannon_request *chunk_head_req, *req;

	blk = ppa / (dev->flash->npage * dev->config->nplane);

	if (dev->config->nplane > 1)
		head |= (1 << SH_WRITE_PLANE_SHIFT);

	/* is parity lun */
	if (dev->config->raid_mode && (head & HEAD_MASK) != INDEP_HEAD && lun == dev->sb[blk].parity_lun) {
		chunk_head_req = alloc_request(dev, sh_raidwrite_cmd, dev->sb[blk].parity_lun, ppa, head, 0, 0);
		if (NULL == chunk_head_req)
			return NULL;

		for (plane = 1; plane < dev->config->nplane; plane++) {
			req = alloc_request(dev, sh_raidwrite_cmd, dev->sb[blk].parity_lun, ppa + plane * dev->flash->npage, head, 0, 0);
			if (NULL == req) {
				free_request(chunk_head_req);
				return NULL;
			}
			list_add_tail(&req->chunk_list, &chunk_head_req->chunk_list);
		}

		return chunk_head_req;
	}

	/* is data lun */
	chunk_head_req = alloc_request(dev, sh_write_cmd, lun, ppa, head, 0, dev->config->page_nsector);
	if (NULL == chunk_head_req)
		return NULL;

	for (plane = 1; plane < dev->config->nplane; plane++) {
		req = alloc_request(dev, sh_write_cmd, lun, ppa + plane * dev->flash->npage, head, 0, dev->config->page_nsector);
		if (NULL == req) {
			free_request(chunk_head_req);
			return NULL;
		}
		list_add_tail(&req->chunk_list, &chunk_head_req->chunk_list);
	}

	return chunk_head_req;
}

#define	PR_REQLIST(op)									\
	do {										\
		printf(""#op"");								\
		for (plane = 0; plane < dev->config->nplane; plane++)			\
			printf(" ppa=%06X", req->ppa + plane * dev->flash->npage);	\
		printf("\n");								\
	} while (0)


/*-------------------------------------------------------------------------------------------------------------------------------------------------*/
/*
 * make single head rand write/read/erase reqlist
 */
struct list_head *make_shead_reqlist(struct shannon_dev *dev, int lun, int block, int head, int seed)
{
	int op, erase_rate;
	int ppa, plane, ppa_cnt;
	struct shannon_request *req, *tmp;
	struct list_head *req_head;

	req_head = malloc(sizeof(*req_head));
	if (NULL == req_head)
		return NULL;
	INIT_LIST_HEAD(req_head);

	srand(seed);
	ppa_cnt = 0;
	erase_rate = 0;
	ppa = block * dev->config->nplane * dev->flash->npage;

	if (dev->config->raid_mode && head != INDEP_HEAD && lun == dev->sb[block].parity_lun) {
		req = alloc_request(dev, sh_raidinit_cmd, lun, ppa, head, 0, 0);
		if (NULL == req)
			goto free_req_out;
		list_add_tail(&req->list, req_head);
		printf("PI: ppa=%06X\n", req->ppa);
	}

	/* crossing write/read/eare requests */
	while (ppa_cnt < 256) {
		op = rand();

		switch (op % rand_nops) {
		case rand_write:
			if (is_bad_lun(dev, block, lun))
				break;

			req = mk_write_req(dev, lun, ppa, head);
			if (NULL == req)
				goto free_req_out;
			list_add_tail(&req->list, req_head);

			ppa++;
			ppa_cnt++;

			if (dev->config->raid_mode && head != INDEP_HEAD && lun == dev->sb[block].parity_lun)
				PR_REQLIST(PW);
			else
				PR_REQLIST(WR);
			break;

		case rand_read:
			req = mk_rand_preread_req(dev, lun);
			if (NULL == req)
				goto free_req_out;
			list_add_tail(&req->list, req_head);

			PR_REQLIST(RD);
			break;

		case rand_erase:
			if ((erase_rate++ % 64) != 0)
				break;

			req = mk_rand_erase_req(dev, lun);
			if (NULL == req)
				goto free_req_out;
			list_add_tail(&req->list, req_head);

			PR_REQLIST(ER);
			break;

		default:
			assert(0);	// not arrive here
			return NULL;
		}
	}
	return req_head;

free_req_out:
	list_for_each_entry_safe(req, tmp, req_head, list) {
		list_del(&req->list);
		free_request(req);
	}
	return NULL;
}

#if 0
struct list_head *make_rand_reqlist(struct shannon_dev *dev, int lun, int head0_block, int head1_block)
{
	int op, erase_rate;
	int head, plane;
	int head0_ppa, head1_ppa,  head0_ppa_cnt, head1_ppa_cnt;
	struct shannon_request *req, *tmp;
	struct list_head *req_head;

	req_head = malloc(sizeof(*req_head));
	if (NULL == req_head)
		return NULL;
	INIT_LIST_HEAD(req_head);

	srand(time(NULL));
	head0_ppa = head0_block * dev->config->nplane * dev->flash->npage;
	head1_ppa = head1_block * dev->config->nplane * dev->flash->npage;
	head0_ppa_cnt = head1_ppa_cnt = 0;
	erase_rate = 0;

	/* raid init request first if needed */
	if (dev->config->raid_mode && lun == dev->sb[head0_block].parity_lun) {
		head = HEAD0;
		req = alloc_request(dev, sh_raidinit_cmd, dev->sb[head0_block].parity_lun, head0_ppa, head, 0, dev->sb[head0_block].ndatalun);
		if (NULL == req)
			goto free_req_out;
		list_add_tail(&req->list, req_head);
	}

	if (dev->config->raid_mode && lun == dev->sb[head1_block].parity_lun) {
		head = HEAD1;
		req = alloc_request(dev, sh_raidinit_cmd, dev->sb[head1_block].parity_lun, head1_ppa, head, 0, dev->sb[head1_block].ndatalun);
		if (NULL == req)
			goto free_req_out;
		list_add_tail(&req->list, req_head);
	}

	/* crossing write/read/eare requests */
	while (head0_ppa_cnt < 255 || head1_ppa_cnt < 255) {
		op = rand();

		switch (op % rand_nops) {
		case rand_write_head0:
			if (head0_ppa_cnt++ > 255)
				break;

			if (is_bad_lun(dev, head0_block, lun))
				break;

			req = mk_write_req(dev, lun, head0_ppa, HEAD0);
			if (NULL == req)
				goto free_req_out;
			list_add_tail(&req->list, req_head);

			head0_ppa++;
			PR_REQLIST(W0);
			break;

		case rand_write_head1:
			if (head1_ppa_cnt++ > 255)
				break;

			if (is_bad_lun(dev, head1_block, lun))
				break;

			req = mk_write_req(dev, lun, head0_ppa, HEAD0);
			if (NULL == req)
				goto free_req_out;
			list_add_tail(&req->list, req_head);

			head1_ppa++;
			PR_REQLIST(W1);
			break;

		case rand_read:
			req = mk_rand_preread_req(dev, lun);
			if (NULL == req)
				goto free_req_out;
			list_add_tail(&req->list, req_head);

			PR_REQLIST(RD);
			break;

		case rand_erase:
			if ((erase_rate++ % 64) != 0)
				break;

			req = mk_rand_erase_req(dev, lun);
			if (NULL == req)
				goto free_req_out;
			list_add_tail(&req->list, req_head);

			PR_REQLIST(ER);
			break;

		default:
			assert(0);	// not arrive here
			return NULL;
		}
	}
	//return req_head;

free_req_out:
	list_for_each_entry_safe(req, tmp, req_head, list) {
		list_del(&req->list);
		free_request(req);
	}
	return NULL;
}
#endif

void destory_reqlist(struct list_head *req_head)
{
	struct shannon_request *req, *tmp;

	list_for_each_entry_safe(req, tmp, req_head, list) {
		list_del(&req->list);
		free_request(req);
	}
	free(req_head);
}
/*-------------------------------------------------------------------------------------------------------------------------------------------------*/
int shannon_xxx(struct shannon_dev *dev)
{
	int rc;
	int lun, block, head;
	int finished_lun;
	struct shannon_request *req, *tmp;
	struct list_head **req_head, *run_req_head;

	block = 111;
	head = INDEP_HEAD;

	req_head = malloc(sizeof(*req_head) * dev->config->nlun);
	assert(NULL != req_head);

	run_req_head = malloc(sizeof(*run_req_head) * dev->config->nlun);
	assert(NULL != run_req_head);

	for_dev_each_lun(dev, lun) {
		req_head[lun] = make_shead_reqlist(dev, lun, block, head, block + lun);
		assert(NULL != req_head[lun]);

		INIT_LIST_HEAD(&run_req_head[lun]);
		list_for_each_entry(req, req_head[lun], list)
			list_add_tail(&req->run_list, &run_req_head[lun]);
	}

	/* loop execute */
	finished_lun = 0;

	while (1) {
		for_dev_each_lun(dev, lun) {
			if (!list_empty(&run_req_head[lun]))
				break;
		}
		if (lun == dev->config->nlun)
			break;

		for_dev_each_lun(dev, lun) {
			list_for_each_entry_safe(req, tmp, &run_req_head[lun], run_list) {
				rc = dev->submit_request(req);
				if (rc == NO_CMDQUEUE_ROOM)
					break;
				assert(rc == 0);
				list_del(&req->run_list);
			}
		}

		for_dev_each_lun(dev, lun)
			update_cmdqueue(dev, lun);

		for_dev_each_lun(dev, lun)
			poll_cmdqueue_nowait(dev, lun);
	}

	for_dev_each_lun(dev, lun)
		poll_cmdqueue(dev, lun);

	/* free reqlist */
	for_dev_each_lun(dev, lun)
		destory_reqlist(req_head[lun]);
	free(req_head);
	free(run_req_head);
	return 0;
}
/*-------------------------------------------------------------------------------------------------------------------------------------------------*/
