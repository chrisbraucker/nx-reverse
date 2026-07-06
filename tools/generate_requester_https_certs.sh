#!/usr/bin/env bash
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)

OUT_DIR="${OUT_DIR:-$REPO_ROOT/workspace/requester-harness/tls}"
CERT_BASENAME="${CERT_BASENAME:-requester-local}"
CERT_DAYS="${CERT_DAYS:-3650}"
COMMON_NAME="${COMMON_NAME:-requester.local}"
ALT_DNS_NAME="${ALT_DNS_NAME:-requester.local}"
ALT_IP_ADDR="${ALT_IP_ADDR:-127.0.0.1}"

CERT_FILE="$OUT_DIR/$CERT_BASENAME.crt"
KEY_FILE="$OUT_DIR/$CERT_BASENAME.key"
OPENSSL_CNF="$OUT_DIR/$CERT_BASENAME.openssl.cnf"

mkdir -p "$OUT_DIR"

cat >"$OPENSSL_CNF" <<EOF
[req]
distinguished_name = dn
x509_extensions = v3_req
prompt = no

[dn]
CN = $COMMON_NAME

[v3_req]
basicConstraints = CA:FALSE
keyUsage = digitalSignature, keyEncipherment
extendedKeyUsage = serverAuth
subjectAltName = @alt_names

[alt_names]
DNS.1 = $ALT_DNS_NAME
IP.1 = $ALT_IP_ADDR
EOF

openssl req \
  -x509 \
  -newkey rsa:2048 \
  -nodes \
  -sha256 \
  -days "$CERT_DAYS" \
  -keyout "$KEY_FILE" \
  -out "$CERT_FILE" \
  -config "$OPENSSL_CNF"

printf 'wrote %s\n' "$CERT_FILE"
printf 'wrote %s\n' "$KEY_FILE"
