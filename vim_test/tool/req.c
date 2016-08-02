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

/*----------------------------------------------------------------------------------------------------------------------------------*/
struct shannon_request *alloc_request_no_dma(struct shannon_dev *dev, enum shannon_cmd opcode,
					     int lun, int ppa, int head, int bsector, int nsector, int no_dma)
{
	struct shannon_request *req;

	req = zmalloc(sizeof(*req));
	if (NULL == req)
		goto out;
	req->opcode = opcode;
	req->lun = lun;
	req->ppa = ppa;
	req->head = head;
	req->bsector = bsector;
	req->nsector = nsector;
	req->dev = dev;
	req->bfree = 1;
	req->rw_entire_buffer = 0;
	req->no_dma = no_dma;
	req->data = NULL;
	req->metadata = NULL;
	INIT_LIST_HEAD(&req->list);
	INIT_LIST_HEAD(&req->run_list);
	INIT_LIST_HEAD(&req->chunk_list);
	INIT_LIST_HEAD(&req->lun_list);
	INIT_LIST_HEAD(&req->mem_listhead);

	req->block = req->ppa / dev->flash->npage;
	req->page = req->ppa % dev->flash->npage;
	if (dev->config->nplane > 1) {
		req->chunk_block = req->block / dev->config->nplane;
		req->chunk_plane = req->block % dev->config->nplane;
	} else {
		req->chunk_block = req->block;
		req->chunk_plane = 0;
	}

	if (sh_write_cmd == opcode || sh_cacheread_cmd == opcode
			|| sh_bufwrite_cmd == opcode || sh_bufread_cmd == opcode) {

		if (nsector && !no_dma) {
			req->data = malloc(req->nsector * dev->config->sector_size);
			if (NULL == req->data)
				goto free_req_out;

			req->metadata = malloc(req->nsector * METADATA_SIZE);
			if (NULL == req->metadata)
				goto free_data_out;
		}
	}

	if (sh_bufwrite_cmd == opcode || sh_bufread_cmd == opcode)
		req->bufcmd = 1;

	if (sh_preread_cmd == opcode || sh_cacheread_cmd == opcode || sh_erase_cmd == opcode)	// no head
		req->head &= ~HEAD_MASK;

	if (sh_raidinit_cmd == opcode || sh_bufwrite_cmd == opcode)				// no multiplane
		req->head &= HEAD_MASK;

	return req;

free_data_out:
	free(req->data);
free_req_out:
	free(req);
out:
	return NULL;
}

struct shannon_request *alloc_request(struct shannon_dev *dev, enum shannon_cmd opcode,
				      int lun, int ppa, int head, int bsector, int nsector)
{
	return alloc_request_no_dma(dev, opcode, lun, ppa, head, bsector, nsector, 0);
}

static void raw_free_request(struct shannon_request *req)
{
	struct memory *mem, *tmp;

	list_for_each_entry_safe(mem, tmp, &req->mem_listhead, list) {
		list_del(&mem->list);
		req->dev->free_mem(req->dev, mem);
		free(mem);
	}

	if (req->metadata && req->bfree) free(req->metadata);
	if (req->data && req->bfree) free(req->data);
	free(req);
}

void free_request(struct shannon_request *req)
{
	struct shannon_request *sub, *tmp;

	list_for_each_entry_safe(sub, tmp, &req->chunk_list, chunk_list) {
		list_del(&sub->chunk_list);
		raw_free_request(sub);
	}
	raw_free_request(req);
}

/*
 * Generate commands and pad them to cmdqueue according to req.
 */
