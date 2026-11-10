# recs-train (C++ batch job: ingest, train, eval, retrain). Build context: the repo root.
#   docker build -f deploy/docker/train.Dockerfile --build-arg GIT_SHA=<sha> -t intermission-train:<sha> .
#
# The builder stage is byte-identical to the one in model.Dockerfile and builds both binaries,
# so whichever image is built second reuses the cached compile layer.
# Builder: Ubuntu 24.04 (the CI compiler, GCC 13), pinned by digest; apt reads a dated Ubuntu
# snapshot so the toolchain versions are pinned too. Runtime: distroless cc (glibc, libstdc++,
# no shell), non-root; libgomp is copied in from the builder.
# Avoid BuildKit-only syntax: the owner's host builds with the classic builder.

FROM ubuntu:noble-20261024.1@sha256:fe81560990fae86dc4743202b2cb4d3df98d0bcf949173cecef15ce1127ef852 AS build
ARG APT_SNAPSHOT=20261101T000000Z
# snapshot.ubuntu.com is HTTPS-only, so ca-certificates comes first from the image's own
# (signed) archive sources; everything after that comes from the dated snapshot.
RUN apt-get update -q \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y -q --no-install-recommends ca-certificates \
 && apt-get update -q --snapshot "${APT_SNAPSHOT}" \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y -q --no-install-recommends --snapshot "${APT_SNAPSHOT}" \
        cmake ninja-build g++ \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY CMakeLists.txt CMakePresets.json ./
COPY cmake/ cmake/
COPY core/ core/
COPY train/ train/
COPY model-service/ model-service/
ARG GIT_SHA=unknown
ARG BUILD_JOBS=3
RUN cmake --preset release -DRECS_BUILD_TESTS=OFF -DRECS_GIT_SHA="${GIT_SHA}" \
 && cmake --build --preset release --parallel "${BUILD_JOBS}" --target recs-model recs-train \
 && mkdir -p /out/lib /out/root/run/recs /out/root/srv/data/artifacts \
 && cp build/release/model-service/recs-model build/release/train/recs-train /out/ \
 && strip /out/recs-model /out/recs-train \
 && cp -L "$(g++ -print-file-name=libgomp.so.1)" /out/lib/libgomp.so.1 \
 && chown 10002:10000 /out/root/run/recs && chmod 0750 /out/root/run/recs

FROM gcr.io/distroless/cc-debian13:nonroot@sha256:cc66d222f0a7ec996b78a99fe76a969bc80b539a5016ad866c14d1949bc6d0ab
ARG GIT_SHA=unknown
LABEL org.opencontainers.image.title="intermission-train" \
      org.opencontainers.image.source="https://github.com/nickw409/intermission" \
      org.opencontainers.image.revision="${GIT_SHA}"
COPY --from=build /out/lib/libgomp.so.1 /usr/local/lib/libgomp.so.1
COPY --from=build /out/recs-train /usr/local/bin/recs-train
ENV LD_LIBRARY_PATH=/usr/local/lib OMP_NUM_THREADS=4
# recs-train resolves its default paths (data/raw, data/cache, data/artifacts, data/reports, ...)
# relative to the working directory; compose mounts each data directory under /srv/data.
WORKDIR /srv
USER 10003:10003
ENTRYPOINT ["/usr/local/bin/recs-train"]
CMD ["--help"]
