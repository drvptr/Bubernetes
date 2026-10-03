# Bubernetes - a peer-to-peer process orchestrator in C.
#
# Everything links into one static binary per command, exactly as the workloads
# it runs are expected to: `make` yields a bubelet and a bubectl that depend on
# nothing at runtime.
#
#   make              build bubelet, bubectl (static, cleartext transport)
#   make TLS=1        same, but node-to-node traffic is mutual-auth TLS (OpenSSL)
#   make examples     build the sample static workload
#   make install      binaries to $(PREFIX)/bin, man pages to $(PREFIX)/share/man
#   make uninstall
#   make clean
#
# Plain and TLS builds keep separate object directories, so switching between
# them never links a stale object compiled for the other mode. Both write their
# binaries to bin/; the last build wins there.

CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -pthread
LDFLAGS ?= -static -pthread
PREFIX  ?= /usr/local
MANDIR  ?= $(PREFIX)/share/man

SRC := src
BIN := bin

ifdef TLS
OBJ    := build/tls
CFLAGS += -DBUBE_TLS
LDLIBS += -lssl -lcrypto -lpthread -ldl
else
OBJ    := build/plain
endif

# header dependencies, generated as a side effect of compiling
CFLAGS += -MMD -MP

CORE := \
	$(OBJ)/util.o $(OBJ)/sha256.o $(OBJ)/apiserver.o $(OBJ)/wire.o \
	$(OBJ)/tls.o $(OBJ)/manifest.o $(OBJ)/blob.o $(OBJ)/exec.o \
	$(OBJ)/membership.o $(OBJ)/gossip.o $(OBJ)/scheduler.o \
	$(OBJ)/reconciler.o $(OBJ)/event.o

CTL := \
	$(OBJ)/util.o $(OBJ)/sha256.o $(OBJ)/apiserver.o $(OBJ)/wire.o \
	$(OBJ)/tls.o $(OBJ)/manifest.o

.PHONY: all examples install uninstall clean
all: $(BIN)/bubelet $(BIN)/bubectl

install: all
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 0755 $(BIN)/bubelet $(BIN)/bubectl $(DESTDIR)$(PREFIX)/bin/
	install -d $(DESTDIR)$(MANDIR)/man1 $(DESTDIR)$(MANDIR)/man7 $(DESTDIR)$(MANDIR)/man8
	install -m 0644 man/bubectl.1    $(DESTDIR)$(MANDIR)/man1/
	install -m 0644 man/bubernetes.7 $(DESTDIR)$(MANDIR)/man7/
	install -m 0644 man/bubelet.8    $(DESTDIR)$(MANDIR)/man8/
	@echo "installed to $(DESTDIR)$(PREFIX); try: man bubernetes"

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/bubelet $(DESTDIR)$(PREFIX)/bin/bubectl
	rm -f $(DESTDIR)$(MANDIR)/man1/bubectl.1 $(DESTDIR)$(MANDIR)/man7/bubernetes.7 \
	      $(DESTDIR)$(MANDIR)/man8/bubelet.8

$(BIN)/bubelet: $(CORE) $(OBJ)/bubelet.o | $(BIN)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BIN)/bubectl: $(CTL) $(OBJ)/bubectl.o | $(BIN)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(OBJ)/%.o: $(SRC)/%.c | $(OBJ)
	$(CC) $(CFLAGS) -c $< -o $@

examples: $(BIN)/workload
$(BIN)/workload: examples/workload.c | $(BIN)
	$(CC) -static -O2 -o $@ $<

$(BIN):
	mkdir -p $(BIN)
$(OBJ):
	mkdir -p $(OBJ)

clean:
	rm -rf build $(BIN)

-include $(wildcard $(OBJ)/*.d)
