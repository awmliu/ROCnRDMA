/*
 * Copyright 2016 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */


#include <linux/version.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/scatterlist.h>

#include <linux/types.h>
#include <linux/delay.h>
#include <linux/compiler.h>
#include <linux/fs.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/init.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/io.h>
#include <linux/uaccess.h>
#include <linux/sched.h>



#include <rdma/peer_mem.h>
#include "amd_rdma.h"

#define AMD_PEER_BRIDGE_DRIVER_VERSION	"1.0"
#define AMD_PEER_BRIDGE_DRIVER_NAME	"amdp2p"


MODULE_AUTHOR("serguei.sagalovitch@amd.com");
MODULE_LICENSE("GPL and additional rights");
MODULE_DESCRIPTION("AMD P2P Bridge Driver for PeerDirect interface");
MODULE_VERSION(AMD_PEER_BRIDGE_DRIVER_VERSION);


#define MSG_DBG(fmt, args ...)	\
			 pr_debug(fmt, ## args)
#define MSG_INFO(fmt, args ...)	\
			 pr_info(AMD_PEER_BRIDGE_DRIVER_NAME ": " fmt, ## args)
#define MSG_ERR(fmt, args ...)	\
			pr_err(AMD_PEER_BRIDGE_DRIVER_NAME ": " fmt, ## args)
#define MSG_WARN(fmt, args ...)	\
			pr_warn(AMD_PEER_BRIDGE_DRIVER_NAME ": " fmt, ## args)


static const struct amd_rdma_interface *rdma_interface;

static invalidate_peer_memory ib_invalidate_callback;
static void *ib_reg_handle;


struct amd_mem_context {
	uint64_t	va;
	uint64_t	size;
	struct pid	*pid;

	struct amd_p2p_info  *p2p_info;

	/* Flag that free callback was called */
	int free_callback_called;

	/* Context received from PeerDirect call */
	void *core_context;
};


static void free_callback(void *client_priv)
{
	struct amd_mem_context *mem_context =
		(struct amd_mem_context *)client_priv;

	MSG_DBG("free_callback: data 0x%p\n", mem_context);

	if (!mem_context) {
		MSG_WARN("free_callback: Invalid client context");
		return;
	}

	MSG_DBG("mem_context->core_context 0x%p\n", mem_context->core_context);

	/* Call back IB stack asking to invalidate memory */
	(*ib_invalidate_callback) (ib_reg_handle, mem_context->core_context);

	/* amdkfd will freed resources when we returned from this callback.
	 * Set flag to inform that there is nothing to do on "put_pages", etc.
	 */
	ACCESS_ONCE(mem_context->free_callback_called) = 1;
}


static int amd_acquire(unsigned long addr, size_t size,
			void *peer_mem_private_data,
			char *peer_mem_name, void **client_context)
{
	int ret;
	struct amd_mem_context *mem_context;
	struct pid *pid;

	/* Get pointer to structure describing current process */
	pid = get_task_pid(current, PIDTYPE_PID);

	MSG_DBG("acquire: addr:0x%lx,size:0x%x, pid 0x%p\n",
					addr, (unsigned int)size, pid);

	/* Check if it is address handled by AMD GPU driver */
	ret = rdma_interface->is_gpu_address(addr, pid);

	if (!ret) {
		MSG_DBG("acquire: Not GPU Address\n");
		/* This is not GPU address */
		return 0;
	}

	MSG_DBG("acquire: GPU address\n");

	/* Initialize context used for operation with given address */
	mem_context = kzalloc(sizeof(struct amd_mem_context), GFP_KERNEL);

	if (!mem_context) {
		MSG_ERR("failure to allocate memory for mem_context\n");
		/* Error case handled as not GPU address  */
		return 0;
	}

	mem_context->free_callback_called = 0;
	mem_context->va   = addr;
	mem_context->size = size;

	/* Save PI. It is guaranteed that such function will be
	 * called in the correct process context as opposite to others.
	 */
	mem_context->pid  = pid;

	MSG_DBG("acquire: Client context %p\n", mem_context);

	/* Return pointer to allocated context */
	*client_context = mem_context;

	/* Increase counter to prevent module unloading */
	__module_get(THIS_MODULE);

	/* Return 1 to inform that it is address which will be handled
	 * by AMD GPU driver
	 */
	return 1;
}

static int amd_get_pages(unsigned long addr, size_t size, int write, int force,
			  struct sg_table *sg_head,
			  void *client_context, void *core_context)
{
	int ret;
	struct amd_mem_context *mem_context =
		(struct amd_mem_context *)client_context;

	MSG_DBG("get_pages: addr:0x%lx,size:0x%x, core_context:%p\n",
						addr, (unsigned int)size, core_context);

	if (!mem_context) {
		MSG_WARN("get_pages: Invalid client context");
		return -EINVAL;
	}

	MSG_DBG("get_pages: pid :0x%p\n", mem_context->pid);


	if (addr != mem_context->va) {
		MSG_WARN("get_pages: Context address (0x%llx) is not the same",
			mem_context->va);
		return -EINVAL;
	}

	if (size != mem_context->size) {
		MSG_WARN("get_pages: Context size (0x%llx) is not the same",
			mem_context->size);
		return -EINVAL;
	}

	ret = rdma_interface->get_pages(addr,
					size,
					mem_context->pid,
					&mem_context->p2p_info,
					free_callback,
					mem_context);

	if (ret || !mem_context->p2p_info) {
		MSG_ERR("Could not rdma::get_pages failure: %d", ret);
		return ret;
	}

	mem_context->core_context = core_context;

	/* Note: At this stage it is OK not to fill sg_table */
	return 0;
}


static int amd_dma_map(struct sg_table *sg_head, void *client_context,
			struct device *dma_device, int dmasync, int *nmap)
{
	/*
	 * NOTE/TODO:
	 * We could have potentially three cases for real memory
	 *	location:
	 *		- all memory in the local
	 *		- all memory in the system
	 *		- memory is spread (s/g) between local and system.
	 *
	 *	In the case of all memory in the system we could use
	 *	iommu driver to build DMA addresses but not in the case
	 *	of local memory because currently iommu driver doesn't
	 *	deal with local/device memory addresses (it requires "struct
	 *	page").
	 *
	 *	Accordingly there return is assumption that iommu funcutionality
	 *	should be disabled so we could assume that sg_table already
	 *	contains DMA addresses.
	 *
	 */
	struct amd_mem_context *mem_context =
		(struct amd_mem_context *)client_context;

	MSG_DBG("dma_map: Context 0x%p, sg_head 0x%p\n",
			client_context, sg_head);

	MSG_DBG("dma_map: pid 0x%p, address 0x%llx, size:0x%llx\n",
			mem_context->pid,
			mem_context->va,
			mem_context->size);

	if (!mem_context->p2p_info) {
		MSG_ERR("dma_map: No sg table were allocated\n");
		return -EINVAL;
	}

	/* Copy information about previosly allocate sg_table */
	*sg_head = *mem_context->p2p_info->pages;

	/* Return number of pages */
	*nmap = mem_context->p2p_info->pages->nents;

	return 0;
}

static int amd_dma_unmap(struct sg_table *sg_head, void *client_context,
			   struct device  *dma_device)
{
	struct amd_mem_context *mem_context =
		(struct amd_mem_context *)client_context;

	MSG_DBG("dma_unmap: Context 0x%p, sg_table 0x%p\n",
			client_context, sg_head);

	MSG_DBG("dma_unmap: pid 0x%p, address 0x%llx, size:0x%llx\n",
			mem_context->pid,
			mem_context->va,
			mem_context->size);

	/* Assume success */
	return 0;
}
static void amd_put_pages(struct sg_table *sg_head, void *client_context)
{
	int ret = 0;
	struct amd_mem_context *mem_context =
		(struct amd_mem_context *)client_context;

	MSG_DBG("put_pages: sg_head %p client_context: 0x%p\n",
			sg_head, client_context);
	MSG_DBG("put_pages: pid 0x%p, address 0x%llx, size:0x%llx\n",
			mem_context->pid,
			mem_context->va,
			mem_context->size);

	MSG_DBG("put_pages: mem_context->p2p_info %p\n",
				mem_context->p2p_info);

	if (ACCESS_ONCE(mem_context->free_callback_called)) {
		MSG_DBG("put_pages: free callback was called\n");
		return;
	}

	if (mem_context->p2p_info) {
		ret = rdma_interface->put_pages(&mem_context->p2p_info);
		mem_context->p2p_info = NULL;

		if (ret)
			MSG_ERR("put_pages failure: %d (callback status %d)",
					ret, mem_context->free_callback_called);
	} else
		MSG_ERR("put_pages: Pointer to p2p info is null\n");
}
static unsigned long amd_get_page_size(void *client_context)
{
	unsigned long page_size;
	int result;
	struct amd_mem_context *mem_context =
		(struct amd_mem_context *)client_context;

	MSG_DBG("get_page_size: context: %p\n", client_context);
	MSG_DBG("get_page_size: pid 0x%p, address 0x%llx, size:0x%llx\n",
			mem_context->pid,
			mem_context->va,
			mem_context->size);


	result = rdma_interface->get_page_size(
				mem_context->va,
				mem_context->size,
				mem_context->pid,
				&page_size);

	if (result) {
		MSG_ERR("Could not get page size. %d", result);
		/* If we failed to get page size then do not know what to do.
		 * Let's return some default value
		 */
		return 4096;
	}

	return page_size;
}

static void amd_release(void *client_context)
{
	struct amd_mem_context *mem_context =
		(struct amd_mem_context *)client_context;

	MSG_DBG("release: context: 0x%p\n", client_context);
	MSG_DBG("release: pid 0x%p, address 0x%llx, size:0x%llx\n",
			mem_context->pid,
			mem_context->va,
			mem_context->size);

	kfree(mem_context);

	/* Decrease counter to allow module unloading */
	module_put(THIS_MODULE);
}


static struct peer_memory_client amd_mem_client = {
	.acquire = amd_acquire,
	.get_pages = amd_get_pages,
	.dma_map = amd_dma_map,
	.dma_unmap = amd_dma_unmap,
	.put_pages = amd_put_pages,
	.get_page_size = amd_get_page_size,
	.release = amd_release,
};


static int __init amd_peer_bridge_init(void)
{
	int result;

	MSG_INFO("init\n");


	result = amdkfd_query_rdma_interface(&rdma_interface);

	if (result < 0) {
		MSG_ERR("Can not get RDMA Interface (result = %d)\n", result);
		return result;
	}

	strcpy(amd_mem_client.name, AMD_PEER_BRIDGE_DRIVER_NAME);
	strcpy(amd_mem_client.version, AMD_PEER_BRIDGE_DRIVER_VERSION);
	ib_reg_handle = ib_register_peer_memory_client(&amd_mem_client,
						&ib_invalidate_callback);

	if (!ib_reg_handle) {
		MSG_ERR("Can not register peer memory client");
		return -EINVAL;
	}

	return 0;
}


/* Note: cleanup_module is never called if registering failed */
static void __exit amd_peer_bridge_cleanup(void)
{
	MSG_INFO("cleanup\n");

	ib_unregister_peer_memory_client(ib_reg_handle);
}


module_init(amd_peer_bridge_init);
module_exit(amd_peer_bridge_cleanup);

