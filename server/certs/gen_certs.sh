#!/usr/bin/env bash
# Generate a self-signed CA + server certificate for TLS modem offload.
# Usage: bash gen_certs.sh <server-hostname>
#
# Outputs:
#   ca.key / ca.crt         — CA keypair (ca.crt goes to modem + committed)
#   server.key / server.crt — server keypair (deploy to server, never commit)
#   ../src/ca_cert.h        — CA cert as a C string for firmware embedding
set -euo pipefail
cd "$(dirname "$0")"

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
} > ../src/ca_cert.h

echo "Done.  Deploy server.key + server.crt to the server."
echo "       ca.crt is embedded in the firmware via src/ca_cert.h."
