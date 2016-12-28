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

#include <stdio.h>
#include <stdint.h>

#include <unistd.h>
#include <pthread.h>

#include "bdbm_drv.h"
#include "debug.h"
#include "umemory.h"
#include "params.h"
#include "utime.h"

#include "dm_dummy.h"
#include "dev_params.h"


bdbm_dm_inf_t _bdbm_dm_inf = {
	.ptr_private = NULL,
	.probe = dm_user_probe,
	.open = dm_user_open,
	.close = dm_user_close,
	.make_req = dm_user_make_req,
	.end_req = dm_user_end_req,
	.load = dm_user_load,
	.store = dm_user_store,
};

/* private data structure for dm */
struct dm_user_private {
	bdbm_spinlock_t dm_lock;
	uint64_t w_cnt;
	uint64_t w_cnt_done;
        uint64_t* oob_data;
};

static void __dm_setup_device_params (bdbm_device_params_t* params)
{
	*params = get_default_device_params ();
}

uint32_t dm_user_probe (bdbm_drv_info_t* bdi, bdbm_device_params_t* params)
{
	struct dm_user_private* p = NULL;

	/* setup NAND parameters according to users' inputs */
	__dm_setup_device_params (params);

	/* create a private structure for ramdrive */
	if ((p = (struct dm_user_private*)bdbm_malloc_atomic
			(sizeof (struct dm_user_private))) == NULL) {
		bdbm_error ("bdbm_malloc_atomic failed");
		goto fail;
	}

        if ((p->oob_data = (uint64_t*)bdbm_malloc
                                (sizeof (uint64_t) * params->nr_subpages_per_ssd)) == NULL) {
                bdbm_error ("bdbm_malloc failed(oob_data)");
                goto fail;
        }

        /* initialize some variables */
        bdbm_spin_lock_init (&p->dm_lock);
        p->w_cnt = 0;
        p->w_cnt_done = 0;

        /* OK! keep private info */
        bdi->ptr_dm_inf->ptr_private = (void*)p;

        return 0;

fail:
        return -1;
}

uint32_t dm_user_open (bdbm_drv_info_t* bdi)
{
        struct dm_user_private * p;

        p = (struct dm_user_private*)bdi->ptr_dm_inf->ptr_private;


        bdbm_msg ("dm_user_open is initialized");

        return 0;
}

void dm_user_close (bdbm_drv_info_t* bdi)
{
        struct dm_user_private* p; 

        p = (struct dm_user_private*)bdi->ptr_dm_inf->ptr_private;

        bdbm_msg ("dm_user: w_cnt = %llu, w_cnt_done = %llu", p->w_cnt, p->w_cnt_done);
        bdbm_msg ("dm_user_close is destroyed");

        bdbm_free_atomic (p);
}

uint32_t dm_user_make_req (bdbm_drv_info_t* bdi, bdbm_llm_req_t* ptr_llm_req)
{
    int i;
    struct dm_user_private* p; 
    bdbm_device_params_t dp = bdi->parm_dev;
    bdbm_llm_req_t* r = ptr_llm_req;
    p = (struct dm_user_private*)bdi->ptr_dm_inf->ptr_private;
    uint64_t idx = (r->phyaddr.channel_no * dp.nr_blocks_per_channel * dp.nr_subpages_per_block) + 
        (r->phyaddr.chip_no * dp.nr_blocks_per_chip * dp.nr_subpages_per_block) + 
        (r->phyaddr.block_no * dp.nr_subpages_per_block) + r->phyaddr.page_no*dp.nr_subpages_per_page;
	uint32_t ofs = r->subpage_ofs;


    /*TODO: do somthing */
    bdbm_spin_lock (&p->dm_lock);
    p->w_cnt++;

    if(bdbm_is_write(r->req_type)){
          //  printf("DUMMY_DEVICE: \n");
		  //printf("Write req OOB: \n");

		  for(i = 0; i < r->nr_valid ; i++) {
			  if(r->logaddr.lpa[i] == -1) continue;
			  bdbm_bug_on (r->fmain.kp_stt[i] != KP_STT_DATA);
			  p->oob_data[idx + ofs + i] = r->logaddr.lpa[i];
			 // bdbm_msg("dm offset %lld, lpa %lld", idx+ofs+i, r->logaddr.lpa[i]);
			/*  printf("logaddr=%d :ch=%d, chip=%d, blk=%d, page=%d, punit=%d, off=%d, fmain[%d]=%p\n",
					  r->logaddr.lpa[i],
					  r->phyaddr.channel_no,
					  r->phyaddr.chip_no,
					  r->phyaddr.block_no,
					  r->phyaddr.page_no,
					  r->phyaddr.punit_id,
					  ofs+i,
					  i,r->fmain.kp_ptr[i]); 
			*/	
		  }

    }else if(bdbm_is_read(r->req_type)){
        for (i = 0; i < BDBM_MAX_PAGES; i++){
            if(r->fmain.kp_stt[i] == KP_STT_DATA){
			//	bdbm_msg("dm read offset %lld, lpa %lld", idx+i, p->oob_data[idx+i]);
                ((uint64_t*)r->foob.data)[i] = p->oob_data[idx + i];
				//must add offset info
				
              //  printf("DUMMY_READ: \n");
				/*
                printf("logaddr=%d :ch=%d, chip=%d, blk=%d, page=%d, punit=%d, off=%d, fmain[%d]=%p\n",
                        p->oob_data[idx + i],
                        r->phyaddr.channel_no,
                        r->phyaddr.chip_no,
                        r->phyaddr.block_no,
                        r->phyaddr.page_no,
                        r->phyaddr.punit_id,
						i,
                        i, r->fmain.kp_ptr[i]);
						
			*/
            //    printf("END_DUMMY_READ: \n");
            }
        }

    }
//	bdbm_msg("dm end");
    bdbm_spin_unlock (&p->dm_lock);

    dm_user_end_req (bdi, ptr_llm_req);

    return 0;
}

void dm_user_end_req (bdbm_drv_info_t* bdi, bdbm_llm_req_t* ptr_llm_req)
{
    struct dm_user_private* p; 

    p = (struct dm_user_private*)bdi->ptr_dm_inf->ptr_private;

    bdbm_spin_lock (&p->dm_lock);
    p->w_cnt_done++;
    bdbm_spin_unlock (&p->dm_lock);

    bdi->ptr_llm_inf->end_req (bdi, ptr_llm_req);
}

/* for snapshot */
uint32_t dm_user_load (bdbm_drv_info_t* bdi, const char* fn)
{	
    struct dm_user_private * p = 
        (struct dm_user_private*)bdi->ptr_dm_inf->ptr_private;

    bdbm_msg ("loading a DRAM snapshot...");

    return 0;
}

uint32_t dm_user_store (bdbm_drv_info_t* bdi, const char* fn)
{
    struct dm_user_private * p = 
        (struct dm_user_private*)bdi->ptr_dm_inf->ptr_private;

    bdbm_msg ("storing a DRAM snapshot...");

    return 0;
}
