CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function

cljc: cljc.c
	$(CC) $(CFLAGS) -o $@ $< -lm

run: cljc
	./cljc

example: examples/host.c cljc.c
	$(CC) $(CFLAGS) -o examples/host examples/host.c -lm
	./examples/host

test: cljc
	@./cljc tests.clj 2>&1 | grep -E 'FAIL|error' && exit 1 || true
	@./cljc tests.clj > /dev/null
	@CLJC_GC_STRESS=1 ./cljc tests.clj 2>&1 | grep -E 'FAIL|error' && exit 1 || true
	@CLJC_GC_STRESS=1 ./cljc tests.clj > /dev/null
	@echo "all tests pass (normal + GC stress)"

clean:
	rm -f examples/host
	rm -f cljc

.PHONY: run test clean example
