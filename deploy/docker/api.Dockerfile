# recs-api (Go). Build context: the repo root.
#   docker build -f deploy/docker/api.Dockerfile --build-arg GIT_SHA=<sha> -t intermission-api:<sha> .
#
# Builder: official Go image, pinned by digest. Runtime: distroless static (no shell, no
# package manager, no libc), non-root. modernc.org/sqlite is pure Go, so CGO is off and the
# binary is fully static.
# Avoid BuildKit-only syntax (RUN --mount, COPY --chmod): the owner's host builds with the
# classic builder.

FROM golang:1.27.3-trixie@sha256:af1bfc2614ebb1dbd31af514b78740fd678850e76498762b425ecee4c5c6ea80 AS build
ENV CGO_ENABLED=0 GOFLAGS=-mod=readonly GOTOOLCHAIN=local
WORKDIR /src/api
COPY api/go.mod api/go.sum ./
RUN go mod download && go mod verify
COPY api/ ./
RUN go build -trimpath -ldflags='-s -w -buildid=' -o /out/recs-api ./cmd/recs-api
# Directories the runtime image needs, created here because the runtime has no shell.
# /run/recs is the model socket directory (a named volume in compose; the volume copies this
# ownership on first use): owner recs-model (10002), group recs (10000), 0750.
# /srv/data/* are mount points; the real permissions come from the host directories.
RUN mkdir -p /out/root/run/recs /out/root/srv/data/db /out/root/srv/data/exports \
        /out/root/srv/data/reports /out/root/srv/data/posters /out/root/tmp \
 && chown 10002:10000 /out/root/run/recs && chmod 0750 /out/root/run/recs \
 && chown -R 10001:10001 /out/root/srv/data && chmod 1777 /out/root/tmp

FROM gcr.io/distroless/static-debian13:nonroot@sha256:25ac8f0c3b2334a9eb05452266bd4697864a8c8ef0ef71e5e6480d7f92868329
ARG GIT_SHA=unknown
LABEL org.opencontainers.image.title="intermission-api" \
      org.opencontainers.image.source="https://github.com/nickw409/intermission" \
      org.opencontainers.image.revision="${GIT_SHA}"
COPY --from=build /out/root/ /
COPY --from=build /out/recs-api /usr/local/bin/recs-api
WORKDIR /srv
# Container defaults; compose sets the same values explicitly.
ENV RECS_ADDR=0.0.0.0:8081 \
    RECS_DB_PATH=/srv/data/db/app.db \
    RECS_MODEL_SOCKET=/run/recs/model.sock \
    RECS_REPORTS_DIR=/srv/data/reports \
    RECS_EXPORTS_DIR=/srv/data/exports \
    RECS_POSTER_CACHE_DIR=/srv/data/posters \
    TMPDIR=/tmp
# api user 10001, supplementary group recs (10000) is added by compose (group_add).
USER 10001:10001
EXPOSE 8081
ENTRYPOINT ["/usr/local/bin/recs-api"]
CMD ["serve"]
