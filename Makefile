# Bubernetes - a peer-to-peer process orchestrator in C.
#
# Everything links into one static binary per command, exactly as the workloads
# it runs are expected to: `make` yields a bubelet and a bubectl that depend on
# nothing at runtime.
#
#   make              build bubelet, bubectl (static, cleartext transport)
#   make TLS=1        same, but node-to-node traffic is mutual-auth TLS (OpenSSL)
#   make examples     build the sample static workload
#   make clean
#
# Plain and TLS builds keep separate object directories, so switching between
# them never links a stale object compiled for the other mode. Both write their
# binaries to bin/; the last build wins there.

CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -pthread
LDFLAGS ?= -static -pthread

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

.PHONY: all examples clean
all: $(BIN)/bubelet $(BIN)/bubectl

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
