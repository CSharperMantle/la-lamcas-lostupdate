CC ?= gcc
CFLAGS ?= -std=gnu23 -O2 -march=la64v1.1 -mno-lsx -mno-lasx -Wall -Wextra -pthread

.PHONY: all
all: amcas_lostupdate

amcas_lostupdate: amcas_lostupdate.c
	$(CC) $(CFLAGS) -o $@ $<

# the reproducer must not contain any LSX (v*) or LASX (xv*) instructions
.PHONY: check-scalar
check-scalar: amcas_lostupdate
	@if objdump -d amcas_lostupdate | grep -E '	v[a-z]|	xv[a-z]'; then \
		echo "FAIL: vector instructions found"; exit 1; \
	else \
		echo "scalar only: no LSX/LASX instructions"; \
	fi

.PHONY: clean
clean:
	rm -f amcas_lostupdate
