#!/usr/bin/env bash
# Generate a self-signed CA + server certificate for TLS modem offload.
# Usage: bash gen_certs.sh <server-hostname> [output-dir]
#
# Outputs, all in output-dir, or beside this script without one:
#   ca.key / ca.crt         — CA keypair (ca.crt goes to firmware/certs/ on the
#                             build machine; never commit either)
#   server.key / server.crt — server keypair (deploy to server, never commit)
#   ca_cert.h               — CA cert as a C string (firmware/build.sh makes
#                             its own from ca.crt)
#
# firmware/certs/gen_certs.sh is the same script run from the firmware tree,
# where it writes ../src/ca_cert.h in place.  There is no src/ beside the
# server, so this copy leaves the header here instead.
set -euo pipefail
cd "${2:-$(dirname "$0")}"

HOST="${1:?Usage: $0 <server-hostname>}"
DAYS_CA=3650
DAYS_SRV=825

echo "--- CA ---"
openssl ecparam -genkey -name prime256v1 -out ca.key
openssl req -new -x509 -key ca.key -out ca.crt -days "$DAYS_CA" \
    -subj "/CN=l0destar CA"

echo "--- server cert for ${HOST} ---"
openssl ecparam -genkey -name prime256v1 -out server.key
openssl req -new -key server.key -out server.csr -subj "/CN=${HOST}"
openssl x509 -req -in server.csr -CA ca.crt -CAkey ca.key -CAcreateserial \
    -out server.crt -days "$DAYS_SRV" \
    -extfile <(printf "subjectAltName=DNS:%s" "$HOST")
rm -f server.csr ca.srl

echo "--- ca_cert.h ---"
{
    echo '#ifndef CA_CERT_H'
    echo '#define CA_CERT_H'
    echo ''
    echo 'static const char ca_cert_pem[] ='
    sed 's/.*/"&\\n"/' ca.crt
    echo ';'
    echo ''
    echo '#endif'
} > ca_cert.h

echo "Done.  server.key + server.crt are the listener's tls_key / tls_cert."
echo "       Copy ca.crt to firmware/certs/ca.crt on the build machine: the"
echo "       firmware build embeds it, and push_fw.sh checks the server with it."
