# Miniature AMCAS lost-update reproducer (Loongson LA664 / 3B6000)

## Build

```sh
make
# objdump scan: must print "scalar only"
make check-scalar
```

## Run

```sh
# ./amcas_lostupdate [amcas|amcasdb|llsc|amadd] [rounds]

./amcas_lostupdate amcas	# erratum mode
./amcas_lostupdate amcasdb	# control - clean
./amcas_lostupdate llsc	# control - clean
./amcas_lostupdate amadd	# control - clean
```

Exit 0 = reproduced; exit 1 = no loss observed within the round budget.

## What it does

Multiple worker threads (each bound to one hart) run a CAS "ticket" loop on one shared 64-bit cell:

```plain-text
expect = observed cell value
rd = amcas.d(cell, expect, expect+1)
if (rd == expect)
	successes++	// swap reported success
else
	expect = rd	// retry
```

Between 64-op bursts each worker issues 32 plain byte loads (`ld.b`) to a cache line neighboring the cell. The plain-load interleave alone trips the bug; pure stores, same-line or far-line accesses do not.

The architecture requires: `final_cell == sum(successes)`. When the bug hits, `final < sum(successes)`: AMCAS returned `old == expected` (software counts a success) but the write was dropped.
