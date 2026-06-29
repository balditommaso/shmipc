#ifndef SHMIPC_UAPI_H
#define SHMIPC_UAPI_H

/*
 * User-facing ABI for /dev/shmipc.
*/

#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/ioctl.h>
#else
#include <stdint.h>
#include <sys/ioctl.h>
#endif


struct shmipc_layout {
	unsigned int peer_id;
	unsigned int max_peers;
	unsigned int state_size;
	unsigned int rw_size;
	unsigned int out_size;
	unsigned int total_size;
};

/* Retrieve the shared memory layout and this peer's assigned ID. */
#define IPC_GET_LAYOUT _IOR('s', 1, struct shmipc_layout)


struct shmipc_peer_slot {
	unsigned int pid;        /* owning PID; valid only while in_use == 1 */
	unsigned int generation; /* bumped every time this slot is (re)allocated */
	unsigned int in_use;     /* 0 = free, 1 = occupied; release/acquire flag */
};


struct shmipc_state_header {
	unsigned int version;
	unsigned int max_peers;
};


#define SHMIPC_STATE_VERSION 1


static inline unsigned int shmipc_peer_slot_offset(unsigned int i)
{
	return sizeof(struct shmipc_state_header) + i * sizeof(struct shmipc_peer_slot);
}

#endif