static void pad_cmdqueue(struct shannon_dev *dev, struct shannon_request *req, void *cmddata, int ncmddata)
{
	int i;

	if (req->bufcmd) {
		for (i = 0; i < ncmddata; i += QW_SIZE) {
			dev->write_mem(dev, dev->bufhead[req->head].cmdmem.kernel_addr + dev->bufhead[req->head].cmdhead, cmddata + i, QW_SIZE);
			dev->bufhead[req->head].cmdhead = (dev->bufhead[req->head].cmdhead + QW_SIZE) % PAGE_SIZE;
			dev->bufhead[req->head].cmdempty -= QW_SIZE;
		}
	} else {
		for (i = 0; i < ncmddata; i += QW_SIZE) {
			dev->write_mem(dev, dev->lun[req->lun].thread->cmdmem.kernel_addr + dev->lun[req->lun].thread->cmdhead, cmddata + i, QW_SIZE);
			dev->lun[req->lun].thread->cmdhead = (dev->lun[req->lun].thread->cmdhead + QW_SIZE) % PAGE_SIZE;
			dev->lun[req->lun].thread->cmdempty -= QW_SIZE;
		}
		if (-1 == dev->lun[req->lun].head)
			dev->lun[req->lun].head = req->cmdhead;
		dev->lun[req->lun].tail = (req->cmdhead + ncmddata) % PAGE_SIZE;
		dev->lun[req->lun].thread->req_count += (ncmddata / req->cmdlen);
	}
}

#if 0
static void back_pad_cmdqueue(struct shannon_dev *dev, struct shannon_request *req)
{
	int i;
	__u64 data = 0x00;

	if (req->bufcmd) {
		for (i = 0; i < req->cmdlen; i += QW_SIZE)
			dev->write_mem(dev, dev->bufhead[req->head].cmdmem.kernel_addr + (req->cmdhead + i)%PAGE_SIZE, &data, QW_SIZE);
	} else {
		for (i = 0; i < req->cmdlen; i += QW_SIZE)
			dev->write_mem(dev, dev->lun[req->lun].cmdmem.kernel_addr + (req->cmdhead + i)%PAGE_SIZE, &data, QW_SIZE);
	}
}
#else
static void back_pad_cmdqueue(struct shannon_dev *dev, struct shannon_request *req) {}
#endif

