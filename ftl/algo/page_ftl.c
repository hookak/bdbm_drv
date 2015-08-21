/*
The MIT License (MIT)

Copyright (c) 2014-2015 CSAIL, MIT

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

#if defined (KERNEL_MODE)
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/log2.h>

#elif defined (USER_MODE)
#include <stdio.h>
#include <stdint.h>
#include "3rd/uilog.h"
#include "utils/upage.h"

#else
#error Invalid Platform (KERNEL_MODE or USER_MODE)
#endif

#include "bdbm_drv.h"
#include "params.h"
#include "debug.h"

#include "utils/utime.h"
#include "utils/ufile.h"
#include "algo/abm.h"
#include "algo/page_ftl.h"


/* FTL interface */
bdbm_ftl_inf_t _ftl_page_ftl = {
	.ptr_private = NULL,
	.create = bdbm_page_ftl_create,
	.destroy = bdbm_page_ftl_destroy,
	.get_free_ppa = bdbm_page_ftl_get_free_ppa,
	.get_ppa = bdbm_page_ftl_get_ppa,
	.map_lpa_to_ppa = bdbm_page_ftl_map_lpa_to_ppa,
	.invalidate_lpa = bdbm_page_ftl_invalidate_lpa,
	.do_gc = bdbm_page_ftl_do_gc,
	.is_gc_needed = bdbm_page_ftl_is_gc_needed,
	.scan_badblocks = bdbm_page_badblock_scan,
	.load = bdbm_page_ftl_load,
	.store = bdbm_page_ftl_store,
	.get_segno = NULL,
};


/* data structures for block-level FTL */
enum BDBM_PFTL_PAGE_STATUS {
	PFTL_PAGE_NOT_ALLOCATED = 0,
	PFTL_PAGE_VALID,
	PFTL_PAGE_INVALID,
	PFTL_PAGE_INVALID_ADDR = -1ULL,
};

struct bdbm_page_mapping_entry {
	uint8_t status; /* BDBM_PFTL_PAGE_STATUS */
	bdbm_phyaddr_t phyaddr; /* physical location */
};

struct bdbm_page_ftl_private {
	bdbm_abm_info_t* bai;
	struct bdbm_page_mapping_entry* ptr_mapping_table;
	bdbm_spinlock_t ftl_lock;
	uint64_t nr_punits;	

	/* for the management of active blocks */
	uint64_t curr_puid;
	uint64_t curr_page_ofs;
	bdbm_abm_block_t** ac_bab;

	/* reserved for gc (reused whenever gc is invoked) */
	bdbm_abm_block_t** gc_bab;
	bdbm_hlm_req_gc_t gc_hlm;

	/* for bad-block scanning */
	bdbm_mutex_t badblk;
};


struct bdbm_page_mapping_entry* __bdbm_page_ftl_create_mapping_table (nand_params_t* np)
{
	struct bdbm_page_mapping_entry* me;
	uint64_t loop;

	/* create a page-level mapping table */
	if ((me = (struct bdbm_page_mapping_entry*)bdbm_zmalloc 
			(sizeof (struct bdbm_page_mapping_entry) * np->nr_pages_per_ssd)) == NULL) {
		return NULL;
	}

	/* initialize a page-level mapping table */
	for (loop = 0; loop < np->nr_pages_per_ssd; loop++) {
		me[loop].status = PFTL_PAGE_NOT_ALLOCATED;
		me[loop].phyaddr.channel_no = PFTL_PAGE_INVALID_ADDR;
		me[loop].phyaddr.chip_no = PFTL_PAGE_INVALID_ADDR;
		me[loop].phyaddr.block_no = PFTL_PAGE_INVALID_ADDR;
		me[loop].phyaddr.page_no = PFTL_PAGE_INVALID_ADDR;
	}

	/* return a set of mapping entries */
	return me;
}

void __bdbm_page_ftl_destroy_mapping_table (
	struct bdbm_page_mapping_entry* me)
{
	if (me == NULL)
		return;
	bdbm_free (me);
}

uint32_t __bdbm_page_ftl_get_active_blocks (
	nand_params_t* np,
	bdbm_abm_info_t* bai,
	bdbm_abm_block_t** bab)
{
	uint64_t i, j;

	/* get a set of free blocks for active blocks */
	for (i = 0; i < np->nr_channels; i++) {
		for (j = 0; j < np->nr_chips_per_channel; j++) {
			/* prepare & commit free blocks */
			if ((*bab = bdbm_abm_get_free_block_prepare (bai, i, j))) {
				bdbm_abm_get_free_block_commit (bai, *bab);
				/*bdbm_msg ("active blk = %p", *bab);*/
				bab++;
			} else {
				bdbm_error ("bdbm_abm_get_free_block_prepare failed");
				return 1;
			}
		}
	}

	return 0;
}

