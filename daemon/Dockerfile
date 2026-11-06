# arkivectord container image. Build from the repository root:
#   docker build -f daemon/Dockerfile -t arkivectord .
# Run (Rabdi defaults; writes need ARKIV_PRIVATE_KEY):
#   docker run -p 8080:8080 -v arkivector-data:/data \
#     -e ARKIVECTOR_ARKIV_ACCESS_KEY=... -e ARKIV_PRIVATE_KEY=... arkivectord
# Optional config file: -v $PWD/config.toml:/etc/arkivector/config.toml and append
#   --config /etc/arkivector/config.toml

FROM alpine:3.22 AS builder
RUN apk add --no-cache build-base cmake ninja linux-headers libuv-dev openssl-dev curl-dev pkgconf python3
WORKDIR /src
COPY spec /src/spec
COPY pgvector /src/pgvector
COPY daemon /src/daemon
WORKDIR /src/daemon
ARG VERSION=""
RUN cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DAV_BUILD_TESTS=OFF -DAV_BUILD_BENCH=OFF -DAV_NATIVE=OFF \
      -DAV_VERSION="${VERSION}" \
    && cmake --build build --target arkivectord \
    && strip build/arkivectord \
    && build/arkivectord --version

FROM alpine:3.22
LABEL org.opencontainers.image.title="arkivectord" \
      org.opencontainers.image.description="pgvector for Arkiv: off-chain vector co-processor daemon" \
      org.opencontainers.image.source="https://github.com/DikshitRJ/arkivector" \
      org.opencontainers.image.licenses="MIT"
RUN apk add --no-cache libuv libcurl openssl ca-certificates \
    && adduser -D -H -u 10001 arkivector \
    && mkdir -p /data /etc/arkivector && chown arkivector /data
COPY --from=builder /src/daemon/build/arkivectord /usr/local/bin/arkivectord
COPY daemon/arkivector.example.toml /etc/arkivector/arkivector.example.toml
COPY daemon/src/pgvector/LICENSE.pgvector daemon/src/pgcompat/LICENSE.postgresql /usr/share/licenses/arkivector/
USER arkivector
ENV ARKIVECTOR_SERVER_LISTEN=0.0.0.0:8080 \
    ARKIVECTOR_STORAGE_DATA_DIR=/data
VOLUME /data
EXPOSE 8080
HEALTHCHECK --interval=30s --timeout=3s CMD wget -qO- http://127.0.0.1:8080/healthz || exit 1
ENTRYPOINT ["/usr/local/bin/arkivectord"]
