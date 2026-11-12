#!/usr/bin/env python3
"""Gate: the compose files keep every container locked down.

Spec (Phase 2): "Containers run as non-root with read-only root filesystems.
`docker compose config` shows no published port except `127.0.0.1:8080`."

Runs `docker compose config --format json` for each stack variant and checks:
  * default stack (no profiles): exactly one published port, 127.0.0.1:8080 -> caddy:8080
  * every variant (all profiles, dev and drill overlays): every published port is bound to
    127.0.0.1; every service is read_only, drops ALL capabilities, adds none, sets
    no-new-privileges, runs as a numeric non-root user, has a memory limit and rotated logs;
    nothing is privileged or shares host namespaces; the Docker socket is never mounted;
    recs-model has no network; read-only mounts stay read-only; node-exporter is on internal
    networks only and never mounts the host root.
  * dev overlay: its own project name (intermission-dev), no published port and no writable
    data path shared with production.

Usage: deploy/tests/check_compose.py   (uses deploy/.env.example; needs docker compose)
"""
import json
import os
import subprocess
import sys

DEPLOY = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ENV_EXAMPLE = os.path.join(DEPLOY, ".env.example")

VARIANTS = {
    "default": ([], []),
    "all-profiles": ([], ["jobs", "monitoring", "check"]),
    "dev": (["compose.dev.yaml"], ["jobs", "monitoring", "check"]),
    "drill": (["compose.drill.yaml"], ["jobs"]),
}

# Mounts that must stay read-only: (service, container target).
READ_ONLY_MOUNTS = {
    ("api", "/srv/data/reports"),
    ("model", "/srv/data/artifacts"),
    ("model-check", "/srv/data/artifacts"),
    ("train", "/srv/data/raw"),
    ("train", "/srv/data/exports"),
    ("train", "/srv/configs"),
    ("prometheus", "/etc/prometheus/prometheus.yml"),
    ("prometheus", "/etc/prometheus/alerts"),
    ("node-exporter", "/host"),
    ("node-exporter", "/host/data"),
    ("node-exporter", "/textfile"),
}

errors = []


def fail(variant, msg):
    errors.append(f"[{variant}] {msg}")


def compose_config(overlays, profiles):
    cmd = ["docker", "compose", "--project-directory", DEPLOY, "--env-file", ENV_EXAMPLE,
           "-f", os.path.join(DEPLOY, "compose.yaml")]
    for o in overlays:
        cmd += ["-f", os.path.join(DEPLOY, o)]
    for p in profiles:
        cmd += ["--profile", p]
    cmd += ["config", "--format", "json"]
    out = subprocess.run(cmd, check=True, capture_output=True, text=True).stdout
    return json.loads(out)


def published(cfg):
    for name, svc in cfg["services"].items():
        for p in svc.get("ports") or []:
            yield name, p


def check_service(variant, name, svc, networks):
    if svc.get("read_only") is not True:
        fail(variant, f"{name}: read_only is not true")
    if "ALL" not in (svc.get("cap_drop") or []):
        fail(variant, f"{name}: cap_drop does not include ALL")
    if svc.get("cap_add"):
        fail(variant, f"{name}: cap_add {svc['cap_add']}")
    if "no-new-privileges:true" not in (svc.get("security_opt") or []):
        fail(variant, f"{name}: missing security_opt no-new-privileges:true")
    user = str(svc.get("user") or "")
    uid = user.split(":")[0]
    if not uid.isdigit() or int(uid) == 0:
        fail(variant, f"{name}: user {user!r} is not a numeric non-root UID")
    if svc.get("privileged"):
        fail(variant, f"{name}: privileged")
    for ns in ("pid", "ipc", "userns_mode", "uts"):
        if svc.get(ns) == "host":
            fail(variant, f"{name}: {ns}: host")
    if svc.get("network_mode") == "host":
        fail(variant, f"{name}: network_mode: host")
    if not svc.get("mem_limit") and not (svc.get("deploy") or {}).get("resources", {}).get("limits", {}).get("memory"):
        fail(variant, f"{name}: no memory limit")
    if not svc.get("pids_limit"):
        fail(variant, f"{name}: no pids_limit")
    log = svc.get("logging") or {}
    if log.get("driver") != "json-file" or (log.get("options") or {}).get("max-size") != "50m":
        fail(variant, f"{name}: logging is not json-file with max-size 50m")
    for v in svc.get("volumes") or []:
        src, tgt = str(v.get("source", "")), v.get("target", "")
        if "docker.sock" in src or "docker.sock" in tgt:
            fail(variant, f"{name}: mounts the Docker socket")
        if (name, tgt) in READ_ONLY_MOUNTS and not v.get("read_only"):
            fail(variant, f"{name}: {tgt} must be mounted read-only")
        if src == "/" and not v.get("read_only"):
            fail(variant, f"{name}: host / mounted read-write")
    if name in ("model", "model-check") and svc.get("network_mode") != "none":
        fail(variant, f"{name}: must have network_mode: none")
    if name == "model-check" and any(
        (v.get("target") if isinstance(v, dict) else str(v)).startswith("/run/recs")
        for v in svc.get("volumes") or []
    ):
        fail(variant, "model-check: must not mount the model socket volume")
    if name == "api":
        for net in svc.get("networks") or {}:
            if net == "frontend":
                fail(variant, "api: must not join the frontend network")
        # Security audit SA1-2: the Go runtime collects garbage harder as the heap nears
        # GOMEMLIMIT instead of growing into the container limit and being OOM-killed.
        # (nightly.sh runs export-ratings as `compose run api`, so it inherits this too.)
        env = svc.get("environment") or {}
        if env.get("GOMEMLIMIT") != "512MiB":
            fail(variant, f"api: GOMEMLIMIT is {env.get('GOMEMLIMIT')!r}, want '512MiB'")
        limit = svc.get("mem_limit")
        if limit is not None and int(limit) <= 512 * 1024 * 1024:
            fail(variant, f"api: mem_limit {limit} must exceed GOMEMLIMIT (512MiB)")
    if name == "train":
        # The tuned per-model configs (configs/*.json) must reach production training; without
        # them recs-train silently falls back to model defaults. WORKDIR is /srv, so recs-train's
        # default --config-dir (configs) resolves to /srv/configs.
        if "/srv/configs" not in {v.get("target") for v in svc.get("volumes") or []}:
            fail(variant, "train: configs not mounted at /srv/configs")
    if name in ("api", "model"):
        targets = {v.get("target") for v in svc.get("volumes") or []}
        if "/run/recs" not in targets:
            fail(variant, f"{name}: model socket volume not mounted at /run/recs")