bdbm_abm_block_t** __bdbm_page_ftl_create_active_blocks (
	nand_params_t* np,
	bdbm_abm_info_t* bai)
{
	uint64_t nr_punits;
	bdbm_abm_block_t** bab = NULL;

	nr_punits = np->nr_chips_per_channel * np->nr_channels;

	/*bdbm_msg ("nr_punits: %llu", nr_punits);*/

	/* create a set of active blocks */
	if ((bab = (bdbm_abm_block_t**)bdbm_zmalloc 
			(sizeof (bdbm_abm_block_t*) * nr_punits)) == NULL) {
		bdbm_error ("bdbm_zmalloc failed");
		goto fail;
	}

	/* get a set of free blocks for active blocks */
	if (__bdbm_page_ftl_get_active_blocks (np, bai, bab) != 0) {
		bdbm_error ("__bdbm_page_ftl_get_active_blocks failed");
		goto fail;
	}

	return bab;

fail:
	if (bab)
		bdbm_free (bab);
	return NULL;
}

void __bdbm_page_ftl_destroy_active_blocks (
	bdbm_abm_block_t** bab)
{
	if (bab == NULL)
		return;

	/* TODO: it might be required to save the status of active blocks 
	 * in order to support rebooting */
	bdbm_free (bab);
}

uint32_t bdbm_page_ftl_create (bdbm_drv_info_t* bdi)
{
	struct bdbm_page_ftl_private* p = NULL;
	nand_params_t* np = BDBM_GET_NAND_PARAMS (bdi);
	uint64_t i = 0, j = 0;
	uint64_t nr_kp_per_fp = np->page_main_size / KERNEL_PAGE_SIZE;	/* e.g., 2 = 8 KB / 4 KB */

	/* create a private data structure */
	if ((p = (struct bdbm_page_ftl_private*)bdbm_zmalloc 
			(sizeof (struct bdbm_page_ftl_private))) == NULL) {
		bdbm_error ("bdbm_malloc failed");
		return 1;
	}
	p->curr_puid = 0;
	p->curr_page_ofs = 0;
	p->nr_punits = np->nr_chips_per_channel * np->nr_channels;
	bdbm_spin_lock_init (&p->ftl_lock);
	_ftl_page_ftl.ptr_private = (void*)p;

	/* create 'bdbm_abm_info' with pst */
	if ((p->bai = bdbm_abm_create (np, 1)) == NULL) {
		bdbm_error ("bdbm_abm_create failed");
		bdbm_page_ftl_destroy (bdi);
		return 1;
	}

	/* create a mapping table */
	if ((p->ptr_mapping_table = __bdbm_page_ftl_create_mapping_table (np)) == NULL) {
		bdbm_error ("__bdbm_page_ftl_create_mapping_table failed");
		bdbm_page_ftl_destroy (bdi);
		return 1;
	}

	/* allocate active blocks */
	if ((p->ac_bab = __bdbm_page_ftl_create_active_blocks (np, p->bai)) == NULL) {
		bdbm_error ("__bdbm_page_ftl_create_active_blocks failed");
		bdbm_page_ftl_destroy (bdi);
		return 1;
	}

	/* allocate gc stuffs */
	if ((p->gc_bab = (bdbm_abm_block_t**)bdbm_zmalloc 
			(sizeof (bdbm_abm_block_t*) * p->nr_punits)) == NULL) {
		bdbm_error ("bdbm_zmalloc failed");
		bdbm_page_ftl_destroy (bdi);
		return 1;
	}
	if ((p->gc_hlm.llm_reqs = (bdbm_llm_req_t*)bdbm_zmalloc
			(sizeof (bdbm_llm_req_t) * p->nr_punits * np->nr_pages_per_block)) == NULL) {
		bdbm_error ("bdbm_zmalloc failed");
		bdbm_page_ftl_destroy (bdi);
		return 1;
	}

	while (i < p->nr_punits * np->nr_pages_per_block) {
		bdbm_llm_req_t* r = &p->gc_hlm.llm_reqs[i];
		r->kpg_flags = NULL;
		r->pptr_kpgs = (uint8_t**)bdbm_malloc_atomic (sizeof(uint8_t*) * nr_kp_per_fp);
		for (j = 0; j < nr_kp_per_fp; j++)
			r->pptr_kpgs[j] = (uint8_t*)get_zeroed_page (GFP_KERNEL);
		r->ptr_oob = (uint8_t*)bdbm_malloc_atomic (sizeof (uint8_t) * np->page_oob_size);
		i++;
	}
	bdbm_mutex_init (&p->gc_hlm.gc_done);

	return 0;
}

