#!/bin/bash
# Generate a cluster CA and one cert per node for Bubernetes mutual TLS.
#
#   ./gen-certs.sh OUTDIR name1 [name2 ...]
#
# Produces OUTDIR/<name>/{ca.pem,node.pem,node.key} for each node. Point a
# bubelet at its own directory with --certs OUTDIR/<name>. Every node trusts the
# same CA and presents a cert whose SAN is its node name, which is exactly the
# identity SWIM and the scheduler use - so "in the cluster" means "holds a cert
# signed by the cluster CA", and a replacement machine taking a node's name and
# cert is, to everyone else, the same node.
set -e
OUT=$1; shift
[ -n "$OUT" ] && [ $# -ge 1 ] || { echo "usage: $0 OUTDIR name1 [name2 ...]"; exit 2; }
mkdir -p "$OUT"

openssl req -x509 -newkey rsa:2048 -nodes -keyout "$OUT/ca.key" -out "$OUT/ca.pem" \
    -days 3650 -subj "/CN=bubernetes-ca" 2>/dev/null

for n in "$@"; do
    mkdir -p "$OUT/$n"
    cp "$OUT/ca.pem" "$OUT/$n/ca.pem"
    openssl req -newkey rsa:2048 -nodes -keyout "$OUT/$n/node.key" \
        -out "$OUT/$n/node.csr" -subj "/CN=$n" 2>/dev/null
    openssl x509 -req -in "$OUT/$n/node.csr" -CA "$OUT/ca.pem" -CAkey "$OUT/ca.key" \
        -CAcreateserial -out "$OUT/$n/node.pem" -days 3650 \
        -extfile <(printf "subjectAltName=DNS:%s" "$n") 2>/dev/null
    rm -f "$OUT/$n/node.csr"
    echo "wrote $OUT/$n/{ca.pem,node.pem,node.key}  (CN=$n)"
done