def bind_sources(cfg):
    """Host paths bind-mounted by any service: {source: [(service, target, read_only)]}."""
    out = {}
    for name, svc in cfg["services"].items():
        for v in svc.get("volumes") or []:
            if v.get("type") == "bind":
                out.setdefault(v["source"], []).append((name, v["target"], bool(v.get("read_only"))))
    return out


def check_dev_isolated(prod, dev):
    """Dev must never touch production: own project name, own ports, own data directories."""
    variant = "dev"
    if dev.get("name") != "intermission-dev" or dev.get("name") == prod.get("name"):
        fail(variant, f"project name is {dev.get('name')!r} (prod {prod.get('name')!r}); want intermission-dev")
    prod_ports = {(p.get("host_ip"), str(p.get("published"))) for _, p in published(prod)}
    dev_ports = {(p.get("host_ip"), str(p.get("published"))) for _, p in published(dev)}
    if prod_ports & dev_ports:
        fail(variant, f"dev publishes production ports {sorted(prod_ports & dev_ports)}")
    if not any(n == "caddy" for n, _ in published(dev)):
        fail(variant, "dev caddy publishes no port")
    prod_src, dev_src = bind_sources(prod), bind_sources(dev)
    for src, uses in dev_src.items():
        if src not in prod_src:
            continue
        # Shared host paths are allowed only for read-only config (Caddyfile, rules, ...).
        if any(not ro for _, _, ro in uses) or any(not ro for _, _, ro in prod_src[src]) \
                or "/data" in src:
            fail(variant, f"dev mounts production path {src} ({uses})")
    for vol in dev.get("volumes") or {}:
        name = (dev["volumes"][vol] or {}).get("name", "")
        if name and not name.startswith("intermission-dev"):
            fail(variant, f"dev volume {vol} resolves to {name}")


def check_monitoring_networks(variant, cfg):
    """node-exporter sits only on internal networks; it never mounts the host root."""
    services, networks = cfg["services"], cfg.get("networks") or {}
    ne = services.get("node-exporter")
    if not ne:
        return
    for net in ne.get("networks") or {}:
        if (networks.get(net) or {}).get("internal") is not True:
            fail(variant, f"node-exporter joins non-internal network {net}")
    for v in ne.get("volumes") or []:
        if v.get("source") == "/":
            fail(variant, "node-exporter mounts the host root filesystem")
    prom = services.get("prometheus") or {}
    if not set(prom.get("networks") or {}) & set(ne.get("networks") or {}):
        fail(variant, "prometheus shares no network with node-exporter")


def main():
    configs = {}
    for variant, (overlays, profiles) in VARIANTS.items():
        cfg = compose_config(overlays, profiles)
        services = cfg["services"]
        for name, svc in services.items():
            check_service(variant, name, svc, cfg.get("networks") or {})
        for name, p in published(cfg):
            if p.get("host_ip") != "127.0.0.1":
                fail(variant, f"{name}: port {p} is not bound to 127.0.0.1")
        backend = (cfg.get("networks") or {}).get("backend") or {}
        if "api" in services and backend.get("internal") is not True:
            fail(variant, "network backend must be internal")
        if variant == "default":
            ports = [(n, p.get("host_ip"), str(p.get("published")), p.get("target")) for n, p in published(cfg)]
            if ports != [("caddy", "127.0.0.1", "8080", 8080)]:
                fail(variant, f"published ports are {ports}, want only caddy 127.0.0.1:8080->8080")
            if set(services) != {"caddy", "api", "model"}:
                fail(variant, f"default services are {sorted(services)}, want caddy, api, model")
        check_monitoring_networks(variant, cfg)
        configs[variant] = cfg
        print(f"checked {variant}: {len(services)} services: {', '.join(sorted(services))}")

    check_dev_isolated(configs["all-profiles"], configs["dev"])

    if errors:
        print("\nFAIL", file=sys.stderr)
        for e in errors:
            print("  " + e, file=sys.stderr)
        return 1
    print("PASS: compose hardening and port checks")
    return 0


if __name__ == "__main__":
    sys.exit(main())
