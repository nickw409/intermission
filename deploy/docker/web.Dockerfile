# Edge image: Caddy plus the built web bundle and deploy/Caddyfile. Build context: the repo root.
#   docker build -f deploy/docker/web.Dockerfile --build-arg GIT_SHA=<sha> -t intermission-web:<sha> .
#
# Stage 1 builds the SPA with Node (pinned by digest, `npm ci` from the lockfile).
# Stage 2 takes the static caddy binary from Caddy's official image (pinned by digest).
# Runtime: distroless static, non-root, no shell. Caddy's official image is Alpine-based and
# ships a shell, so only its binary is used.
# Avoid BuildKit-only syntax: the owner's host builds with the classic builder.

FROM node:24.22.2-trixie-slim@sha256:0820eaa85e07d0490446dd01d14707e17e6108a36988cf374899cd057eb3812d AS web
ENV NPM_CONFIG_UPDATE_NOTIFIER=false NPM_CONFIG_FUND=false NPM_CONFIG_AUDIT=false
WORKDIR /src/web
COPY web/package.json web/package-lock.json ./
RUN npm ci --no-audit --no-fund
# The SPA imports a few shared definitions from ../contracts.
COPY contracts/ /src/contracts/
COPY web/ ./
RUN npm run build
# The Caddyfile passes through this stage so its mode is fixed (0644) whatever the build
# context's permissions are (COPY --chmod needs BuildKit).
COPY deploy/Caddyfile /out/Caddyfile
RUN chmod -R a+rX dist && chmod 0644 /out/Caddyfile

FROM caddy:2.11.9@sha256:0c2243efcf437ab7f7a978f1027fcf0d4f9be8a75df27b1b3fbc5dbe0ffa8cf0 AS caddy

FROM gcr.io/distroless/static-debian13:nonroot@sha256:25ac8f0c3b2334a9eb05452266bd4697864a8c8ef0ef71e5e6480d7f92868329
ARG GIT_SHA=unknown
LABEL org.opencontainers.image.title="intermission-web" \
      org.opencontainers.image.source="https://github.com/nickw409/intermission" \
      org.opencontainers.image.revision="${GIT_SHA}"
COPY --from=caddy /usr/bin/caddy /usr/local/bin/caddy
COPY --from=web /src/web/dist/ /srv/www/
COPY --from=web /out/Caddyfile /etc/caddy/Caddyfile
# Caddy keeps autosave config and state under XDG dirs; compose mounts /tmp as tmpfs.
ENV XDG_CONFIG_HOME=/tmp/caddy-config XDG_DATA_HOME=/tmp/caddy-data
USER 10004:10004
EXPOSE 8080
ENTRYPOINT ["/usr/local/bin/caddy"]
CMD ["run", "--config", "/etc/caddy/Caddyfile", "--adapter", "caddyfile"]
