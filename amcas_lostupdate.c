/* amcas_lostupdate.c - miniature self-contained reproducer for the
 * Loongson LA664 (3B6000) AMCAS lost-update erratum.
 *
 * What it does
 * ------------
 * Eight worker threads on CPUs 0..7 run a CAS "ticket" loop on one shared
 * 64-bit cell:
 *
 *     expect = observed cell value
 *     rd = amcas.d(cell, expect, expect+1)   // write expect+1 iff ==expect
 *     if (rd == expect) successes++          // swap reported success
 *     else expect = rd                       // retry with the new old value
 *
 * invariant under the architecture:  final_cell == sum(successes).
 *
 * Between 64-op bursts each worker dirties a private 128KB buffer
 * (512 lines x 64B x 8 sweeps, plain byte ld/st), so the shared cell line
 * is constantly transferred between cores - this stretches the AMCAS
 * read->write window, which is where the silicon drops writes.
 *
 * To reproduce *reliably*, the reproducer forks `spinners` child processes
 * (default 3) that hammer AMCAS ops on their own private lines, mimicking
 * independent AMCAS user processes (the condition under which the erratum
 * was first observed; quiet-machine runs only rarely lose updates).
 */

#define _GNU_SOURCE

#include <assert.h>
#include <pthread.h>
#include <sched.h>
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
#define N_WORKERS 16
#define DEFAULT_N_SPINNERS 3
#define BOUNCE_SIZE (256 * 1024)

