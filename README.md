# Miniature AMCAS lost-update reproducer (Loongson LA664 / 3B6000)

## Build

```sh
make
# objdump scan: must print "scalar only"
make check-scalar
```

## Run

```sh
# ./amcas_lostupdate <amcas|amcasdb|llsc> <rounds> <spinner-processes>

./amcas_lostupdate
./amcas_lostupdate amcas 60 3
./amcas_lostupdate llsc 60 3
```

Exit 0 = reproduced; exit 2 = no loss observed within the round budget.

## What it does

8 worker threads (CPUs 0..7) run a CAS "ticket" loop on one shared 64-bit cell:

```plain-text
expect = observed cell value
rd = amcas.d(cell, expect, expect+1)
if (rd == expect)
	successes++	// swap reported success
else
	expect = rd	// retry
```

Between 64-op bursts each worker dirties a private 128KB buffer (512 lines * 64B * 8 sweeps of scalar byte ld/st), stretching the AMCAS read->write window via constant cache-line transfer.

The architecture requires: final_cell == sum(successes). When the erratum hits, final < sum(successes): AMCAS returned `old == expected` (software counts a success) but the write was dropped.

3 spinner child processes (independent AMCAS users on private lines) raise the loss rate; the erratum reproduces without them too, just more slowly.
