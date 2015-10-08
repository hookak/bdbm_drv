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

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/slab.h>

#include "params.h"
#include "platform.h"
#include "debug.h"
#include "bdbm_drv.h"

/* 
 * setup parameters according to user configurations 
 */
#if !defined (USE_BLOCKIO_PROXY)
int _param_kernel_sector_size		= KERNEL_SECTOR_SIZE;	/* 512 Bytes */
int _param_gc_policy 				= GC_POLICY_GREEDY;
int _param_wl_policy 				= WL_POLICY_NONE;
int _param_queuing_policy			= QUEUE_POLICY_NO;
int _param_trim						= TRIM_ENABLE;
int _param_host_type				= HOST_BLOCK;
int _param_llm_type					= LLM_MULTI_QUEUE;
int _param_snapshot					= SNAPSHOT_DISABLE;
#if defined (USE_RISA)
int _param_mapping_policy 			= MAPPING_POLICY_SEGMENT;
int _param_hlm_type					= HLM_RSD;
#elif defined (USE_DFTL)
int _param_mapping_policy 			= MAPPING_POLICY_DFTL;
int _param_hlm_type					= HLM_DFTL;
#else
int _param_mapping_policy 			= MAPPING_POLICY_PAGE;
int _param_hlm_type					= HLM_NO_BUFFER;
#endif

#else /* USE_BLOCKIO_PROXY */
int _param_kernel_sector_size		= KERNEL_SECTOR_SIZE;	/* 512 Bytes */
int _param_host_type				= HOST_PROXY;
int _param_trim						= TRIM_ENABLE;

int _param_gc_policy 				= GC_POLICY_NOT_SPECIFIED;
int _param_wl_policy 				= WL_POLICY_NOT_SPECIFIED;
int _param_queuing_policy			= QUEUE_POLICY_NOT_SPECIFIED;
int _param_llm_type					= LLM_NOT_SPECIFIED;
int _param_mapping_policy 			= MAPPING_POLICY_NOT_SPECIFIED;
int _param_hlm_type					= HLM_NOT_SPECIFIED;
int _param_snapshot					= SNAPSHOT_DISABLE;

#endif


/* for kernel modules (nothing for user-level applications */
module_param (_param_mapping_policy, int, 0000);
module_param (_param_gc_policy, int, 0000);
module_param (_param_wl_policy, int, 0000);
module_param (_param_queuing_policy, int, 0000);
module_param (_param_kernel_sector_size, int, 0000);
module_param (_param_trim, int, 0000);
module_param (_param_host_type, int, 0000);
module_param (_param_llm_type, int, 0000);
module_param (_param_hlm_type, int, 0000);
module_param (_param_snapshot, int, 0000);

MODULE_PARM_DESC (_param_mapping_policy, "mapping policy");
MODULE_PARM_DESC (_param_gc_policy, "garbage collection policy");
MODULE_PARM_DESC (_param_wl_policy, "wear-leveling policy");
MODULE_PARM_DESC (_param_queuing_policy, "queueing policy");
MODULE_PARM_DESC (_param_kernel_sector_size, "kernel sector size");
MODULE_PARM_DESC (_param_trim, "trim option");
MODULE_PARM_DESC (_param_host_type, "host interface type");
MODULE_PARM_DESC (_param_llm_type, "low-level memory management type");
MODULE_PARM_DESC (_param_hlm_type, "high-level memory management type");
MODULE_PARM_DESC (_param_snapshot, "snapshot (0: disable (default), 1: enable)");

bdbm_params_t* read_driver_params (void)
{
	bdbm_params_t* p = NULL;

	/* allocate the memory for parameters */
	if ((p = (bdbm_params_t*)bdbm_malloc (sizeof (bdbm_params_t))) == NULL) {
		bdbm_error ("failed to allocate the memory for params");
		return NULL;
	}

	/* setup driver parameters */
	p->driver.mapping_policy = _param_mapping_policy;
	p->driver.gc_policy = _param_gc_policy;
	p->driver.wl_policy = _param_wl_policy;
	p->driver.kernel_sector_size = _param_kernel_sector_size;
	p->driver.trim = _param_trim;
	p->driver.host_type = _param_host_type; 
	p->driver.llm_type = _param_llm_type;
	p->driver.hlm_type = _param_hlm_type;
	p->driver.mapping_type = _param_mapping_policy;
	p->driver.snapshot = _param_snapshot;

	return p;
}

void display_default_params (bdbm_drv_info_t* bdi)
{
	bdbm_params_t* p = bdi->ptr_bdbm_params;

	if (p == NULL) {
		bdbm_msg ("oops! the parameters are not loaded properly");
		return;
	} 

	bdbm_msg ("=====================================================================");
	bdbm_msg ("DRIVER CONFIGURATION");
	bdbm_msg ("=====================================================================");
	bdbm_msg ("mapping policy = %d (0: no ftl, 1: block-mapping, 2: page-mapping)", p->driver.mapping_policy);
	bdbm_msg ("gc policy = %d (1: merge 2: random, 3: greedy, 4: cost-benefit)", p->driver.gc_policy);
	bdbm_msg ("wl policy = %d (1: none, 2: swap)", p->driver.wl_policy);
	bdbm_msg ("trim mode = %d (1: enable, 2: disable)", p->driver.trim);
	bdbm_msg ("host type = %d (1: block I/O, 2: direct)", p->driver.host_type);
	bdbm_msg ("kernel sector = %d bytes", p->driver.kernel_sector_size);
	bdbm_msg ("");

	bdbm_msg ("=====================================================================");
	bdbm_msg ("DEVICE PARAMETERS");
	bdbm_msg ("=====================================================================");
	bdbm_msg ("# of channels = %llu", p->device.nr_channels);
	bdbm_msg ("# of chips per channel = %llu", p->device.nr_chips_per_channel);
	bdbm_msg ("# of blocks per chip = %llu", p->device.nr_blocks_per_chip);
	bdbm_msg ("# of pages per block = %llu", p->device.nr_pages_per_block);
	bdbm_msg ("page main size  = %llu bytes", p->device.page_main_size);
	bdbm_msg ("page oob size = %llu bytes", p->device.page_oob_size);
	bdbm_msg ("SSD type = %u (0: ramdrive, 1: ramdrive with timing , 2: BlueDBM(emul), 3: BlueDBM)", p->device.device_type);
	bdbm_msg ("");
}

