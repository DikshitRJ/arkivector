#!/usr/bin/env bash
# Build libcurl from source with OpenSSL into $CURL_PREFIX (CI only).
# Ubuntu 24.04 ships curl 8.5, but the daemon needs >= 8.11 (WebSocket).
# Usage: CURL_VERSION=8.23.0 CURL_PREFIX=/opt/curl tools/ci-install-curl.sh
set -euo pipefail
: "${CURL_VERSION:?}" "${CURL_PREFIX:?}"
if [ -f "$CURL_PREFIX/lib/pkgconfig/libcurl.pc" ]; then echo "curl cached"; exit 0; fi
work="$(mktemp -d)"
curl -fsSL "https://curl.se/download/curl-${CURL_VERSION}.tar.xz" -o "$work/curl.tar.xz"
tar -C "$work" -xf "$work/curl.tar.xz"
cd "$work/curl-${CURL_VERSION}"
./configure --prefix="$CURL_PREFIX" --with-openssl --without-libpsl --without-libidn2 \
  --without-nghttp2 --without-brotli --without-zstd --disable-ldap --disable-docs \
  --disable-manual --disable-static
make -j"$(nproc)"
sudo make install