void bdbm_page_ftl_destroy (bdbm_drv_info_t* bdi)
{
	struct bdbm_page_ftl_private* p = _ftl_page_ftl.ptr_private;
	nand_params_t* np = BDBM_GET_NAND_PARAMS (bdi);

	if (!p)
		return;

	if (p->gc_hlm.llm_reqs) {
		uint64_t i = 0, j = 0;
		uint64_t nr_kp_per_fp = np->page_main_size / KERNEL_PAGE_SIZE;	/* e.g., 2 = 8 KB / 4 KB */
		while (i < p->nr_punits * np->nr_pages_per_block) {
			bdbm_llm_req_t* r = &p->gc_hlm.llm_reqs[i];
			for (j = 0; j < nr_kp_per_fp; j++)
				free_page ((unsigned long)r->pptr_kpgs[j]);
			bdbm_free_atomic (r->pptr_kpgs);
			bdbm_free_atomic (r->ptr_oob);
			i++;
		}
		bdbm_free (p->gc_hlm.llm_reqs);
	}
	if (p->gc_bab)
		bdbm_free (p->gc_bab);
	if (p->ac_bab)
		__bdbm_page_ftl_destroy_active_blocks (p->ac_bab);
	if (p->ptr_mapping_table)
		__bdbm_page_ftl_destroy_mapping_table (p->ptr_mapping_table);
	if (p->bai)
		bdbm_abm_destroy (p->bai);
	bdbm_free (p);
}

uint32_t bdbm_page_ftl_get_free_ppa (bdbm_drv_info_t* bdi, uint64_t lpa, bdbm_phyaddr_t* ppa)
{
	struct bdbm_page_ftl_private* p = _ftl_page_ftl.ptr_private;
	nand_params_t* np = BDBM_GET_NAND_PARAMS (bdi);
	bdbm_abm_block_t* b = NULL;
	uint64_t curr_channel;
	uint64_t curr_chip;

	/* get the channel & chip numbers */
	curr_channel = p->curr_puid % np->nr_channels;
	curr_chip = p->curr_puid / np->nr_channels;

	/* get the physical offset of the active blocks */
	/*b = &p->ac_bab[curr_channel][curr_chip];*/
	b = p->ac_bab[curr_channel * np->nr_chips_per_channel + curr_chip];
	ppa->channel_no =  b->channel_no;
	ppa->chip_no = b->chip_no;
	ppa->block_no = b->block_no;
	ppa->page_no = p->curr_page_ofs;
	ppa->punit_id = GET_PUNIT_ID (bdi, ppa);

	/* check some error cases before returning the physical address */
	bdbm_bug_on (ppa->channel_no != curr_channel);
	bdbm_bug_on (ppa->chip_no != curr_chip);
	bdbm_bug_on (ppa->page_no >= np->nr_pages_per_block);

	/* go to the next parallel unit */
	if ((p->curr_puid + 1) == p->nr_punits) {
		p->curr_puid = 0;
		p->curr_page_ofs++;	/* go to the next page */

		/* see if there are sufficient free pages or not */
		if (p->curr_page_ofs == np->nr_pages_per_block) {
			/* get active blocks */
			if (__bdbm_page_ftl_get_active_blocks (np, p->bai, p->ac_bab) != 0) {
				bdbm_error ("__bdbm_page_ftl_get_active_blocks failed");
				return 1;
			}
			/* ok; go ahead with 0 offset */
			/*bdbm_msg ("curr_puid = %llu", p->curr_puid);*/
			p->curr_page_ofs = 0;
		}
	} else {
		/*bdbm_msg ("curr_puid = %llu", p->curr_puid);*/
		p->curr_puid++;
	}

	return 0;
}

uint32_t bdbm_page_ftl_map_lpa_to_ppa (bdbm_drv_info_t* bdi, uint64_t lpa, bdbm_phyaddr_t* ptr_phyaddr)
{
	nand_params_t* np = BDBM_GET_NAND_PARAMS (bdi);
	struct bdbm_page_ftl_private* p = _ftl_page_ftl.ptr_private;
	struct bdbm_page_mapping_entry* me = NULL;

	/* is it a valid logical address */
	if (lpa >= np->nr_pages_per_ssd) {
		bdbm_error ("LPA is beyond logical space (%llX)", lpa);
		return 1;
	}

	/* get the mapping entry for lpa */
	me = &p->ptr_mapping_table[lpa];
	bdbm_bug_on (me == NULL);

	/* update the mapping table */
	if (me->status == PFTL_PAGE_VALID) {
		bdbm_abm_invalidate_page (
			p->bai, 
			me->phyaddr.channel_no, 
			me->phyaddr.chip_no,
			me->phyaddr.block_no,
			me->phyaddr.page_no
		);
	}
	me->status = PFTL_PAGE_VALID;
	me->phyaddr.channel_no = ptr_phyaddr->channel_no;
	me->phyaddr.chip_no = ptr_phyaddr->chip_no;
	me->phyaddr.block_no = ptr_phyaddr->block_no;
	me->phyaddr.page_no = ptr_phyaddr->page_no;

	return 0;
}