/* ------------------------------------------------------------------ */
/* scalar assembly workers                                            */
/* ------------------------------------------------------------------ */
__asm__(".text\n"

	/* uint64_t ticket_burst_amcas(uint64_t *cell, uint64_t per);
	 *
	 * CAS fetch-and-increment; returns the number of successful swaps.
	 */
	".globl ticket_burst_amcas\n"
	".type  ticket_burst_amcas, @function\n"
	"ticket_burst_amcas:\n"
	".cfi_startproc\n"
	"	li.d	$t2, 0\n"	 /* expect = 0 (cell starts at 0) */
	"	li.d	$t4, 0\n"	 /* successes */
	"1:	addi.d	$t3, $t2, 1\n"	 /* new = expect + 1 */
	"	move	$t5, $t2\n"	 /* save expect */
	"	amcas.d	$t2, $t3, $a0\n" /* t2 = old; mem = t3 iff old==exp */
	"	bne	$t2, $t5, 1b\n"	 /* old != expect -> retry */
	"	addi.d	$t4, $t4, 1\n"	 /* success */
	"	move	$t2, $t3\n"	 /* expect = old + 1 */
	"	addi.d	$a1, $a1, -1\n"
	"	bnez	$a1, 1b\n"
	"	move	$a0, $t4\n"
	"	ret\n"
	".cfi_endproc\n"
	".size ticket_burst_amcas, .-ticket_burst_amcas\n"

	/* uint64_t ticket_burst_amcasdb(uint64_t *cell, uint64_t per);
	 *
	 * Identical protocol on AMCAS_DB. Per spec the RMW atomicity is specified for both forms;
	 * _DB additionally orders surrounding accesses. Discriminates "RMW atomicity broken" from
	 * "non-DB execution path drops writes".
	 */
	".globl ticket_burst_amcasdb\n"
	".type  ticket_burst_amcasdb, @function\n"
	"ticket_burst_amcasdb:\n"
	".cfi_startproc\n"
	"	li.d	$t2, 0\n"
	"	li.d	$t4, 0\n"
	"1:	addi.d	$t3, $t2, 1\n"
	"	move	$t5, $t2\n"
	"	amcas_db.d	$t2, $t3, $a0\n"
	"	bne	$t2, $t5, 1b\n"
	"	addi.d	$t4, $t4, 1\n"
	"	move	$t2, $t3\n"
	"	addi.d	$a1, $a1, -1\n"
	"	bnez	$a1, 1b\n"
	"	move	$a0, $t4\n"
	"	ret\n"
	".cfi_endproc\n"
	".size ticket_burst_amcasdb, .-ticket_burst_amcasdb\n"

	/* uint64_t ticket_burst_llsc(uint64_t *cell, uint64_t per);
	 *
	 * Identical protocol on LL/SC -- the failsafe control.
	 */
	".globl ticket_burst_llsc\n"
	".type  ticket_burst_llsc, @function\n"
	"ticket_burst_llsc:\n"
	".cfi_startproc\n"
	"	li.d	$t2, 0\n"
	"	li.d	$t4, 0\n"
	"1:	ll.d	$t2, $a0, 0\n"
	"	addi.d	$t3, $t2, 1\n"
	"	sc.d	$t3, $a0, 0\n"
	"	beqz	$t3, 1b\n" /* sc failed -> retry */
	"	addi.d	$t4, $t4, 1\n"
	"	addi.d	$a1, $a1, -1\n"
	"	bnez	$a1, 1b\n"
	"	move	$a0, $t4\n"
	"	ret\n"
	".cfi_endproc\n"
	".size ticket_burst_llsc, .-ticket_burst_llsc\n"

	/* void bounce_sweep(uint8_t *buf);
	 *
	 * Dirty 512 lines * 8 sweeps with plain scalar byte ld/st.
	 */
	".globl bounce_sweep\n"
	".type  bounce_sweep, @function\n"
	"bounce_sweep:\n"
	".cfi_startproc\n"
	"	move	$t5, $a0\n"
	"	li.d	$t1, 8\n"   /* 8 sweeps */
	"	li.d	$t6, 512\n" /* 512 lines */
	"1:	li.d	$t2, 0\n"
	"2:	slli.d	$t3, $t2, 6\n" /* offset = line * 64 */
	"	ldx.b	$t4, $t5, $t3\n"
	"	addi.w	$t4, $t4, 1\n"
	"	stx.b	$t4, $t5, $t3\n" /* dirty the line */
	"	addi.d	$t2, $t2, 1\n"
	"	bltu	$t2, $t6, 2b\n"
	"	addi.d	$t1, $t1, -1\n"
	"	bnez	$t1, 1b\n"
	"	ret\n"
	".cfi_endproc\n"
	".size bounce_sweep, .-bounce_sweep\n"

	/* void am_hammer_burst(uint64_t *cell, uint64_t iters);
	 * read + write AMCAS pressure for the spinner processes.
	 */
	".globl am_hammer_burst\n"
	".type  am_hammer_burst, @function\n"
	"am_hammer_burst:\n"
	".cfi_startproc\n"
	"	li.d	$t2, 0xDEADBE00DEADBE00\n"
	"	li.d	$t3, 0x1357246813572468\n"
	"1:	amcas.d	$t2, $t3, $a0\n"	 /* read pressure (compare misses) */
	"	amswap.d	$t2, $t3, $a0\n" /* write pressure (always swaps) */
	"	addi.d	$a1, $a1, -1\n"
	"	bnez	$a1, 1b\n"
	"	ret\n"
	".cfi_endproc\n"
	".size am_hammer_burst, .-am_hammer_burst\n");

extern uint64_t ticket_burst_amcas(uint64_t *cell, uint64_t per);
extern uint64_t ticket_burst_amcasdb(uint64_t *cell, uint64_t per);
extern uint64_t ticket_burst_llsc(uint64_t *cell, uint64_t per);
extern void bounce_sweep(uint8_t *buf);
extern void am_hammer_burst(uint64_t *cell, uint64_t iters);

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