int submit_request(struct shannon_request *req)
{
	int i, cmdlen, plane, nplane, rc;
	void *cmddata;
	int ncmddata;
	struct list_head tmp_chunk_head;
	struct shannon_request *tmp;
	struct memory *mem;
	struct shannon_dev *dev = req->dev;

	struct sh_reset *sh_reset;
	struct sh_readid *sh_readid;
	struct sh_writereg *sh_writereg;
	struct sh_erase *sh_erase;
	struct sh_preread *sh_preread;
	struct sh_cacheread *sh_cacheread;
	struct sh_write *sh_write;
	struct sh_raidinit *sh_raidinit;
	struct sh_raidwrite *sh_raidwrite;
	struct sh_bufwrite *sh_bufwrite;

	rc = 0;
	plane = 0;
	nplane = list_empty(&req->chunk_list) ? 1 : dev->config->nplane;
	assert(nplane == 1 || nplane == 2 || nplane == 4 || nplane == 8);

	INIT_LIST_HEAD(&tmp_chunk_head);

	switch (req->opcode) {
	case sh_reset_cmd:
		cmdlen = sizeof(*sh_reset);
		if (cmdlen > dev->lun[req->lun].thread->cmdempty) {
			rc = NO_CMDQUEUE_ROOM;
			goto out;
		}

		sh_reset = zmalloc(cmdlen);
		if (NULL == sh_reset) {
			rc = ALLOCMEM_FAILED;
			goto out;
		}
		cmddata = sh_reset;
		ncmddata = cmdlen;

		sh_reset->opcode = sh_reset_cmd;
		sh_reset->lun = dev->lun[req->lun].phylun;

		req->cmdlen = cmdlen;
		req->cmdhead = dev->lun[req->lun].thread->cmdhead;
		break;

	case sh_readid_cmd:
		cmdlen = sizeof(*sh_readid);
		if (cmdlen > dev->lun[req->lun].thread->cmdempty) {
			rc = NO_CMDQUEUE_ROOM;
			goto out;
		}

		sh_readid = zmalloc(cmdlen);
		if (NULL == sh_readid) {
			rc = ALLOCMEM_FAILED;
			goto out;
		}
		cmddata = sh_readid;
		ncmddata = cmdlen;

		sh_readid->opcode = sh_readid_cmd;
		sh_readid->lun = dev->lun[req->lun].phylun;
#if 1
		sh_readid->addr = 0x00;		// read id
		sh_readid->cmd = 0x90;
		sh_readid->nbyte = 8;
#else
		sh_readid->addr = 0x10;		// read feature
		sh_readid->cmd = 0xEE;
		sh_readid->nbyte = 4;
#endif

		req->cmdlen = cmdlen;
		req->cmdhead = dev->lun[req->lun].thread->cmdhead;
		break;

	case sh_writereg_cmd:
		cmdlen = sizeof(*sh_writereg) + ((req->wr_flash_reg_nbyte > 1) ? 8 : 0);
		if (cmdlen > dev->lun[req->lun].thread->cmdempty) {
			rc = NO_CMDQUEUE_ROOM;
			goto out;
		}

		sh_writereg = zmalloc(cmdlen);
		if (NULL == sh_writereg) {
			rc = ALLOCMEM_FAILED;
			goto out;
		}
		cmddata = sh_writereg;
		ncmddata = cmdlen;

		memcpy(sh_writereg, req->direct_cmdqueue, cmdlen);

		req->cmdlen = cmdlen;
		req->cmdhead = dev->lun[req->lun].thread->cmdhead;
		break;

	case sh_erase_cmd:
		cmdlen = sizeof(*sh_erase);
		if (cmdlen * nplane > dev->lun[req->lun].thread->cmdempty) {
			rc = NO_CMDQUEUE_ROOM;
			goto out;
		}

		sh_erase = zmalloc(cmdlen * nplane);
		if (NULL == sh_erase) {
			rc = ALLOCMEM_FAILED;
			goto out;
		}
		cmddata = sh_erase;
		ncmddata = nplane * cmdlen;

		list_add_tail(&tmp_chunk_head, &req->chunk_list);
		list_for_each_entry(tmp, &tmp_chunk_head, chunk_list) {
			sh_erase->opcode = sh_erase_cmd;
			sh_erase->head = tmp->head;
			sh_erase->ppa = cpu_to_le32(tmp->ppa | (dev->lun[req->lun].phylun << 24));

			tmp->cmdlen = cmdlen;
			tmp->cmdhead = (dev->lun[tmp->lun].thread->cmdhead + plane * cmdlen) % PAGE_SIZE;

			plane++;
			sh_erase++;
		}
		list_del(&tmp_chunk_head);
		break;

	case sh_preread_cmd:
		cmdlen = sizeof(*sh_preread);
		if (cmdlen * nplane > dev->lun[req->lun].thread->cmdempty) {
			rc = NO_CMDQUEUE_ROOM;
			goto out;
		}

		sh_preread = zmalloc(cmdlen * nplane);
		if (NULL == sh_preread) {
			rc = ALLOCMEM_FAILED;
			goto out;
		}
		cmddata = sh_preread;
		ncmddata = cmdlen * nplane;

		list_add_tail(&tmp_chunk_head, &req->chunk_list);
		list_for_each_entry(tmp, &tmp_chunk_head, chunk_list) {
			sh_preread->opcode = sh_preread_cmd;
			sh_preread->head = tmp->head;
			sh_preread->ppa = cpu_to_le32(tmp->ppa | (dev->lun[req->lun].phylun << 24));

			tmp->cmdlen = cmdlen;
			tmp->cmdhead = (dev->lun[tmp->lun].thread->cmdhead + plane * cmdlen) % PAGE_SIZE;

			plane++;
			sh_preread++;
		}
		list_del(&tmp_chunk_head);
		break;

	case sh_cacheread_cmd:
		cmdlen = sizeof(*sh_cacheread) + req->nsector * METADATA_SIZE;
		if (cmdlen > dev->lun[req->lun].thread->cmdempty) {
			rc = NO_CMDQUEUE_ROOM;
			goto out;
		}

		sh_cacheread = zmalloc(cmdlen);
		if (NULL == sh_cacheread) {
			rc = ALLOCMEM_FAILED;
			goto out;
		}
		cmddata = sh_cacheread;
		ncmddata = cmdlen;

		sh_cacheread->opcode = req->last_cacheread ? sh_last_cacheread_cmd : sh_cacheread_cmd;
		sh_cacheread->bsector = req->bsector;
		sh_cacheread->nsector = req->nsector - 1;
		sh_cacheread->head = req->no_dma ? (0x20 | req->head) : req->head;
		sh_cacheread->ppa = cpu_to_le32(req->ppa | (dev->lun[req->lun].phylun << 24));
		if (dev->has_advance_read && (dev->advance_read || req->advance_read)){
                        sh_cacheread->opcode = sh_cacheread_adv_cmd;
                        sh_cacheread->head = 0;
		}

		if (!req->no_dma) {
			for (i = 0; i < req->nsector; i++) {
				mem = zmalloc(sizeof(*mem));
				assert(NULL != mem);		// FIXME: please use error process instead of assert
				mem->size = dev->config->sector_size;
				dev->get_mem(dev, mem);
				sh_cacheread->pte[i] = mem->dma_addr;
				cpu_to_le64s(&sh_cacheread->pte[i]);
				list_add_tail(&mem->list, &req->mem_listhead);
			}
		}

		req->cmdlen = cmdlen;
		req->cmdhead = dev->lun[req->lun].thread->cmdhead;
		break;

	case sh_write_cmd:
		cmdlen = sizeof(struct sh_write) + dev->config->page_nsector * sizeof(struct sh_write_sector);
		if (cmdlen * nplane > dev->lun[req->lun].thread->cmdempty) {
			rc = NO_CMDQUEUE_ROOM;
			goto out;
		}

		sh_write = zmalloc(cmdlen * nplane);
		if (NULL == sh_write) {
			rc =ALLOCMEM_FAILED;
			goto out;
		}
		cmddata = sh_write;
		ncmddata = cmdlen * nplane;

		list_add_tail(&tmp_chunk_head, &req->chunk_list);
		list_for_each_entry(tmp, &tmp_chunk_head, chunk_list) {
			sh_write->opcode = sh_write_cmd;
			sh_write->head = tmp->head;
			sh_write->ppa = cpu_to_le32(tmp->ppa | (dev->lun[req->lun].phylun << 24));
			tmp->cmdlen = cmdlen;
			tmp->cmdhead = (dev->lun[tmp->lun].thread->cmdhead + plane * cmdlen) % PAGE_SIZE;

			if (!req->no_dma) {
				for (i = 0; i < dev->config->page_nsector; i++) {
					mem = zmalloc(sizeof(*mem));
					assert(NULL != mem);		// FIXME: please use error process instead of assert

					mem->size = dev->config->sector_size;
					dev->get_mem(dev, mem);
					if (i < tmp->nsector) {
						if (req->rw_entire_buffer) {
							dev->write_mem(dev, mem->kernel_addr, tmp->data + i * (dev->config->sector_size + METADATA_SIZE), dev->config->sector_size);
							sh_write->sector[i].metadata = *((__u64 *)(tmp->data + i * (dev->config->sector_size + METADATA_SIZE) + dev->config->sector_size));
						} else {
							dev->write_mem(dev, mem->kernel_addr, tmp->data + i * dev->config->sector_size, dev->config->sector_size);
							sh_write->sector[i].metadata = tmp->metadata[i];
						}
						sh_write->sector[i].pte = mem->dma_addr;
					} else {
						dev->write_mem(dev, mem->kernel_addr, dev->padding_buffer + i * dev->config->sector_size, dev->config->sector_size);
						sh_write->sector[i].pte = mem->dma_addr;
						sh_write->sector[i].metadata = 0xA5A5A5A5;
					}
					cpu_to_le64s(&sh_write->sector[i].pte);
					cpu_to_le64s(&sh_write->sector[i].metadata);
					list_add_tail(&mem->list, &tmp->mem_listhead);
				}
			} else {
				// the hw backdoor of no dma at all. DMA reading from NULL is ok on X86
				for (i = 0; i < dev->config->page_nsector; i++) {
					sh_write->sector[i].pte = cpu_to_le64(0x1);
					// sh_write->sector[i].pte = cpu_to_le64(dev->dummy_mem.dma_addr);
					sh_write->sector[i].metadata = cpu_to_le64(0xA5A5A5A5);
				}
			}

			plane++;
			sh_write = (struct sh_write *)((__u64)sh_write + cmdlen);
		}
		list_del(&tmp_chunk_head);
		break;

	case sh_raidinit_cmd:
		cmdlen = sizeof(*sh_raidinit);
		if (cmdlen > dev->lun[req->lun].thread->cmdempty) {
			rc = NO_CMDQUEUE_ROOM;
			goto out;
		}

		sh_raidinit = zmalloc(cmdlen);
		if (NULL == sh_raidinit) {
			rc = ALLOCMEM_FAILED;
			goto out;
		}
		cmddata = sh_raidinit;
		ncmddata = cmdlen;

		sh_raidinit->opcode = sh_raidinit_cmd;
		sh_raidinit->ndatalun = req->nsector;	/* ns used for ndatalun */
		sh_raidinit->head = req->head;
		sh_raidinit->ppa = cpu_to_le32(req->ppa | (dev->lun[req->lun].phylun << 24));

		req->cmdlen = cmdlen;
		req->cmdhead = dev->lun[req->lun].thread->cmdhead;
		break;

	case sh_raidwrite_cmd:
		cmdlen = sizeof(*sh_raidwrite);
		if (cmdlen * nplane > dev->lun[req->lun].thread->cmdempty) {
			rc = NO_CMDQUEUE_ROOM;
			goto out;
		}

		sh_raidwrite = zmalloc(cmdlen * nplane);
		if (NULL == sh_raidwrite) {
			rc = ALLOCMEM_FAILED;
			goto out;
		}
		cmddata = sh_raidwrite;
		ncmddata = cmdlen * nplane;

		list_add_tail(&tmp_chunk_head, &req->chunk_list);
		list_for_each_entry(tmp, &tmp_chunk_head, chunk_list) {
			sh_raidwrite->opcode = sh_raidwrite_cmd;
			sh_raidwrite->head = tmp->head;
			sh_raidwrite->ppa = cpu_to_le32(tmp->ppa | (dev->lun[req->lun].phylun << 24));

			tmp->cmdlen = cmdlen;
			tmp->cmdhead = (dev->lun[tmp->lun].thread->cmdhead + cmdlen * plane) % PAGE_SIZE;

			plane++;
			sh_raidwrite++;
		}
		list_del(&tmp_chunk_head);
		break;

	case sh_bufwrite_cmd:
		cmdlen = sizeof(*sh_bufwrite);
		if (cmdlen > dev->bufhead[req->head].cmdempty) {
			rc = NO_CMDQUEUE_ROOM;
			goto out;
		}

		sh_bufwrite = zmalloc(cmdlen);
		if (NULL == sh_bufwrite) {
			rc = ALLOCMEM_FAILED;
			goto out;
		}
		cmddata = sh_bufwrite;
		ncmddata = cmdlen;

		sh_bufwrite->opcode = sh_bufwrite_cmd;
		sh_bufwrite->lun = log2phy_lun(dev, req->lun);
		sh_bufwrite->bsector = req->bsector;
		sh_bufwrite->head = req->head;
		sh_bufwrite->ppa = cpu_to_le32(req->ppa);
		mem = zmalloc(sizeof(*mem));
		if (NULL == mem) {
			free(sh_bufwrite);
			rc = ALLOCMEM_FAILED;
			goto out;
		}
		mem->size = dev->config->sector_size;
		dev->get_mem(dev, mem);
		dev->write_mem(dev, mem->kernel_addr, req->data, dev->config->sector_size);
		sh_bufwrite->pte = mem->dma_addr;
		sh_bufwrite->metadata = req->metadata[0];
		cpu_to_le64s(&sh_bufwrite->pte);
		cpu_to_le64s(&sh_bufwrite->metadata);
		list_add_tail(&mem->list, &req->mem_listhead);

		req->cmdlen = cmdlen;
		req->cmdhead = dev->bufhead[req->head].cmdhead; assert(req->head < INDEP_HEAD);
		break;

	default:
		printf("%s(): No such command\n", __func__);
		rc = ERR;
		goto out;
	}

	pad_cmdqueue(dev, req, cmddata, ncmddata);
	free(cmddata);

	if (sh_bufwrite_cmd == req->opcode || sh_bufread_cmd == req->opcode) {
		list_add_tail(&req->bufhead_list, &dev->bufhead[req->head].req_listhead);
	} else {
		list_add_tail(&req->lun_list, &dev->lun[req->lun].req_listhead);
		list_for_each_entry(tmp, &req->chunk_list, chunk_list)
			list_add_tail(&tmp->lun_list, &dev->lun[tmp->lun].req_listhead);
	}

out:
	return rc;
}

