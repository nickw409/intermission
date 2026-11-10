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
    recs-model has no network; read-only mounts stay read-only.

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
    "all-profiles": ([], ["jobs", "monitoring"]),
    "dev": (["compose.dev.yaml"], []),
    "drill": (["compose.drill.yaml"], ["jobs"]),
}

# Mounts that must stay read-only: (service, container target).
READ_ONLY_MOUNTS = {
    ("api", "/srv/data/reports"),
    ("model", "/srv/data/artifacts"),
    ("train", "/srv/data/raw"),
    ("train", "/srv/data/exports"),
    ("prometheus", "/etc/prometheus/prometheus.yml"),
    ("prometheus", "/etc/prometheus/alerts"),
    ("node-exporter", "/host"),
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
    if name == "model" and svc.get("network_mode") != "none":
        fail(variant, "model: must have network_mode: none (Unix socket only)")
    if name == "api":
        for net in svc.get("networks") or {}:
            if net == "frontend":
                fail(variant, "api: must not join the frontend network")
    if name in ("api", "model"):
        targets = {v.get("target") for v in svc.get("volumes") or []}
        if "/run/recs" not in targets:
            fail(variant, f"{name}: model socket volume not mounted at /run/recs")


def main():
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
        print(f"checked {variant}: {len(services)} services: {', '.join(sorted(services))}")

    if errors:
        print("\nFAIL", file=sys.stderr)
        for e in errors:
            print("  " + e, file=sys.stderr)
        return 1
    print("PASS: compose hardening and port checks")
    return 0


if __name__ == "__main__":
    sys.exit(main())
