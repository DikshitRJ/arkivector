# `daemon/Dockerfile`

Multi-stage Alpine Linux Docker build producing a scratch-based static binary distribution for `arkivectord`.

## Build Stages
1. **Builder**: Uses `alpine:3.20` with static libraries (`libuv-static`, `openssl-libs-static`, `curl-static`), musl toolchain, and CMake Ninja generator. Compiles with `-DAV_STATIC=ON` and strips the resulting binary.
2. **Runtime**: Uses `FROM scratch`, copying CA root certificates (`/etc/ssl/certs/ca-certificates.crt`) and the single statically-linked `arkivectord` binary.