int __poll_cmdqueue(struct shannon_dev *dev, int lun, int wait)
{
	int i, pos, timeout, thread_cmphead, thread_cmdhead;
	struct memory *mem, *mem_tmp;
	struct shannon_request *req, *req_tmp;
	__u8 *p;
	void *cmp_queue;

	/* wait command queue execute completely */
	if (wait) {
		timeout = 0;

		while (1) {
			thread_cmdhead = dev->ioread_lunreg(dev, lun, HW_cmdq_head);
			thread_cmphead = dev->ioread_lunreg(dev, lun, HW_cmpq_head);

			if (dev->lun[lun].head == dev->lun[lun].tail) {
				return 0;
			} else if (thread_cmdhead == thread_cmphead) {
				break;
			}

			usleep(10);
			if (timeout++ > 10 * RUNCMDQ_US_TIMEOUT) {	// 80ms
				printf("\nHW Firmware BUG: lun %d command completion is not to return!\n", lun);
				exit(EXIT_FAILURE);
			}
		}
	}

	dev->lun[lun].head = dev->lun[lun].tail = -1;

	/* lookup finished req, copy completion, free memory, copy read data if it is read req */
	list_for_each_entry_safe(req, req_tmp, &dev->lun[lun].req_listhead, lun_list) {
		back_pad_cmdqueue(dev, req);
		list_del(&req->lun_list);
		p = req->data;

		/* just for read/write have mem list */
		list_for_each_entry_safe(mem, mem_tmp, &req->mem_listhead, list) {
			list_del(&mem->list);

			if (sh_cacheread_cmd == req->opcode && NULL != p) {
				dev->read_mem(dev, p, mem->kernel_addr, dev->config->sector_size);
				if (req->rw_entire_buffer)
					p += (dev->config->sector_size + METADATA_SIZE);
				else
					p += dev->config->sector_size;
			}

			dev->free_mem(dev, mem);
			free(mem);
		}

		/* copy completion data to user buffer */
		p = req->data + dev->config->sector_size;
		cmp_queue = dev->lun[lun].thread->cmpmem.kernel_addr;

		if (sh_cacheread_cmd != req->opcode) {
			dev->read_mem(dev, &req->status, cmp_queue + req->cmdhead, QW_SIZE);
			if (sh_readid_cmd != req->opcode)
				le64_to_cpus(&req->status);
		} else {
			dev->read_mem(dev, req->ecc, cmp_queue + req->cmdhead, req->nsector);	/* ecc */

			if (req->metadata != NULL || req->rw_entire_buffer) {
				for (i = 0; i < req->nsector; i++) {				/* metadata */
					pos = (req->cmdhead + (1 + i) * QW_SIZE) % PAGE_SIZE;
					if (req->rw_entire_buffer) {
						dev->read_mem(dev, p + i * (dev->config->sector_size + METADATA_SIZE), cmp_queue + pos, QW_SIZE);
						le64_to_cpus((__u64 *)(p + i * (dev->config->sector_size + METADATA_SIZE)));
					} else {
						dev->read_mem(dev, req->metadata + i, cmp_queue + pos, QW_SIZE);
						le64_to_cpus(req->metadata + i);
					}
				}
			}
		}

		if (--dev->lun[lun].thread->req_count == 0)
			dev->lun[lun].thread->cmdempty = PAGE_SIZE - 8;
	}

	return 0;
}

