#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <time.h>


#define DEVICE_PATH 	"/dev/mmap-test"
#define SENTINEL_STOP 	UINT64_MAX
#define DEFAULT_ROUNDS 	100000
#define DEFAULT_WARMUP 	1000
#define MMAP_SIZE       512


struct pingpong_payload {
    _Atomic uint64_t seq;
    _Atomic bool is_read;
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
static void *map_device(unsigned int size)
{
    int fd;
    void *base;

    fd = open(DEVICE_PATH, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "open(%s): %s\n", DEVICE_PATH, strerror(errno));
		exit(1);
    }

    base = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) {
        fprintf(stderr, "mmap: %s\n", strerror(errno));
		exit(1);
    }
    close(fd);

    return base;
}


static int run_pinger(void *base,
                      long rounds,
                      long warmup)
{
    struct pingpong_payload *shared = base;
    uint64_t *samples;

    samples = malloc((size_t)rounds * sizeof(uint64_t));
	if (!samples) {
		fprintf(stderr, "malloc failed\n");
		return 1;
	}

    if (sizeof(struct pingpong_payload) > MMAP_SIZE) {
        fprintf(stderr, "pinger: output section too small\n");
        return 1;
    }

    long total = rounds + warmup;
    printf("pinger: running %ld warmup + %ld timed rounds...\n", warmup, rounds);
    for (long i = 1; i <= total; i++) {

        uint64_t t0 = now_ns();

        shared->ts_ns = t0;

        atomic_store_explicit(&shared->is_read, false, memory_order_relaxed);
        atomic_store_explicit(&shared->seq, i, memory_order_release);
        printf("Pinger is writing seq %ld\n", i);

        while (!atomic_load_explicit(&shared->is_read, memory_order_acquire));

        uint64_t t1 = now_ns();

        if (i > warmup)
			samples[i - warmup - 1] = t1 - t0;
    }

    atomic_store(&shared->seq, SENTINEL_STOP);
    printf("Pinger sent STOP sequence\n");

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


static int run_ponger(void)
{
    void *base = map_device(MMAP_SIZE);

    if (sizeof(struct pingpong_payload) > MMAP_SIZE) {
        fprintf(stderr, "ponger: output section too small\n");
        return 1;
    }

    struct pingpong_payload *shared = base;

    uint64_t last = 0;

    for (;;) {

        uint64_t seq = atomic_load_explicit(&shared->seq, memory_order_acquire);
        

        if (seq == SENTINEL_STOP) {
            printf("Ponger received STOP signal");
            break;
        }

        if (seq == last)
            continue;

        printf("Ponger is reading seq %ld\n", seq);
        last = seq;

        atomic_store_explicit(&shared->is_read, true, memory_order_release);
    }

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

    void *base = map_device(MMAP_SIZE);

    pid_t child = fork();

    if (child < 0) {
        perror("fork");
        return 1;
    }

    if (child == 0) {
        return run_ponger();
    }

    int rc = run_pinger(base, rounds, warmup);

    return rc;
}