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
./amcas_lostupdate amadd	# erratum mode
./amcas_lostupdate amcasdb	# control - clean
./amcas_lostupdate llsc	# control - clean
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

## Results

On a Loongson 3B6000 (12C24T):

```console
$ ./amcas_lostupdate amcas
mech=amcas rounds=60 workers=3
REPRODUCED: round 0 lost=49301 (final=2950699 succ=3000000, 3.0M ops/round) after 0.1s
REPRODUCED: round 1 lost=69840 (final=2930160 succ=3000000, 3.0M ops/round) after 0.2s
REPRODUCED: round 2 lost=50030 (final=2949970 succ=3000000, 3.0M ops/round) after 0.2s
REPRODUCED: round 3 lost=27877 (final=2972123 succ=3000000, 3.0M ops/round) after 0.3s
REPRODUCED: round 4 lost=63348 (final=2936652 succ=3000000, 3.0M ops/round) after 0.4s
REPRODUCED: round 5 lost=73091 (final=2926909 succ=3000000, 3.0M ops/round) after 0.4s
REPRODUCED: round 6 lost=10156 (final=2989844 succ=3000000, 3.0M ops/round) after 0.5s
REPRODUCED: round 7 lost=71212 (final=2928788 succ=3000000, 3.0M ops/round) after 0.6s
REPRODUCED: round 8 lost=72803 (final=2927197 succ=3000000, 3.0M ops/round) after 0.6s
REPRODUCED: round 9 lost=12706 (final=2987294 succ=3000000, 3.0M ops/round) after 0.7s
REPRODUCED: round 10 lost=70756 (final=2929244 succ=3000000, 3.0M ops/round) after 0.8s
REPRODUCED: round 11 lost=69680 (final=2930320 succ=3000000, 3.0M ops/round) after 0.8s
REPRODUCED: round 12 lost=51511 (final=2948489 succ=3000000, 3.0M ops/round) after 0.9s
REPRODUCED: round 13 lost=73004 (final=2926996 succ=3000000, 3.0M ops/round) after 1.0s
REPRODUCED: round 14 lost=72642 (final=2927358 succ=3000000, 3.0M ops/round) after 1.1s
REPRODUCED: round 15 lost=72616 (final=2927384 succ=3000000, 3.0M ops/round) after 1.1s
REPRODUCED: round 16 lost=73016 (final=2926984 succ=3000000, 3.0M ops/round) after 1.2s
REPRODUCED: round 17 lost=72801 (final=2927199 succ=3000000, 3.0M ops/round) after 1.2s
REPRODUCED: round 18 lost=68652 (final=2931348 succ=3000000, 3.0M ops/round) after 1.3s
REPRODUCED: round 19 lost=65412 (final=2934588 succ=3000000, 3.0M ops/round) after 1.3s
REPRODUCED: round 20 lost=11170 (final=2988830 succ=3000000, 3.0M ops/round) after 1.4s
REPRODUCED: round 21 lost=63371 (final=2936629 succ=3000000, 3.0M ops/round) after 1.5s
REPRODUCED: round 22 lost=63461 (final=2936539 succ=3000000, 3.0M ops/round) after 1.5s
REPRODUCED: round 23 lost=26986 (final=2973014 succ=3000000, 3.0M ops/round) after 1.6s
REPRODUCED: round 24 lost=72573 (final=2927427 succ=3000000, 3.0M ops/round) after 1.7s
REPRODUCED: round 25 lost=73076 (final=2926924 succ=3000000, 3.0M ops/round) after 1.7s
REPRODUCED: round 26 lost=13019 (final=2986981 succ=3000000, 3.0M ops/round) after 1.8s
REPRODUCED: round 27 lost=63778 (final=2936222 succ=3000000, 3.0M ops/round) after 1.9s
REPRODUCED: round 28 lost=63395 (final=2936605 succ=3000000, 3.0M ops/round) after 1.9s
REPRODUCED: round 29 lost=10045 (final=2989955 succ=3000000, 3.0M ops/round) after 2.0s
REPRODUCED: round 30 lost=63490 (final=2936510 succ=3000000, 3.0M ops/round) after 2.1s
REPRODUCED: round 31 lost=63493 (final=2936507 succ=3000000, 3.0M ops/round) after 2.1s
REPRODUCED: round 32 lost=44543 (final=2955457 succ=3000000, 3.0M ops/round) after 2.2s
REPRODUCED: round 33 lost=63620 (final=2936380 succ=3000000, 3.0M ops/round) after 2.3s
REPRODUCED: round 34 lost=63977 (final=2936023 succ=3000000, 3.0M ops/round) after 2.3s
REPRODUCED: round 35 lost=62969 (final=2937031 succ=3000000, 3.0M ops/round) after 2.4s
REPRODUCED: round 36 lost=27475 (final=2972525 succ=3000000, 3.0M ops/round) after 2.5s
REPRODUCED: round 37 lost=63343 (final=2936657 succ=3000000, 3.0M ops/round) after 2.6s
REPRODUCED: round 38 lost=63426 (final=2936574 succ=3000000, 3.0M ops/round) after 2.6s
REPRODUCED: round 39 lost=13324 (final=2986676 succ=3000000, 3.0M ops/round) after 2.7s
REPRODUCED: round 40 lost=63432 (final=2936568 succ=3000000, 3.0M ops/round) after 2.8s
REPRODUCED: round 41 lost=63547 (final=2936453 succ=3000000, 3.0M ops/round) after 2.8s
REPRODUCED: round 42 lost=9999 (final=2990001 succ=3000000, 3.0M ops/round) after 2.9s
REPRODUCED: round 43 lost=63517 (final=2936483 succ=3000000, 3.0M ops/round) after 3.0s
REPRODUCED: round 44 lost=62972 (final=2937028 succ=3000000, 3.0M ops/round) after 3.0s
REPRODUCED: round 45 lost=46843 (final=2953157 succ=3000000, 3.0M ops/round) after 3.1s
REPRODUCED: round 46 lost=63303 (final=2936697 succ=3000000, 3.0M ops/round) after 3.2s
REPRODUCED: round 47 lost=54962 (final=2945038 succ=3000000, 3.0M ops/round) after 3.2s
REPRODUCED: round 48 lost=34096 (final=2965904 succ=3000000, 3.0M ops/round) after 3.3s
REPRODUCED: round 49 lost=39323 (final=2960677 succ=3000000, 3.0M ops/round) after 3.4s
REPRODUCED: round 50 lost=63435 (final=2936565 succ=3000000, 3.0M ops/round) after 3.5s
REPRODUCED: round 51 lost=63440 (final=2936560 succ=3000000, 3.0M ops/round) after 3.5s
REPRODUCED: round 52 lost=63477 (final=2936523 succ=3000000, 3.0M ops/round) after 3.6s
REPRODUCED: round 53 lost=63403 (final=2936597 succ=3000000, 3.0M ops/round) after 3.6s
REPRODUCED: round 54 lost=13186 (final=2986814 succ=3000000, 3.0M ops/round) after 3.7s
REPRODUCED: round 55 lost=63631 (final=2936369 succ=3000000, 3.0M ops/round) after 3.8s
REPRODUCED: round 56 lost=63563 (final=2936437 succ=3000000, 3.0M ops/round) after 3.8s
REPRODUCED: round 57 lost=43798 (final=2956202 succ=3000000, 3.0M ops/round) after 4.0s
REPRODUCED: round 58 lost=72116 (final=2927884 succ=3000000, 3.0M ops/round) after 4.0s
REPRODUCED: round 59 lost=54557 (final=2945443 succ=3000000, 3.0M ops/round) after 4.1s
done: 60 rounds, 180.0M ops, lost-runs=60 lost-events-total=3247588 in 4.1s
VERDICT: REPRODUCED -- amcas.d lost updates observed (successes counted, writes dropped)
$ ./amcas_lostupdate amadd
mech=amadd rounds=60 workers=3
REPRODUCED: round 2 lost=4 (final=2999996 succ=3000000, 3.0M ops/round) after 0.3s
REPRODUCED: round 3 lost=1 (final=2999999 succ=3000000, 3.0M ops/round) after 0.3s
REPRODUCED: round 5 lost=1 (final=2999999 succ=3000000, 3.0M ops/round) after 0.5s
REPRODUCED: round 8 lost=10 (final=2999990 succ=3000000, 3.0M ops/round) after 0.7s
REPRODUCED: round 9 lost=7 (final=2999993 succ=3000000, 3.0M ops/round) after 0.8s
REPRODUCED: round 10 lost=10 (final=2999990 succ=3000000, 3.0M ops/round) after 0.9s
REPRODUCED: round 15 lost=9 (final=2999991 succ=3000000, 3.0M ops/round) after 1.3s
REPRODUCED: round 16 lost=3 (final=2999997 succ=3000000, 3.0M ops/round) after 1.4s
REPRODUCED: round 25 lost=17 (final=2999983 succ=3000000, 3.0M ops/round) after 2.0s
REPRODUCED: round 28 lost=6 (final=2999994 succ=3000000, 3.0M ops/round) after 2.2s
REPRODUCED: round 31 lost=8 (final=2999992 succ=3000000, 3.0M ops/round) after 2.4s
REPRODUCED: round 32 lost=1 (final=2999999 succ=3000000, 3.0M ops/round) after 2.5s
REPRODUCED: round 36 lost=8 (final=2999992 succ=3000000, 3.0M ops/round) after 2.8s
REPRODUCED: round 37 lost=10 (final=2999990 succ=3000000, 3.0M ops/round) after 2.9s
REPRODUCED: round 38 lost=7 (final=2999993 succ=3000000, 3.0M ops/round) after 3.0s
REPRODUCED: round 39 lost=1 (final=2999999 succ=3000000, 3.0M ops/round) after 3.1s
REPRODUCED: round 43 lost=10 (final=2999990 succ=3000000, 3.0M ops/round) after 3.4s
REPRODUCED: round 46 lost=8 (final=2999992 succ=3000000, 3.0M ops/round) after 3.6s
REPRODUCED: round 48 lost=15 (final=2999985 succ=3000000, 3.0M ops/round) after 3.8s
REPRODUCED: round 53 lost=6 (final=2999994 succ=3000000, 3.0M ops/round) after 4.2s
done: 60 rounds, 180.0M ops, lost-runs=20 lost-events-total=142 in 4.6s
VERDICT: REPRODUCED -- amadd.d lost updates observed (successes counted, writes dropped)
$ ./amcas_lostupdate amcasdb
mech=amcasdb rounds=60 workers=3
done: 60 rounds, 180.0M ops, lost-runs=0 lost-events-total=0 in 5.4s
VERDICT: no loss observed in 60 rounds (180M ops)
$ ./amcas_lostupdate llsc
mech=llsc rounds=60 workers=3
done: 60 rounds, 180.0M ops, lost-runs=0 lost-events-total=0 in 4.8s
VERDICT: no loss observed in 60 rounds (180M ops)
```
