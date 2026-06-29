/*
 * shmipc ping-pong RTT benchmark.
 *
 * Exercises the REAL per-peer access control: pinger and ponger each 
 * write only into their own output section (RW for the owner) and read 
 * only from the peer's output section (RO for everyone else). Discovery 
 * of "where is the peer's output section" is done via PID lookup against 
 * the State Table 
 *
 * Usage:
 *   ./pingpong_bench [rounds] [warmup]
 *     rounds  - number of timed ping-pong rounds (default 100000)
 *     warmup  - number of untimed warmup rounds discarded first
 *               (default 1000)

 *
 * Discovery (both sides, after their own open()+mmap()):
 *   Spins on shmipc_find_peer_by_pid() until the peer's slot appears in the
 *   State Table. This doubles as the readiness handshake: "found in
 *   the table" already implies the peer has called open() (which is
 *   what populates its slot). Because the pinger opens+mmaps before
 *   forking, its slot exists before the ponger even starts looking;
 *   the ponger's own open()+mmap() happens before IT calls
 *   discover_peer() for the pinger, so by the time the pinger's
 *   lookup of the ponger succeeds, the ponger has necessarily
 *   already mmap()'d. No separate ready-flag is needed anywhere.
 *
 *
 * Protocol per round n (n starts at 1):
 *   pinger: t0 = now(); write OWN output: {seq=n, ts_ns=t0}
 *   ponger: spin-read PINGER's output until seq == n;
 *           write OWN output: {seq=n}
 *   pinger: spin-read PONGER's output until seq == n;
 *           rtt = now() - t0
 *
 * A sentinel seq value (UINT64_MAX) written into the pinger's own
 * output section tells the ponger to exit.
 *
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdatomic.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <time.h>
#include <errno.h>
#include "../shmipc_uapi.h"
#include "../shmipc_lookup.h"

#define DEVICE_PATH 	"/dev/shmipc"
#define SENTINEL_STOP 	UINT64_MAX
#define DEFAULT_ROUNDS 	100000
#define DEFAULT_WARMUP 	1000


struct pingpong_payload {
	_Atomic uint64_t seq;
	uint64_t ts_ns; 
};

/*
 * Benchmarking
*/
static inline uint64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int compare_u64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
	return (x > y) - (x < y);
}

static uint64_t percentile(uint64_t *sorted, size_t n, double p)
{
	double idx = p * (double)(n - 1);
	size_t lo = (size_t)idx;
	size_t hi = lo + 1 < n ? lo + 1 : lo;
	double frac = idx - (double)lo;
	return (uint64_t)((double)sorted[lo] * (1.0 - frac) + (double)sorted[hi] * frac);
}


/*
 * Device utilities
*/
static void *map_device(struct shmipc_layout *layout_out)
{
	int fd;
	void *base;

	fd = open(DEVICE_PATH, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "open(%s): %s\n", DEVICE_PATH, strerror(errno));
		exit(1);
	}

	if (ioctl(fd, IPC_GET_LAYOUT, layout_out) < 0) {
		fprintf(stderr, "ioctl(IPC_GET_LAYOUT): %s\n", strerror(errno));
		exit(1);
	}

	base = mmap(NULL, layout_out->total_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (base == MAP_FAILED) {
		fprintf(stderr, "mmap: %s\n", strerror(errno));
		exit(1);
	}

	close(fd);
	return base;
}


static int discover_peer(const void *base, 
	                     const struct shmipc_layout *layout,
			  			 pid_t target_pid)
{
	for (;;) {
		struct shmipc_lookup_result r = shmipc_find_peer_by_pid(base, 
																layout, 
																(unsigned int)target_pid);
		if (r.id >= 0) {
			printf("PID %d dicovered at slot %d\n", target_pid, r.id);
			return r.id;
		}
		
	}
}


static struct pingpong_payload *peer_output(const void *base, 
												  const struct shmipc_layout *layout, 
												  int peer_id)
{
	return (struct pingpong_payload *)shmipc_output_addr((void *)base, 
														 layout, 
														 (unsigned int)peer_id);
}


