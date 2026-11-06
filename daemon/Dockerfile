# Multi-stage static build for arkivectord
FROM alpine:3.20 AS builder

RUN apk add --no-cache \
    build-base \
    cmake \
    ninja \
    linux-headers \
    libuv-dev \
    libuv-static \
    openssl-dev \
    openssl-libs-static \
    curl-dev \
    curl-static \
    ca-certificates \
    python3 \
    git

WORKDIR /src

# Copy monorepo sources needed for build
COPY spec /src/spec
COPY pgvector /src/pgvector
COPY daemon /src/daemon

WORKDIR /src/daemon

RUN cmake -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DAV_STATIC=ON \
    -DAV_BUILD_TESTS=OFF \
    -DAV_BUILD_BENCH=OFF \
    -DAV_NATIVE=OFF

RUN cmake --build build --target arkivectord
RUN strip build/arkivectord

# Final minimal scratch runtime
FROM scratch

COPY --from=builder /etc/ssl/certs/ca-certificates.crt /etc/ssl/certs/
COPY --from=builder /src/daemon/build/arkivectord /usr/local/bin/arkivectord

EXPOSE 8080

ENTRYPOINT ["/usr/local/bin/arkivectord"]
