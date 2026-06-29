/*
 * Userspace reference implementation for looking up a peer's output
 * section by PID.
 *
 * Usage pattern from an actual peer process:
 *
 *   int fd = open("/dev/shmipc", O_RDWR);
 *   struct shmipc_layout layout;
 *   ioctl(fd, IPC_GET_LAYOUT, &layout);
 *   void *base = mmap(NULL, layout.total_size, PROT_READ | PROT_WRITE,
 *                      MAP_SHARED, fd, 0);
 *
 *   int id = shmipc_find_peer_by_pid(base, &layout, target_pid);
 *   if (id >= 0) {
 *       void *peer_output = shmipc_output_addr(base, &layout, id);
 *       // peer_output is read-only unless id == layout.peer_id
 *   }
 *
 * The lookup walks the State Table directly -- no ioctl round-trip --
 * using the same release/acquire protocol the kernel publishes with.
 * See the concurrency contract documented in shmipc_uapi.h.
 */
#include <stdatomic.h>
#include <stddef.h>
#include "shmipc_uapi.h"

static inline unsigned int shmipc_output_offset(const struct shmipc_layout *layout, unsigned int id)
{
	return layout->state_size + layout->rw_size + id * layout->out_size;
}

static inline void *shmipc_output_addr(void *base,
									   const struct shmipc_layout *layout,
									   unsigned int id)
{
	return (char *)base + shmipc_output_offset(layout, id);
}

static inline const struct shmipc_peer_slot *shmipc_slot_addr(const void *base, unsigned int i)
{
	return (const struct shmipc_peer_slot *)((const char *)base + shmipc_peer_slot_offset(i));
}


struct shmipc_lookup_result {
	int id;            
	unsigned int generation; 
};


static struct shmipc_lookup_result shmipc_find_peer_by_pid(const void *base, 
														   const struct shmipc_layout *layout,
			 											   unsigned int target_pid)
{
	struct shmipc_lookup_result result = { .id = -1, .generation = 0 };

	for (unsigned int i = 0; i < layout->max_peers; i++) {
		const struct shmipc_peer_slot *slot = shmipc_slot_addr(base, i);

		uint32_t in_use = atomic_load_explicit((const atomic_uint *)&slot->in_use, 
											   memory_order_acquire);
		if (!in_use)
			continue;

		uint32_t pid = atomic_load_explicit((const atomic_uint *)&slot->pid, 
											memory_order_relaxed);

		if (pid == target_pid) {
			result.id = (int)i;
			result.generation = atomic_load_explicit((const atomic_uint *)&slot->generation,
													 memory_order_relaxed);
			return result;
		}
	}

	return result;
}