static int run_ponger(pid_t pinger_pid)
{
	struct shmipc_layout layout;
	void *base = map_device(&layout);

	if (sizeof(struct pingpong_payload) > layout.out_size) {
		fprintf(stderr, "ponger: output section too small (%u/%zu bytes ratio)\n",
				layout.out_size, 
				sizeof(struct pingpong_payload));
		return 1;
	}

	int pinger_id = discover_peer(base, &layout, pinger_pid);
	struct pingpong_payload *pinger_out = peer_output(base, &layout, pinger_id);
	struct pingpong_payload *my_out = peer_output(base, &layout, layout.peer_id);

	uint64_t last_seq = 0;

	for (;;) {
		uint64_t seq;

		for (;;) {
			seq = atomic_load_explicit(&pinger_out->seq, memory_order_acquire);
			if (seq != last_seq) {
				printf("Ponger is reading seq %ld\n", seq);
				break;
			}
				
		}

		if (seq == SENTINEL_STOP) {
			printf("Ponger received STOP signal");
			break;
		}

		last_seq = seq;
		atomic_store_explicit(&my_out->seq, seq, memory_order_release);
		printf("Ponger is writing back seq %ld\n", seq);
	}

	munmap(base, layout.total_size);
	return 0;
}


static int run_pinger(void *base, 
					  const struct shmipc_layout *layout,
		              pid_t ponger_pid, 
					  long rounds, 
					  long warmup)
{
	long total_rounds = rounds + warmup;
	uint64_t *samples;

	if (sizeof(struct pingpong_payload) > layout->out_size) {
		fprintf(stderr, "pinger: output section too small");
		return 1;
	}

	samples = malloc((size_t)rounds * sizeof(uint64_t));
	if (!samples) {
		fprintf(stderr, "malloc failed\n");
		return 1;
	}

	int ponger_id = discover_peer(base, layout, ponger_pid);
	struct pingpong_payload *ponger_out = peer_output(base, layout, ponger_id);
	struct pingpong_payload *my_out = peer_output(base, layout, layout->peer_id);

	printf("pinger: ponger ready (id=%d), own peer_id=%u\n",
		   ponger_id, 
		   layout->peer_id);
	printf("pinger: running %ld warmup + %ld timed rounds...\n", warmup, rounds);

	for (long n = 1; n <= total_rounds; n++) {
		uint64_t seq = (uint64_t)n;
		uint64_t t0 = now_ns();

		my_out->ts_ns = t0;
		atomic_store_explicit(&my_out->seq, seq, memory_order_release);
		printf("Pinger is writing seq %ld\n", seq);

		for (;;) {
			uint64_t p = atomic_load_explicit(&ponger_out->seq, memory_order_acquire);
			if (p == seq) {
				printf("Pinger is reading back seq %ld\n", p);
				break;
			}
		}

		uint64_t t1 = now_ns();

		if (n > warmup)
			samples[n - warmup - 1] = t1 - t0;
	}

	atomic_store_explicit(&my_out->seq, SENTINEL_STOP, memory_order_release);
	printf("Pinger sent STOP sequence\n");

	int status;
	waitpid(ponger_pid, &status, 0);

	qsort(samples, (size_t)rounds, sizeof(uint64_t), compare_u64);

	uint64_t sum = 0;
	for (long i = 0; i < rounds; i++)
		sum += samples[i];
	double mean = (double)sum / (double)rounds;

	printf("\n=== shmipc ping-pong RTT (rounds=%ld, warmup=%ld) ===\n",
	       rounds, warmup);
	printf("min:    %8.0f ns\n", (double)samples[0]);
	printf("p50:    %8.0f ns\n", (double)percentile(samples, (size_t)rounds, 0.50));
	printf("mean:   %8.0f ns\n", mean);
	printf("p90:    %8.0f ns\n", (double)percentile(samples, (size_t)rounds, 0.90));
	printf("p99:    %8.0f ns\n", (double)percentile(samples, (size_t)rounds, 0.99));
	printf("p99.9:  %8.0f ns\n", (double)percentile(samples, (size_t)rounds, 0.999));
	printf("max:    %8.0f ns\n", (double)samples[rounds - 1]);
	printf("rounds/sec (1/mean): %.0f\n", 1e9 / mean);

	free(samples);
	return 0;
}



int main(int argc, char **argv)
{
    long rounds = DEFAULT_ROUNDS;
    long warmup = DEFAULT_WARMUP;

    if (argc >= 2)
        rounds = atol(argv[1]);

    if (argc >= 3)
        warmup = atol(argv[2]);

    struct shmipc_layout layout;
    void *base = map_device(&layout);

    pid_t child = fork();

    if (child < 0) {
        perror("fork");
        return 1;
    }

    if (child == 0) {
        pid_t parent_pid = getppid();
        return run_ponger(parent_pid);
    }

    int rc = run_pinger(base, &layout, child, rounds, warmup);

    munmap(base, layout.total_size);

    return rc;
}