uint32_t bdbm_page_ftl_get_ppa (bdbm_drv_info_t* bdi, uint64_t lpa, bdbm_phyaddr_t* ppa)
{
	nand_params_t* np = BDBM_GET_NAND_PARAMS (bdi);
	struct bdbm_page_ftl_private* p = _ftl_page_ftl.ptr_private;
	struct bdbm_page_mapping_entry* me = NULL;
	uint32_t ret;

	/* is it a valid logical address */
	if (lpa >= np->nr_pages_per_ssd) {
		bdbm_error ("A given lpa is beyond logical space (%llu)", lpa);
		return 1;
	}

	/* get the mapping entry for lpa */
	me = &p->ptr_mapping_table[lpa];

	/* NOTE: sometimes a file system attempts to read 
	 * a logical address that was not written before.
	 * in that case, we return 'address 0' */
	if (me->status != PFTL_PAGE_VALID) {
		ppa->channel_no = 0;
		ppa->chip_no = 0;
		ppa->block_no = 0;
		ppa->page_no = 0;
		ppa->punit_id = 0;
		ret = 1;
	} else {
		ppa->channel_no = me->phyaddr.channel_no;
		ppa->chip_no = me->phyaddr.chip_no;
		ppa->block_no = me->phyaddr.block_no;
		ppa->page_no = me->phyaddr.page_no;
		ppa->punit_id = GET_PUNIT_ID (bdi, ppa);
		ret = 0;
	}

	return ret;
}

uint32_t bdbm_page_ftl_invalidate_lpa (bdbm_drv_info_t* bdi, uint64_t lpa, uint64_t len)
{	
	nand_params_t* np = BDBM_GET_NAND_PARAMS (bdi);
	struct bdbm_page_ftl_private* p = _ftl_page_ftl.ptr_private;
	struct bdbm_page_mapping_entry* me = NULL;
	uint64_t loop;

	/* check the range of input addresses */
	if ((lpa + len) > np->nr_pages_per_ssd) {
		bdbm_warning ("LPA is beyond logical space (%llu = %llu+%llu) %llu", 
			lpa+len, lpa, len, np->nr_pages_per_ssd);
		return 1;
	}

	/* make them invalid */
	for (loop = lpa; loop < (lpa + len); loop++) {
		me = &p->ptr_mapping_table[loop];
		if (me->status == PFTL_PAGE_VALID) {
			bdbm_abm_invalidate_page (
				p->bai, 
				me->phyaddr.channel_no, 
				me->phyaddr.chip_no,
				me->phyaddr.block_no,
				me->phyaddr.page_no
			);
			me->status = PFTL_PAGE_INVALID;
		}
	}

	return 0;
}

uint8_t bdbm_page_ftl_is_gc_needed (bdbm_drv_info_t* bdi)
{
	struct bdbm_page_ftl_private* p = _ftl_page_ftl.ptr_private;
	uint64_t nr_total_blks = bdbm_abm_get_nr_total_blocks (p->bai);
	uint64_t nr_free_blks = bdbm_abm_get_nr_free_blocks (p->bai);

	/* invoke gc when remaining free blocks are less than 1% of total blocks */
	if ((nr_free_blks * 100 / nr_total_blks) <= 1) {
		return 1;
	}

	/* invoke gc when there is only one dirty block (for debugging) */
	/*
	struct bdbm_page_ftl_private* p = _ftl_page_ftl.ptr_private;
	if (bdbm_abm_get_nr_dirty_blocks (p->bai) > 1) {
		return 1;
	}
	*/

	return 0;
}

/* VICTIM SELECTION - First Selection:
 * select the first dirty block in a list */
