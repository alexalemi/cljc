CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function
PREFIX  ?= /usr/local
DESTDIR ?=
SHAREDIR = $(PREFIX)/share/cljc
BATTERIES = libc.clj json.clj fs.clj process.clj test.clj jit.clj bundle.clj clerk.clj judge.cljc csp.clj http.clj nrepl.clj

cljc: cljc.c
	$(CC) $(CFLAGS) -DCLJC_SHAREDIR='"$(SHAREDIR)"' -o $@ $< -lm -ldl

run: cljc
	./cljc

example: examples/host.c cljc.c
	$(CC) $(CFLAGS) -o examples/host examples/host.c -lm -ldl
	./examples/host

test: cljc
	@./cljc tests.clj 2>&1 | grep -E 'FAIL|error' && exit 1 || true
	@./cljc tests.clj > /dev/null
	@CLJC_GC_STRESS=1 ./cljc tests.clj 2>&1 | grep -E 'FAIL|error' && exit 1 || true
	@CLJC_GC_STRESS=1 ./cljc tests.clj > /dev/null
	@echo "all tests pass (normal + GC stress)"

install: cljc
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 cljc $(DESTDIR)$(PREFIX)/bin/cljc
	install -d $(DESTDIR)$(SHAREDIR)
	install -m 644 $(BATTERIES) $(DESTDIR)$(SHAREDIR)/
	install -m 644 cljc.c $(DESTDIR)$(SHAREDIR)/   # `cljc bundle` needs the source
	install -d $(DESTDIR)$(SHAREDIR)/vendor/clojure
	install -m 644 vendor/clojure/*.clj $(DESTDIR)$(SHAREDIR)/vendor/clojure/
	install -d $(DESTDIR)$(SHAREDIR)/vendor/clojure/core
	install -m 644 vendor/clojure/core/*.clj $(DESTDIR)$(SHAREDIR)/vendor/clojure/core/
	install -d $(DESTDIR)$(SHAREDIR)/vendor/clojure/java
	install -m 644 vendor/clojure/java/*.clj $(DESTDIR)$(SHAREDIR)/vendor/clojure/java/
	install -d $(DESTDIR)$(SHAREDIR)/vendor/clojure/data
	install -m 644 vendor/clojure/data/*.clj $(DESTDIR)$(SHAREDIR)/vendor/clojure/data/
	install -d $(DESTDIR)$(SHAREDIR)/vendor/clojure/math
	install -m 644 vendor/clojure/math/*.clj $(DESTDIR)$(SHAREDIR)/vendor/clojure/math/
	install -d $(DESTDIR)$(SHAREDIR)/vendor/nextjournal
	install -m 644 vendor/nextjournal/*.clj $(DESTDIR)$(SHAREDIR)/vendor/nextjournal/
	install -d $(DESTDIR)$(SHAREDIR)/vendor/medley
	install -m 644 vendor/medley/*.cljc $(DESTDIR)$(SHAREDIR)/vendor/medley/
	install -d $(DESTDIR)$(SHAREDIR)/vendor/cheshire
	install -m 644 vendor/cheshire/*.clj $(DESTDIR)$(SHAREDIR)/vendor/cheshire/
	install -d $(DESTDIR)$(SHAREDIR)/vendor/camel_snake_kebab/internals
	install -m 644 vendor/camel_snake_kebab/*.cljc $(DESTDIR)$(SHAREDIR)/vendor/camel_snake_kebab/
	install -m 644 vendor/camel_snake_kebab/internals/*.cljc $(DESTDIR)$(SHAREDIR)/vendor/camel_snake_kebab/internals/
	@echo "installed $(DESTDIR)$(PREFIX)/bin/cljc (batteries: $(SHAREDIR))"

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/cljc
	rm -rf $(DESTDIR)$(SHAREDIR)

clean:
	rm -f examples/host
	rm -f cljc

lint:
	clj-kondo --lint json.clj libc.clj test.clj examples 2>/dev/null || true
	@echo "(name files .cljc if they use #?(:cljc ...) reader conditionals)"

.PHONY: run test clean example lint install uninstall
