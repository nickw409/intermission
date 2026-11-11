# recs-backup: one-shot SQLite backup/verify/restore tool (deploy/tools/recs-backup).
# Build context: the repo root.
#   docker build -f deploy/docker/backup.Dockerfile --build-arg GIT_SHA=<sha> -t intermission-backup:<sha> .
# Pure Go (modernc.org/sqlite), so the runtime is distroless static: no shell, no sqlite3 CLI.

FROM golang:1.27.4-trixie@sha256:b54b2639de8c1bfe538ec5ffc45f29b1932235ac5f6d63c059853646677112d9 AS build
ENV CGO_ENABLED=0 GOFLAGS=-mod=readonly GOTOOLCHAIN=local
WORKDIR /src
COPY deploy/tools/recs-backup/go.mod deploy/tools/recs-backup/go.sum ./
RUN go mod download && go mod verify
COPY deploy/tools/recs-backup/ ./
RUN go build -trimpath -ldflags='-s -w -buildid=' -o /out/recs-backup . \
 && mkdir -p /out/root/srv/data/db /out/root/srv/data/backups \
 && chown -R 10001:10001 /out/root/srv/data

FROM gcr.io/distroless/static-debian13:nonroot@sha256:25ac8f0c3b2334a9eb05452266bd4697864a8c8ef0ef71e5e6480d7f92868329
ARG GIT_SHA=unknown
LABEL org.opencontainers.image.title="intermission-backup" \
      org.opencontainers.image.source="https://github.com/nickw409/intermission" \
      org.opencontainers.image.revision="${GIT_SHA}"
COPY --from=build /out/root/ /
COPY --from=build /out/recs-backup /usr/local/bin/recs-backup
WORKDIR /srv
# Same UID as recs-api: it reads app.db and owns the backups.
USER 10001:10001
ENTRYPOINT ["/usr/local/bin/recs-backup"]
CMD ["backup", "--db", "/srv/data/db/app.db", "--dir", "/srv/data/backups"]
