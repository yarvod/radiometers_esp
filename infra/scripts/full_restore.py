#!/usr/bin/env python3
"""Restore an extracted full backup into a fresh Compose project, never over existing data."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import time


def run(*args, **kwargs):
    return subprocess.run(args, check=True, **kwargs)


def output(*args):
    return run(*args, stdout=subprocess.PIPE, text=True).stdout.strip()


def wait_database(container):
    for _ in range(90):
        result = subprocess.run(["docker", "exec", container, "sh", "-ec",
                                 'pg_isready -U "$POSTGRES_USER" -d "$POSTGRES_DB"'],
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if result.returncode == 0:
            return
        time.sleep(1)
    raise RuntimeError("Restored PostgreSQL did not become ready within 90 seconds")


def restore(bundle, target, project, isolated, host_config):
    if isolated and host_config:
        raise RuntimeError("Isolated validation must not install host configuration")
    manifest = json.loads((bundle / "manifest.json").read_text())
    if manifest.get("format") != 1:
        raise RuntimeError("Unsupported backup format")
    configuration = json.loads((bundle / "infra/compose.restore.json").read_text())
    configuration["name"] = project
    # Do not collide with running containers or restore over any existing volume.
    existing = output("docker", "ps", "--all", "--quiet", "--filter", f"label=com.docker.compose.project={project}")
    if existing:
        raise RuntimeError(f"Compose project {project} already has containers; choose a fresh project")
    for key, spec in configuration.get("volumes", {}).items():
        if spec.get("external"):
            raise RuntimeError("External volumes require manual restoration")
        spec["name"] = f"{project}_{key}"
        if subprocess.run(["docker", "volume", "inspect", spec["name"]],
                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0:
            raise RuntimeError(f"Refusing existing Docker volume: {spec['name']}")
    for spec in configuration["services"].values():
        if "container_name" in spec:
            raise RuntimeError("Explicit container_name requires manual restoration")
        if isolated:
            spec.pop("ports", None)
    for key, spec in configuration.get("networks", {}).items():
        if spec.get("external"):
            raise RuntimeError("External networks require manual restoration")
        spec["name"] = f"{project}_{key}"
        if isolated:
            spec["internal"] = True
    if target.exists() and any(target.iterdir()):
        raise RuntimeError(f"Target directory must be new or empty: {target}")
    # Validate every input before loading images or starting data services.
    for path in (bundle / "images.tar", bundle / "postgres/database.dump"):
        if not path.is_file() or path.stat().st_size == 0:
            raise RuntimeError(f"Missing or empty backup input: {path.name}")
    for mount in manifest["volumes"].values():
        name = mount["archive"]
        if Path(name).name != name or not (bundle / "volumes" / name).is_file():
            raise RuntimeError("Invalid or missing volume archive")
    if host_config:
        install_paths = [p for p in manifest["host_paths"] if p.startswith((
            "/etc/nginx/conf.d/", "/etc/letsencrypt/live/", "/etc/letsencrypt/archive/",
            "/etc/letsencrypt/renewal/"))]
        for path in install_paths:
            if not path.startswith(("/etc/nginx/", "/etc/letsencrypt/")):
                raise RuntimeError("Unexpected host configuration path")
            if Path(path).exists():
                raise RuntimeError(f"Host configuration exists; install manually: {path}")
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copytree(bundle / "infra", target, dirs_exist_ok=True, symlinks=True)
    # Absolute mounts in the original deployment are already made relative by backup.
    restore_config = target / "compose.restore.json"
    restore_config.write_text(json.dumps(configuration, indent=2) + "\n")
    run("docker", "image", "load", "--input", str(bundle / "images.tar"))
    compose = ["docker", "compose", "--project-directory", str(target), "--project-name", project,
               "-f", str(restore_config)]
    run(*compose, "create", "--pull", "never")
    created = output(*compose, "ps", "--all", "--quiet").split()
    containers = json.loads(output("docker", "inspect", *created))
    deployed = {c["Config"]["Labels"]["com.docker.compose.service"]: c for c in containers}
    restored_volumes = set()
    for saved in manifest["volumes"].values():
        for usage in saved["targets"]:
            container = deployed[usage["service"]]
            matches = [m for m in container["Mounts"] if m["Destination"] == usage["target"] and m["Type"] == "volume"]
            if len(matches) != 1:
                raise RuntimeError(f"Cannot locate restored mount: {usage}")
            mount = matches[0]
            if mount["Name"] in restored_volumes:
                continue
            restored_volumes.add(mount["Name"])
            run("tar", "--numeric-owner", "-C", mount["Source"], "-xf", str(bundle / "volumes" / saved["archive"]))
    run("docker", "start", deployed["db"]["Id"], stdout=subprocess.DEVNULL)
    wait_database(deployed["db"]["Id"])
    with (bundle / "postgres/database.dump").open("rb") as stream:
        run("docker", "exec", "-i", deployed["db"]["Id"], "sh", "-ec",
            'pg_restore -U "$POSTGRES_USER" -d "$POSTGRES_DB" --no-owner --no-acl --exit-on-error', stdin=stream)
    run(*compose, "up", "-d", "--pull", "never")
    if host_config:
        for path in install_paths:
            source = bundle / "host" / path.lstrip("/")
            dest = Path(path)
            dest.parent.mkdir(parents=True, exist_ok=True)
            if source.is_dir():
                shutil.copytree(source, dest, symlinks=True)
            else:
                shutil.copy2(source, dest)
        # Standard Certbot support files can be reused if already installed.
        for name in ("options-ssl-nginx.conf", "ssl-dhparams.pem"):
            source = bundle / "host/etc/letsencrypt" / name
            dest = Path("/etc/letsencrypt") / name
            if source.is_file() and not dest.exists():
                shutil.copy2(source, dest)
        run("nginx", "-t")
        run("systemctl", "reload", "nginx")
    print(f"Restored project {project} in {target}", flush=True)
    print("Inspect service readiness with docker compose -f compose.restore.json ps.", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bundle", type=Path)
    parser.add_argument("--target", required=True, type=Path)
    parser.add_argument("--project", default="infra")
    parser.add_argument("--isolated", action="store_true", help="No published ports or external network access")
    parser.add_argument("--host-config", action="store_true", help="Install Nginx/TLS only where paths do not exist")
    args = parser.parse_args()
    if os.geteuid() != 0:
        parser.error("Run as root to preserve volume ownership")
    os.umask(0o077)
    here = Path(__file__).resolve().parent
    bundle = args.bundle or (here if (here / "manifest.json").exists() else here.parent / "bundle")
    restore(bundle.resolve(), args.target.resolve(), args.project, args.isolated, args.host_config)


if __name__ == "__main__":
    main()