/* spinner child process body: independent AMCAS user (own address space) */
static int spinner_main(int id, bool use_amcas) {
	uint8_t *const mem = mmap(NULL, 4096 + BOUNCE_SIZE, PROT_READ | PROT_WRITE,
				  MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (mem == MAP_FAILED)
		return EXIT_FAILURE;

	uint64_t *const cell = (uint64_t *)(((uintptr_t)mem + 63) & ~63ULL);
	uint8_t *const buf = mem + 4096;
	*cell = 0x1122334455667788ULL;
	memset(buf, id, BOUNCE_SIZE);

	cpu_set_t s;
	CPU_ZERO(&s);
	for (int c = id * 8; c < id * 8 + 8; c++)
		CPU_SET(c, &s);
	sched_setaffinity(0, sizeof s, &s);

	for (;;)
		if (use_amcas) {
			am_hammer_burst(cell, 4096);
			bounce_sweep(buf);
		} else {
			for (volatile int i = 0; i < 100000; i++)
				;
		}
}

/* ------------------------------------------------------------------ */

enum mech {
	MECH_AMCAS,
	MECH_AMCASDB,
	MECH_LLSC,
};

struct worker_args {
	int cpu;
	enum mech mech;
	uint64_t *cell;
	uint8_t *bounce;
	uint64_t bursts, per, result;
	pthread_barrier_t *bar;
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
	const int spinners = (argc > 3) ? atoi(argv[3]) : DEFAULT_N_SPINNERS;
	enum mech mech;
	if (strcmp(mech_s, "llsc") == 0)
		mech = MECH_LLSC;
	else if (strcmp(mech_s, "amcasdb") == 0)
		mech = MECH_AMCASDB;
	else if (strcmp(mech_s, "amcas") == 0)
		mech = MECH_AMCAS;
	else {
		fprintf(stderr, "error: mech must be amcas, amcasdb or llsc\n");
		return EXIT_FAILURE;
	}

	const bool spinner_amcas = (mech != MECH_LLSC);

	printf("mech=%s rounds=%d workers=%d spinners=%d\n", mech_s, rounds, N_WORKERS, spinners);

	// spinner processes: independent AMCAS users on the same CPUs
	pid_t sp[16];
	for (int i = 0; i < spinners; i++) {
		pid_t p = fork();
		if (p == 0)
			_exit(spinner_main(i, spinner_amcas));
		sp[i] = p;
	}

	uint64_t total_ops = 0, total_lost = 0;
	int bad_rounds = 0;
	const double t0 = now_sec();

	for (int r = 0; r < rounds; r++) {
		uint8_t *const mem =
		    mmap(NULL, (size_t)N_WORKERS * BOUNCE_SIZE + 8192, PROT_READ | PROT_WRITE,
			 MAP_SHARED | MAP_ANONYMOUS, -1, 0);
		if (mem == MAP_FAILED) {
			perror("mmap");
			return EXIT_FAILURE;
		}

		uint64_t *const cell = (uint64_t *)(((uintptr_t)mem + 63) & ~63ULL);
		*cell = 0;

		pthread_t threads[N_WORKERS];
		struct worker_args workers[N_WORKERS];
		pthread_barrier_t bar;
		pthread_barrier_init(&bar, NULL, N_WORKERS);
		for (int t = 0; t < N_WORKERS; t++) {
			workers[t].cpu = t;
			workers[t].mech = mech;
			workers[t].cell = cell;
			workers[t].bounce = mem + 8192 + (size_t)t * BOUNCE_SIZE;
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

		const uint64_t final = *(volatile uint64_t *)cell;
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
		munmap(mem, (size_t)N_WORKERS * BOUNCE_SIZE + 8192);
	}

	for (int i = 0; i < spinners; i++)
		kill(sp[i], SIGKILL);
	for (int i = 0; i < spinners; i++)
		waitpid(sp[i], NULL, 0);

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
		}
		printf("VERDICT: REPRODUCED -- %s lost updates observed "
		       "(successes counted, writes dropped)\n",
		       mech_str);
		return EXIT_SUCCESS;
	}
	printf("VERDICT: no loss observed in %d rounds (%.0fM ops)\n", rounds, total_ops / 1e6);
	return EXIT_FAILURE;
}