bdbm_abm_block_t* __bdbm_page_ftl_victim_selection (
	bdbm_drv_info_t* bdi,
	uint64_t channel_no,
	uint64_t chip_no)
{
	struct bdbm_page_ftl_private* p = _ftl_page_ftl.ptr_private;
	nand_params_t* np = BDBM_GET_NAND_PARAMS (bdi);
	bdbm_abm_block_t* a = NULL;
	bdbm_abm_block_t* b = NULL;
	struct list_head* pos = NULL;

	a = p->ac_bab[channel_no*np->nr_chips_per_channel + chip_no];
	bdbm_abm_list_for_each_dirty_block (pos, p->bai, channel_no, chip_no) {
		b = bdbm_abm_fetch_dirty_block (pos);
		if (a != b)
			break;
		b = NULL;
	}

	return b;
}

/* VICTIM SELECTION - Greedy:
 * select a dirty block with a small number of valid pages */
bdbm_abm_block_t* __bdbm_page_ftl_victim_selection_greedy (
	bdbm_drv_info_t* bdi,
	uint64_t channel_no,
	uint64_t chip_no)
{
	struct bdbm_page_ftl_private* p = _ftl_page_ftl.ptr_private;
	nand_params_t* np = BDBM_GET_NAND_PARAMS (bdi);
	bdbm_abm_block_t* a = NULL;
	bdbm_abm_block_t* b = NULL;
	bdbm_abm_block_t* v = NULL;
	struct list_head* pos = NULL;

	a = p->ac_bab[channel_no*np->nr_chips_per_channel + chip_no];

	bdbm_abm_list_for_each_dirty_block (pos, p->bai, channel_no, chip_no) {
		b = bdbm_abm_fetch_dirty_block (pos);
		if (a == b)
			continue;
		if (b->nr_invalid_pages == np->nr_pages_per_block) {
			v = b;
			break;
		}
		if (v == NULL) {
			v = b;
			continue;
		}
		if (a->nr_invalid_pages > v->nr_invalid_pages)
			v = b;
	}

	return v;
}

