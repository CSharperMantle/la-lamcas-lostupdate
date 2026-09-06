/* Minimal reproducer for the Loongson LA664 (3B6000) AMCAS lost-update erratum.
 *
 * Workers run a CAS "ticket" loop on one shared cell:
 *
 *     expect = current cell value
 *     rd = amcas.d(cell, expect, expect + 1) // write expect+1 iff ==expect
 *     if (rd == expect) successes++; else expect = rd;
 *
 * Invariant under the architecture:  final_cell == sum(successes).
 *
 * Between 64-op bursts, each worker issues plain byte loads to a cache line neighboring the cell.
 * That interleave alone breaks amcas.d's RMW: counted successes whose writes never commit.
 * amcas_db.d, amadd.d and ll/sc stay exact under the same schedule.
 */

#define _GNU_SOURCE

#include <assert.h>
#include <larchintrin.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define BURSTS 15625 /* 15625 * 64 = ~1M ops / worker */
#define PER_BURST 64
#define N_WORKERS 3
#define CACHE_LINE_SZ 64
#define BOUNCE_SIZE (CACHE_LINE_SZ * 1)
#define BOUNCE_BURSTS 32

/* ------------------------------------------------------------------- */
/* C workers -- only the atomic ops (amcas/amcas_db/amswap/ll/sc) stay */
/* as inline asm, embedded directly inside each routine                */
/* ------------------------------------------------------------------- */

// CAS fetch-and-increment; returns the number of successful swaps.
static uint64_t ticket_burst_amcas(atomic_ulong *cell, uint64_t per) {
	uint64_t expect =
	    atomic_load_explicit(cell, memory_order_relaxed); /* expect = current cell value */
	uint64_t successes = 0;

	while (per--) {
		for (;;) {
			const uint64_t new = expect + 1;
			uint64_t old = expect;

			__asm__ __volatile__("amcas.d %0, %2, %1"
					     : "+r"(old)
					     : "r"(cell), "r"(new)
					     : "memory");
			if (old == expect) { /* swap reported success */
				successes++;
				expect = new;
				break;
			}
			expect = old; /* retry with the new old value */
		}
	}

	return successes;
}

// Identical protocol on AMCAS_DB. Per spec the RMW atomicity is specified for both forms;
// _DB additionally orders surrounding accesses. Discriminates "RMW atomicity broken" from
// "non-DB execution path drops writes".
static uint64_t ticket_burst_amcasdb(atomic_ulong *cell, uint64_t per) {
	uint64_t expect =
	    atomic_load_explicit(cell, memory_order_relaxed); /* expect = current cell value */
	uint64_t successes = 0;

	while (per--) {
		for (;;) {
			const uint64_t new = expect + 1;
			uint64_t old = expect;

			__asm__ __volatile__("amcas_db.d %0, %2, %1"
					     : "+r"(old)
					     : "r"(cell), "r"(new)
					     : "memory");
			if (old == expect) { /* swap reported success */
				successes++;
				expect = new;
				break;
			}
			expect = old; /* retry with the new old value */
		}
	}

	return successes;
}

// Identical protocol on LL/SC -- the failsafe control.
static uint64_t ticket_burst_llsc(atomic_ulong *cell, uint64_t per) {
	uint64_t successes = 0;

	while (per--) {
		uint64_t v;

		do {
			__asm__ __volatile__("ll.d %0, %1, 0" : "=r"(v) : "r"(cell));
			v = v + 1;
			__asm__ __volatile__("sc.d %0, %1, 0" : "+r"(v) : "r"(cell) : "memory");
		} while (!v); /* sc failed -> retry */
		successes++;
	}

	return successes;
}

// Identical protocol on AMADD.
static uint64_t ticket_burst_amadd(atomic_ulong *cell, uint64_t per) {
	uint64_t successes = 0;

	while (per--) {
		atomic_fetch_add_explicit(cell, 1, memory_order_relaxed);
		successes++;
	}

	return successes;
}

static void bounce_sweep(uint8_t *buf) {
	__attribute__((unused)) uint64_t sink;

	volatile uint8_t *const buf_vola = buf;

	for (int i = 0; i < BOUNCE_BURSTS; i++)
		sink += buf_vola[0];
}

/* ------------------------------------------------------------------ */

