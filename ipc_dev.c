#include <linux/vmalloc.h>
#include <linux/mm.h>
#include "ipc_dev.h"


struct ipc_dev dev;


#define IPC_STATE_SIZE_RAW  4096
#define IPC_RW_SIZE_RAW     4096
#define IPC_OUT_SIZE_RAW    4096


int ipc_dev_init(unsigned int max_peers)
{
	struct shmipc_state_header *hdr;
	unsigned int slots_size;

	if (max_peers == 0)
		return -EINVAL;

	mutex_init(&dev.lock);
	dev.max_peers = max_peers;

	dev.state_size = PAGE_ALIGN(IPC_STATE_SIZE_RAW);
	if (dev.state_size == 0)
		return -EINVAL;

	dev.rw_size  = PAGE_ALIGN(IPC_RW_SIZE_RAW);
	dev.out_size = PAGE_ALIGN(IPC_OUT_SIZE_RAW);

	dev.total_size = dev.state_size + dev.rw_size + dev.max_peers * dev.out_size;

	slots_size = sizeof(struct shmipc_state_header) + max_peers * sizeof(struct shmipc_peer_slot);
	if (slots_size > dev.state_size) {
		printk("shmipc: max_peers=%u too larges\n", max_peers);
		return -EINVAL;
	}

	dev.base = vzalloc(dev.total_size);
	if (!dev.base)
		return -ENOMEM;

	hdr = ipc_dev_state_header();
	hdr->version = SHMIPC_STATE_VERSION;
	hdr->max_peers = max_peers;


	printk("shmipc: allocated %u bytes (state=%u rw=%u out=%u x%u)\n",
		   dev.total_size, dev.state_size, dev.rw_size,
		   dev.out_size, dev.max_peers);
	return 0;
}


void ipc_dev_destroy(void)
{
	if (dev.base)
		vfree(dev.base);
	dev.base = NULL;
}


/*
 * Allocate a free peer id for @pid: scan slots for either an existing
 * slot owned by @pid (-EBUSY, one slot per process) or the lowest free
 * slot to claim.
 */
int ipc_dev_alloc_id(pid_t pid)
{
	struct shmipc_peer_slot *slot;
	unsigned int i;
	int free_id = -1;
	int ret = -ENOSPC;

	mutex_lock(&dev.lock);
	printk("Searching free slot for pid %u\n", pid);
	for (i = 0; i < dev.max_peers; i++) {
		slot = ipc_dev_slot(i);

		if (smp_load_acquire(&slot->in_use)) {
			if ((pid_t)slot->pid == pid) {
				ret = -EBUSY;
				printk("Slot already allocated to pid %u\n", pid);
				goto out_unlock;
			}
			continue;
		}

		if (free_id < 0) 
			free_id = i;
			
	}

	if (free_id < 0) {
		printk("No available slot found.");
		goto out_unlock;
	}
		
	slot = ipc_dev_slot(free_id);
	slot->pid = pid;
	slot->generation++;
	smp_store_release(&slot->in_use, 1);

	ret = free_id;

out_unlock:
	mutex_unlock(&dev.lock);
	return ret;
}


void ipc_dev_free_id(unsigned int id)
{
	struct shmipc_peer_slot *slot;

	if (id >= dev.max_peers)
		return;

	mutex_lock(&dev.lock);
	slot = ipc_dev_slot(id);
	smp_store_release(&slot->in_use, 0);
	mutex_unlock(&dev.lock);
}