/* TODO: need to improve it for background gc */
uint32_t bdbm_page_ftl_do_gc (bdbm_drv_info_t* bdi)
{
	struct bdbm_page_ftl_private* p = _ftl_page_ftl.ptr_private;
	nand_params_t* np = BDBM_GET_NAND_PARAMS (bdi);
	bdbm_hlm_req_gc_t* hlm_gc = &p->gc_hlm;
	uint64_t nr_gc_blks = 0;
	uint64_t nr_llm_reqs = 0;
	uint64_t nr_punits = 0;
	uint64_t i, j;

	bdbm_stopwatch_t sw;

	nr_punits = np->nr_channels * np->nr_chips_per_channel;

	/* choose victim blocks for individual parallel units */
	bdbm_memset (p->gc_bab, 0x00, sizeof (bdbm_abm_block_t*) * nr_punits);
	bdbm_stopwatch_start (&sw);
	for (i = 0, nr_gc_blks = 0; i < np->nr_channels; i++) {
		for (j = 0; j < np->nr_chips_per_channel; j++) {
			bdbm_abm_block_t* b; 
			if ((b = __bdbm_page_ftl_victim_selection_greedy (bdi, i, j))) {
				p->gc_bab[nr_gc_blks] = b;
				nr_gc_blks++;
			}
		}
	}
	if (nr_gc_blks < nr_punits) {
		/* TODO: we need to implement a load balancing feature to avoid this */
		/*bdbm_warning ("TODO: this warning will be removed with load-balancing");*/
		return 0;
	}

	/* build hlm_req_gc for reads */
	for (i = 0, nr_llm_reqs = 0; i < nr_gc_blks; i++) {
		bdbm_abm_block_t* b = p->gc_bab[i];
		if (b == NULL)
			break;
		for (j = 0; j < np->nr_pages_per_block; j++) {
			if (b->pst[j] != BDBM_ABM_PAGE_INVALID) {
				bdbm_llm_req_t* r = &hlm_gc->llm_reqs[nr_llm_reqs];
				r->req_type = REQTYPE_GC_READ;
				r->lpa = -1ULL; /* lpa is not available now */
				r->ptr_hlm_req = (void*)hlm_gc;
				r->phyaddr = &r->phyaddr_r;
				r->phyaddr->channel_no = b->channel_no;
				r->phyaddr->chip_no = b->chip_no;
				r->phyaddr->block_no = b->block_no;
				r->phyaddr->page_no = j;
				r->ret = 0;
				nr_llm_reqs++;
			}
		}
	}

	/*
	bdbm_msg ("----------------------------------------------");
	bdbm_msg ("gc-victim: %llu pages, %llu blocks, %llu us", 
		nr_llm_reqs, nr_gc_blks, bdbm_stopwatch_get_elapsed_time_us (&sw));
	*/

	/* wait until Q in llm becomes empty 
	 * TODO: it might be possible to further optimize this */
	bdi->ptr_llm_inf->flush (bdi);

	if (nr_llm_reqs == 0) 
		goto erase_blks;

	/* send read reqs to llm */
	hlm_gc->req_type = REQTYPE_GC_READ;
	hlm_gc->nr_done_reqs = 0;
	hlm_gc->nr_reqs = nr_llm_reqs;
	bdbm_mutex_lock (&hlm_gc->gc_done);
	for (i = 0; i < nr_llm_reqs; i++) {
		if ((bdi->ptr_llm_inf->make_req (bdi, &hlm_gc->llm_reqs[i])) != 0) {
			bdbm_error ("llm_make_req failed");
			bdbm_bug_on (1);
		}
	}
	bdbm_mutex_lock (&hlm_gc->gc_done);
	bdbm_mutex_unlock (&hlm_gc->gc_done);

	/* build hlm_req_gc for writes */
	for (i = 0; i < nr_llm_reqs; i++) {
		bdbm_llm_req_t* r = &hlm_gc->llm_reqs[i];
		r->req_type = REQTYPE_GC_WRITE;	/* change to write */
		r->lpa = ((uint64_t*)r->ptr_oob)[0]; /* update LPA */
		if (bdbm_page_ftl_get_free_ppa (bdi, r->lpa, r->phyaddr) != 0) {
			bdbm_error ("bdbm_page_ftl_get_free_ppa failed");
			bdbm_bug_on (1);
		}
		if (bdbm_page_ftl_map_lpa_to_ppa (bdi, r->lpa, r->phyaddr) != 0) {
			bdbm_error ("bdbm_page_ftl_map_lpa_to_ppa failed");
			bdbm_bug_on (1);
		}
	}

	/* send write reqs to llm */
	hlm_gc->req_type = REQTYPE_GC_WRITE;
	hlm_gc->nr_done_reqs = 0;
	hlm_gc->nr_reqs = nr_llm_reqs;
	bdbm_mutex_lock (&hlm_gc->gc_done);
	for (i = 0; i < nr_llm_reqs; i++) {
		if ((bdi->ptr_llm_inf->make_req (bdi, &hlm_gc->llm_reqs[i])) != 0) {
			bdbm_error ("llm_make_req failed");
			bdbm_bug_on (1);
		}
	}
	bdbm_mutex_lock (&hlm_gc->gc_done);
	bdbm_mutex_unlock (&hlm_gc->gc_done);

	/* erase blocks */
erase_blks:
	for (i = 0; i < nr_gc_blks; i++) {
		bdbm_abm_block_t* b = p->gc_bab[i];
		bdbm_llm_req_t* r = &hlm_gc->llm_reqs[i];
		r->req_type = REQTYPE_GC_ERASE;
		r->lpa = -1ULL; /* lpa is not available now */
		r->ptr_hlm_req = (void*)hlm_gc;
		r->phyaddr = &r->phyaddr_w;
		r->phyaddr->channel_no = b->channel_no;
		r->phyaddr->chip_no = b->chip_no;
		r->phyaddr->block_no = b->block_no;
		r->phyaddr->page_no = 0;
		r->ret = 0;
	}

	/* send erase reqs to llm */
	hlm_gc->req_type = REQTYPE_GC_ERASE;
	hlm_gc->nr_done_reqs = 0;
	hlm_gc->nr_reqs = nr_gc_blks;
	bdbm_mutex_lock (&hlm_gc->gc_done);
	for (i = 0; i < nr_gc_blks; i++) {
		if ((bdi->ptr_llm_inf->make_req (bdi, &hlm_gc->llm_reqs[i])) != 0) {
			bdbm_error ("llm_make_req failed");
			bdbm_bug_on (1);
		}
	}
	bdbm_mutex_lock (&hlm_gc->gc_done);
	bdbm_mutex_unlock (&hlm_gc->gc_done);

	/* FIXME: what happens if block erasure fails */
	for (i = 0; i < nr_gc_blks; i++) {
		uint8_t ret = 0;
		bdbm_abm_block_t* b = p->gc_bab[i];
		if (hlm_gc->llm_reqs[i].ret != 0) 
			ret = 1;	/* bad block */
/*
#ifdef EMULATE_BAD_BLOCKS
		{
			bdbm_llm_req_t* r = (bdbm_llm_req_t*)&hlm_gc->llm_reqs[i];
			if (r->phyaddr->block_no % 8 == 0) {
				bdbm_msg (" -- FTL: b:%llu c:%llu b:%llu (ret=%u)",
						r->phyaddr->channel_no, 
						r->phyaddr->chip_no, 
						r->phyaddr->block_no,
						r->ret);
			}
		}
#endif
*/
		bdbm_abm_erase_block (p->bai, b->channel_no, b->chip_no, b->block_no, ret);
	}

	return 0;
}

