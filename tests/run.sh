#!/bin/bash
# Build and run the in-process robustness tests under AddressSanitizer +
# UndefinedBehaviorSanitizer. Run from the repo root: bash tests/run.sh
set -e
cd "$(dirname "$0")/.."
SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -g"
cc -std=c11 -Wall -Wextra -pthread $SAN -Isrc \
    tests/test_robust.c \
    src/util.c src/sha256.c src/apiserver.c src/wire.c src/tls.c \
    src/manifest.c src/blob.c src/membership.c src/gossip.c \
    -o /tmp/bube_test
ASAN_OPTIONS=abort_on_error=1:detect_leaks=0 /tmp/bube_test
