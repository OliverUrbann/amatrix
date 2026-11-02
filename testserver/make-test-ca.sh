#!/bin/bash
# Private test CA + server certificate for localhost/127.0.0.1 (tlsproxy.py).
set -eu
d=${1:-$HOME/amatrix-tls}; mkdir -p "$d"; cd "$d"
openssl req -x509 -newkey rsa:2048 -nodes -keyout ca.key -out ca.pem -days 3650 -subj "/CN=Amatrix Test CA"
openssl req -newkey rsa:2048 -nodes -keyout srv.key -out srv.csr -subj "/CN=localhost"
printf "subjectAltName=DNS:localhost,IP:127.0.0.1\nbasicConstraints=CA:FALSE\nextendedKeyUsage=serverAuth\n" > ext.cnf
openssl x509 -req -in srv.csr -CA ca.pem -CAkey ca.key -CAcreateserial -out srv.pem -days 3650 -extfile ext.cnf
echo "CA hash: $(openssl x509 -hash -noout -in ca.pem)"