static double now_sec(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void pin(int cpu) {
	cpu_set_t s;
	CPU_ZERO(&s);
	CPU_SET(cpu, &s);
	pthread_setaffinity_np(pthread_self(), sizeof s, &s);
}

/* ------------------------------------------------------------------ */

enum mech {
	MECH_AMCAS,
	MECH_AMCASDB,
	MECH_LLSC,
	MECH_AMADD,
};

struct worker_args {
	int cpu;
	enum mech mech;
	atomic_ulong *cell;
	uint8_t *bounce;
	uint64_t bursts, per, result;
	pthread_barrier_t *bar;
};

struct worker_mem {
	atomic_ulong cell;
	uint8_t unused[CACHE_LINE_SZ - sizeof(atomic_ulong)];
	uint8_t bounce[N_WORKERS][BOUNCE_SIZE];
};

static void *worker(void *raw) {
	struct worker_args *const a = raw;
	pin(a->cpu);
	pthread_barrier_wait(a->bar);
	for (uint64_t r = 0; r < a->bursts; r++) {
		switch (a->mech) {
		case MECH_AMCAS:
			a->result += ticket_burst_amcas(a->cell, a->per);
			break;
		case MECH_AMCASDB:
			a->result += ticket_burst_amcasdb(a->cell, a->per);
			break;
		case MECH_LLSC:
			a->result += ticket_burst_llsc(a->cell, a->per);
			break;
		case MECH_AMADD:
			a->result += ticket_burst_amadd(a->cell, a->per);
			break;
		}
		bounce_sweep(a->bounce);
	}
	return NULL;
}

int main(int argc, char **argv) {
	assert(argc > 0);

	setvbuf(stdout, NULL, _IONBF, 0);

	const char *const mech_s = (argc > 1) ? argv[1] : "amcas";
	const int rounds = (argc > 2) ? atoi(argv[2]) : 60;
	enum mech mech;
	if (strcmp(mech_s, "llsc") == 0)
		mech = MECH_LLSC;
	else if (strcmp(mech_s, "amcasdb") == 0)
		mech = MECH_AMCASDB;
	else if (strcmp(mech_s, "amcas") == 0)
		mech = MECH_AMCAS;
	else if (strcmp(mech_s, "amadd") == 0)
		mech = MECH_AMADD;
	else {
		fprintf(stderr, "error: mech must be amcas, amcasdb, llsc or amadd\n");
		return EXIT_FAILURE;
	}

	printf("mech=%s rounds=%d workers=%d\n", mech_s, rounds, N_WORKERS);

	uint64_t total_ops = 0, total_lost = 0;
	int bad_rounds = 0;
	const double t0 = now_sec();

	for (int r = 0; r < rounds; r++) {
		struct worker_mem *const mem = calloc(1, sizeof *mem);
		if (!mem) {
			perror("calloc");
			return EXIT_FAILURE;
		}

		atomic_ulong *const cell = &mem->cell;
		atomic_store_explicit(cell, 0, memory_order_relaxed);

		pthread_t threads[N_WORKERS];
		struct worker_args workers[N_WORKERS];
		pthread_barrier_t bar;
		pthread_barrier_init(&bar, NULL, N_WORKERS);
		for (int t = 0; t < N_WORKERS; t++) {
			workers[t].cpu = t;
			workers[t].mech = mech;
			workers[t].cell = cell;
			workers[t].bounce = mem->bounce[t];
			workers[t].bursts = BURSTS;
			workers[t].per = PER_BURST;
			workers[t].result = 0;
			workers[t].bar = &bar;
			pthread_create(&threads[t], NULL, worker, &workers[t]);
		}

		uint64_t succ = 0;
		for (int t = 0; t < N_WORKERS; t++) {
			pthread_join(threads[t], NULL);
			succ += workers[t].result;
		}

		const uint64_t final = atomic_load_explicit(cell, memory_order_relaxed);
		const uint64_t ops = (uint64_t)N_WORKERS * BURSTS * PER_BURST;
		total_ops += ops;
		if (final != succ) {
			uint64_t lost = succ - final;
			total_lost += lost;
			bad_rounds++;
			printf("REPRODUCED: round %d lost=%lu (final=%lu succ=%lu, "
			       "%.1fM ops/round) after %.1fs\n",
			       r, (unsigned long)lost, (unsigned long) final, (unsigned long)succ,
			       ops / 1e6, now_sec() - t0);
		}

		pthread_barrier_destroy(&bar);
		free(mem);
	}

	printf("done: %d rounds, %.1fM ops, lost-runs=%d lost-events-total=%lu "
	       "in %.1fs\n",
	       rounds, total_ops / 1e6, bad_rounds, (unsigned long)total_lost, now_sec() - t0);
	if (bad_rounds) {
		const char *mech_str;
		switch (mech) {
		case MECH_AMCAS:
			mech_str = "amcas.d";
			break;
		case MECH_AMCASDB:
			mech_str = "amcas_db.d";
			break;
		case MECH_LLSC:
			mech_str = "ll/sc";
			break;
		case MECH_AMADD:
			mech_str = "amadd.d";
			break;
		}
		printf("VERDICT: REPRODUCED -- %s lost updates observed "
		       "(successes counted, writes dropped)\n",
		       mech_str);
		return EXIT_SUCCESS;
	}
	printf("VERDICT: no loss observed in %d rounds (%.0fM ops)\n", rounds, total_ops / 1e6);
	return EXIT_FAILURE;
}
