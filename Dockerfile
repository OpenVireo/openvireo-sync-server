# syntax=docker/dockerfile:1
#
# The sync server with both storage backends: the data directory (default, mount a volume on /data) and
# Postgres plus an S3-compatible bucket (set SYNC_DATABASE_URL and SYNC_S3_*). See README.md.
#
#   docker build -t openvireo-sync-server .
#   docker build --target test -t openvireo-sync-server-test .   # the API tests, see compose.cloud.yaml

ARG DEBIAN_RELEASE=bookworm

# Drogon with PostgreSQL support, which distribution packages leave out. This layer only changes with
# DROGON_VERSION, so rebuilding after a change to the server compiles just the server.
FROM debian:${DEBIAN_RELEASE}-slim AS drogon
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential ca-certificates cmake git \
        libbrotli-dev libjsoncpp-dev libpq-dev libsqlite3-dev libssl-dev uuid-dev zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*
ARG DROGON_VERSION=v1.9.14
RUN git clone --depth 1 --branch "${DROGON_VERSION}" --recurse-submodules --shallow-submodules \
        https://github.com/drogonframework/drogon /tmp/drogon \
    && cmake -S /tmp/drogon -B /tmp/drogon/build \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_SHARED_LIBS=OFF \
        -DBUILD_POSTGRESQL=ON -DBUILD_MYSQL=OFF -DBUILD_SQLITE=OFF -DBUILD_REDIS=OFF \
        -DBUILD_EXAMPLES=OFF -DBUILD_CTL=OFF -DBUILD_TESTING=OFF -DBUILD_YAML_CONFIG=OFF \
    && cmake --build /tmp/drogon/build --parallel "$(nproc)" \
    && cmake --install /tmp/drogon/build \
    && rm -rf /tmp/drogon

FROM drogon AS build
WORKDIR /src
COPY CMakeLists.txt ./
COPY src ./src
COPY test ./test
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DSYNC_SERVER_WITH_CLOUD=ON \
    && cmake --build build --parallel "$(nproc)" \
    && cmake --install build --prefix /out

# The API tests (standalone, both and jwt modes); they use Postgres and S3 when SYNC_DATABASE_URL and SYNC_S3_* are set
FROM build AS test
CMD ["ctest", "--test-dir", "/src/build", "--output-on-failure"]

FROM debian:${DEBIAN_RELEASE}-slim AS runtime
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        ca-certificates libbrotli1 libjsoncpp25 libpq5 libsqlite3-0 libssl3 libuuid1 zlib1g \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --uid 10001 --home-dir /data --shell /usr/sbin/nologin openvireo \
    && install -d -o openvireo -g openvireo -m 0700 /data
COPY --from=build /out/bin/openvireo-sync-server /usr/local/bin/openvireo-sync-server

# Listens on all interfaces inside the container; terminate TLS in front of it
ENV SYNC_HOST=0.0.0.0 \
    SYNC_PORT=8787 \
    SYNC_DATA_DIR=/data
USER openvireo
WORKDIR /data
VOLUME ["/data"]
EXPOSE 8787
ENTRYPOINT ["openvireo-sync-server"]