int __poll_bufcmdqueue(struct shannon_dev *dev, int head, int wait)
{
	int timeout;
	struct memory *mem, *mem_tmp;
	struct shannon_request *req, *req_tmp;
	void *cmp_queue;

	/* wait command queue execute completely */
	if (wait) {
		timeout = 0;

		while (dev->ioread_buflunreg(dev, head, HW_cmpq_head) != dev->bufhead[head].cmdhead) {
			usleep(1);
			if (timeout++ > 10 * RUNCMDQ_US_TIMEOUT) {	// 80ms
				if (dev->timeout_silent)
					printf("### bufwrite/read head-%d wait for completion timeout ###\n", head);
				return ERR;
			}
		}
	}

	/* lookup finished req, copy completion, free memory, copy read data if it is read req */
	list_for_each_entry_safe(req, req_tmp, &dev->bufhead[head].req_listhead, bufhead_list) {
		if (req->cmdhead == dev->ioread_buflunreg(dev, head, HW_cmpq_head))
			return 0;

		back_pad_cmdqueue(dev, req);
		list_del(&req->lun_list);

		list_for_each_entry_safe(mem, mem_tmp, &req->mem_listhead, list) {
			list_del(&mem->list);
			dev->free_mem(dev, mem);
			free(mem);
		}

		cmp_queue = dev->bufhead[head].cmpmem.kernel_addr;
		dev->read_mem(dev, &req->status, cmp_queue + req->cmdhead, QW_SIZE);

		dev->bufhead[head].cmdempty += req->cmdlen;
	}

	return 0;
}

/*
 * this function submit all req in the list, wait if necessarily,
 */
void submit_polling_loop(struct shannon_dev *dev, struct list_head *req_head)
{
	struct shannon_request *req;
	int rc, lun;

	/* submit all request and execute them */
	list_for_each_entry(req, req_head, list) {
	again:
		rc = dev->submit_request(req);
		if (rc == 0) {
			continue;
		}
		else if (rc != NO_CMDQUEUE_ROOM)
			submit_failed_exit(req->lun);

		for_dev_each_lun(dev, lun)
			update_cmdqueue(dev, lun);

		for_dev_each_lun(dev, lun)
			poll_cmdqueue(dev, lun);
		goto again;
	}
	// make sure everything is flushed
	for_dev_each_lun(dev, lun)
		update_cmdqueue(dev, lun);
	for_dev_each_lun(dev, lun)
		poll_cmdqueue(dev, lun);

}

/*----------------------------------------------------------------------------------------------------------------------------------*/