/* for snapshot */
uint32_t bdbm_page_ftl_load (bdbm_drv_info_t* bdi, const char* fn)
{
	struct bdbm_page_ftl_private* p = _ftl_page_ftl.ptr_private;
	nand_params_t* np = BDBM_GET_NAND_PARAMS (bdi);
	struct bdbm_page_mapping_entry* me;
	/*struct file* fp = NULL;*/
	bdbm_file_t fp = 0;
	uint64_t i, pos = 0;

	/* step1: load abm */
	if (bdbm_abm_load (p->bai, "/usr/share/bdbm_drv/abm.dat") != 0) {
		bdbm_error ("bdbm_abm_load failed");
		return 1;
	}

	/* step2: load mapping table */
	if ((fp = bdbm_fopen (fn, O_RDWR, 0777)) == 0) {
		bdbm_error ("bdbm_fopen failed");
		return 1;
	}

	me = p->ptr_mapping_table;
	for (i = 0; i < np->nr_pages_per_ssd; i++) {
		pos += bdbm_fread (fp, pos, (uint8_t*)&me[i], sizeof (struct bdbm_page_mapping_entry));
		if (me[i].status != PFTL_PAGE_NOT_ALLOCATED &&
			me[i].status != PFTL_PAGE_VALID &&
			me[i].status != PFTL_PAGE_INVALID &&
			me[i].status != PFTL_PAGE_INVALID_ADDR) {
			bdbm_msg ("snapshot: invalid status = %u", me[i].status);
		}
	}

	/* step3: get active blocks */
	if (__bdbm_page_ftl_get_active_blocks (np, p->bai, p->ac_bab) != 0) {
		bdbm_error ("__bdbm_page_ftl_get_active_blocks failed");
		bdbm_fclose (fp);
		return 1;
	}
	p->curr_puid = 0;
	p->curr_page_ofs = 0;

	bdbm_fclose (fp);

	return 0;
}

uint32_t bdbm_page_ftl_store (bdbm_drv_info_t* bdi, const char* fn)
{
	struct bdbm_page_ftl_private* p = _ftl_page_ftl.ptr_private;
	nand_params_t* np = BDBM_GET_NAND_PARAMS (bdi);
	struct bdbm_page_mapping_entry* me;
	bdbm_abm_block_t* b = NULL;
	/*struct file* fp = NULL;*/
	bdbm_file_t fp = 0;
	uint64_t pos = 0;
	uint64_t i, j;
	uint32_t ret;

	/* step1: make active blocks invalid (it's ugly!!!) */
	if ((fp = bdbm_fopen (fn, O_CREAT | O_WRONLY, 0777)) == 0) {
		bdbm_error ("bdbm_fopen failed");
		return 1;
	}

	while (1) {
		/* get the channel & chip numbers */
		i = p->curr_puid % np->nr_channels;
		j = p->curr_puid / np->nr_channels;

		/* get the physical offset of the active blocks */
		b = p->ac_bab[i*np->nr_chips_per_channel + j];

		/* invalidate remaining pages */
		bdbm_abm_invalidate_page (p->bai, 
			b->channel_no, b->chip_no, b->block_no, p->curr_page_ofs);
		bdbm_bug_on (b->channel_no != i);
		bdbm_bug_on (b->chip_no != j);

		/* go to the next parallel unit */
		if ((p->curr_puid + 1) == p->nr_punits) {
			p->curr_puid = 0;
			p->curr_page_ofs++;	/* go to the next page */

			/* see if there are sufficient free pages or not */
			if (p->curr_page_ofs == np->nr_pages_per_block) {
				p->curr_page_ofs = 0;
				break;
			}
		} else {
			p->curr_puid++;
		}
	}

	/* step2: store mapping table */
	me = p->ptr_mapping_table;
	for (i = 0; i < np->nr_pages_per_ssd; i++) {
		pos += bdbm_fwrite (fp, pos, (uint8_t*)&me[i], sizeof (struct bdbm_page_mapping_entry));
	}
	bdbm_fsync (fp);
	bdbm_fclose (fp);

	/* step3: store abm */
	ret = bdbm_abm_store (p->bai, "/usr/share/bdbm_drv/abm.dat");

	return ret;
}

