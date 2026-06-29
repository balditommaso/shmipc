#ifndef IPC_DEV_H
#define IPC_DEV_H

#include <linux/mutex.h>
#include <linux/types.h>
#include "shmipc_uapi.h"

/*
 * Shared memory layout (low address -> high address):
 *
 *   +-----------------------------+  <- base
 *   |   shmipc_state_header       |
 *   |   shmipc_peer_slot[0..n-1]  |  <- PID lookup / id-allocation table
 *   |   (rest unused)             |  state_size   (read-only for all peers)
 *   +-----------------------------+
 *   |     Read/Write Section      |  rw_size      (read-write for all peers)
 *   +-----------------------------+
 *   |  Output Section for peer 0  |  out_size     (RW for peer 0, RO for rest)
 *   +-----------------------------+
 *   |  Output Section for peer 1  |  out_size
 *   +-----------------------------+
 *   :            ...              :
 *   +-----------------------------+
 *   | Output Section for peer n-1 |  out_size
 *   +-----------------------------+  <- base + total_size
 *
 * All section sizes are page-aligned so that access control can be
 * enforced at PTE granularity.
 *
 * The State Table's shmipc_peer_slot[] array is both the id-allocation
 * bitmap and the PID lookup table: slot i is occupied iff peer id i is
 * currently assigned. See shmipc_uapi.h for the release/acquire
 * concurrency contract that makes this safe to read from userspace
 * without a lock.
 */

struct ipc_dev {
	void *base;
	unsigned int total_size;
	unsigned int state_size;
	unsigned int rw_size;
	unsigned int out_size;
	unsigned int max_peers;
	struct mutex lock;
};


extern struct ipc_dev dev;


int ipc_dev_init(unsigned int max_peers);
void ipc_dev_destroy(void);


static inline unsigned int ipc_dev_rw_offset(void)
{
	return dev.state_size;
}


static inline unsigned int ipc_dev_out_offset(unsigned int id)
{
	return dev.state_size + dev.rw_size + id * dev.out_size;
}


static inline struct shmipc_state_header *ipc_dev_state_header(void)
{
	return (struct shmipc_state_header *)dev.base;
}


static inline struct shmipc_peer_slot *ipc_dev_slot(unsigned int i)
{
	return (struct shmipc_peer_slot *)((char *)dev.base + shmipc_peer_slot_offset(i));
}


int ipc_dev_alloc_id(pid_t pid);
void ipc_dev_free_id(unsigned int id);
int ipc_dev_find_by_pid(pid_t pid);


#endif