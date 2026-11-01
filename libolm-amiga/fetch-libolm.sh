#!/bin/bash
# Recreate third_party/libolm from the python-olm sdist on PyPI (libolm 3.2.16)
# and apply our patches.  Only needed if third_party/libolm is missing.
# (gitlab.matrix.org is the upstream; the python-olm sdist carries the same tree.)
set -eu
here=$(cd "$(dirname "$0")" && pwd)
dst=$here/../third_party/libolm
tmp=$(mktemp -d)
pip download --no-binary :all: --no-deps python-olm==3.2.16 -d "$tmp"
tar xzf "$tmp"/python-olm-3.2.16.tar.gz -C "$tmp"
rm -rf "$dst"; mv "$tmp"/python-olm-3.2.16/libolm "$dst"
for p in "$here"/patches/*.patch; do patch -d "$dst" -p1 < "$p"; done
rm -rf "$tmp"