void __bdbm_page_badblock_scan_eraseblks (
	bdbm_drv_info_t* bdi,
	uint64_t block_no)
{
	struct bdbm_page_ftl_private* p = _ftl_page_ftl.ptr_private;
	nand_params_t* np = BDBM_GET_NAND_PARAMS (bdi);
	bdbm_hlm_req_gc_t* hlm_gc = &p->gc_hlm;
	uint64_t i, j;

	/* setup blocks to erase */
	bdbm_memset (p->gc_bab, 0x00, sizeof (bdbm_abm_block_t*) * p->nr_punits);
	for (i = 0; i < np->nr_channels; i++) {
		for (j = 0; j < np->nr_chips_per_channel; j++) {
			bdbm_abm_block_t* b = NULL;
			bdbm_llm_req_t* r = NULL;
			uint64_t punit_id = i*np->nr_chips_per_channel+j;

			if ((b = bdbm_abm_get_block (p->bai, i, j, block_no)) == NULL) {
				bdbm_error ("oops! bdbm_abm_get_block failed");
				bdbm_bug_on (1);
			}
			p->gc_bab[punit_id] = b;

			r = &hlm_gc->llm_reqs[punit_id];
			r->req_type = REQTYPE_GC_ERASE;
			r->lpa = -1ULL; /* lpa is not available now */
			r->ptr_hlm_req = (void*)hlm_gc;
			r->phyaddr = &r->phyaddr_w;
			r->phyaddr->channel_no = b->channel_no;
			r->phyaddr->chip_no = b->chip_no;
			r->phyaddr->block_no = b->block_no;
			r->phyaddr->page_no = 0;
			r->ret = 0;
		}
	}

	/* send erase reqs to llm */
	hlm_gc->req_type = REQTYPE_GC_ERASE;
	hlm_gc->nr_done_reqs = 0;
	hlm_gc->nr_reqs = p->nr_punits;
	bdbm_mutex_lock (&hlm_gc->gc_done);
	for (i = 0; i < p->nr_punits; i++) {
		if ((bdi->ptr_llm_inf->make_req (bdi, &hlm_gc->llm_reqs[i])) != 0) {
			bdbm_error ("llm_make_req failed");
			bdbm_bug_on (1);
		}
	}
	bdbm_mutex_lock (&hlm_gc->gc_done);
	bdbm_mutex_unlock (&hlm_gc->gc_done);

	for (i = 0; i < p->nr_punits; i++) {
		uint8_t ret = 0;
		bdbm_abm_block_t* b = p->gc_bab[i];

		if (hlm_gc->llm_reqs[i].ret != 0) {
			ret = 1; /* bad block */
		}

		bdbm_abm_erase_block (p->bai, b->channel_no, b->chip_no, b->block_no, ret);
	}

	/* measure gc elapsed time */
}

uint32_t bdbm_page_badblock_scan (bdbm_drv_info_t* bdi)
{
	struct bdbm_page_ftl_private* p = _ftl_page_ftl.ptr_private;
	nand_params_t* np = BDBM_GET_NAND_PARAMS (bdi);
	struct bdbm_page_mapping_entry* me = NULL;
	uint64_t i = 0;
	uint32_t ret = 0;

	bdbm_msg ("[WARNING] 'bdbm_page_badblock_scan' is called! All of the flash blocks will be erased!!!");

	/* step1: reset the page-level mapping table */
	bdbm_msg ("step1: reset the page-level mapping table");
	me = p->ptr_mapping_table;
	for (i = 0; i < np->nr_pages_per_ssd; i++) {
		me[i].status = PFTL_PAGE_NOT_ALLOCATED;
		me[i].phyaddr.channel_no = PFTL_PAGE_INVALID_ADDR;
		me[i].phyaddr.chip_no = PFTL_PAGE_INVALID_ADDR;
		me[i].phyaddr.block_no = PFTL_PAGE_INVALID_ADDR;
		me[i].phyaddr.page_no = PFTL_PAGE_INVALID_ADDR;
	}

	/* step2: erase all the blocks */
	bdi->ptr_llm_inf->flush (bdi);
	for (i = 0; i < np->nr_blocks_per_chip; i++) {
		__bdbm_page_badblock_scan_eraseblks (bdi, i);
	}

	/* step3: store abm */
	if ((ret = bdbm_abm_store (p->bai, "/usr/share/bdbm_drv/abm.dat"))) {
		bdbm_error ("bdbm_abm_store failed");
		return 1;
	}

	/* step4: get active blocks */
	bdbm_msg ("step2: get active blocks");
	if (__bdbm_page_ftl_get_active_blocks (np, p->bai, p->ac_bab) != 0) {
		bdbm_error ("__bdbm_page_ftl_get_active_blocks failed");
		return 1;
	}
	p->curr_puid = 0;
	p->curr_page_ofs = 0;

	bdbm_msg ("done");
	 
	return 0;
}

