#!/usr/bin/env python3
"""Portable, consistent backup of a radiometer Compose stack (Linux/root)."""
import argparse
import datetime as dt
import fcntl
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import tempfile

SCRIPT_DIR = Path(__file__).resolve().parent
DEFAULT_STATE_DIR = SCRIPT_DIR.parent / "backups" / ".state"


def log(message):
    print(f"[{dt.datetime.now(dt.timezone.utc).isoformat()}] {message}", flush=True)


def run(*args, **kwargs):
    return subprocess.run(args, check=True, **kwargs)


def output(*args):
    return run(*args, stdout=subprocess.PIPE, text=True).stdout.strip()


def checksum(path, algorithm="sha256"):
    digest = hashlib.new(algorithm)
    with open(path, "rb") as stream:
        for chunk in iter(lambda: stream.read(8 * 1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def resume_stopped():
    state = Path(os.environ.get("BACKUP_STATE", str(DEFAULT_STATE_DIR / "stopped.json")))
    if state.exists():
        ids = json.loads(state.read_text())
        if ids:
            run("docker", "start", *ids, stdout=subprocess.DEVNULL)
        state.unlink()


def backup(infra, destination, upload):
    infra, destination = infra.resolve(), destination.resolve()
    destination.mkdir(parents=True, exist_ok=True, mode=0o700)
    compose_file = os.environ.get("COMPOSE_FILE", "compose-prod.yml")
    compose = ["docker", "compose", "--project-directory", str(infra),
               "--env-file", str(infra / ".env"), "-f", str(infra / compose_file)]
    configuration = json.loads(output(*compose, "config", "--format", "json"))
    container_ids = output(*compose, "ps", "--all", "--quiet").split()
    if not container_ids:
        raise RuntimeError("No deployed Compose containers found")
    containers = json.loads(output("docker", "inspect", *container_ids))
    deployed = {c["Config"]["Labels"]["com.docker.compose.service"]: c for c in containers}
    for service in ("db", "minio", "mosquitto", "redis", "backend", "worker", "arq-worker", "frontend"):
        if service not in deployed:
            raise RuntimeError(f"Missing required deployed service: {service}")
    if not deployed["db"]["State"]["Running"]:
        raise RuntimeError("PostgreSQL is not running")
    # A dedicated upload check happens before interrupting any service.
    if upload:
        run("python3", str(SCRIPT_DIR / "upload_backup.py"), "check")
    available = shutil.disk_usage(destination).free
    if available < int(os.environ.get("BACKUP_MIN_FREE_BYTES", str(4 * 1024**3))):
        raise RuntimeError("Less than the configured minimum free space for a full backup")
    stamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    archive = destination / f"radiometer-full-{stamp}.tar.zst"
    stopped, tags = [], []
    state = Path(os.environ.get("BACKUP_STATE", str(DEFAULT_STATE_DIR / "stopped.json")))
    state.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    resume_stopped()
    snapshot_started = None
    with tempfile.TemporaryDirectory(prefix=".radiometer-stage-", dir=destination) as tmp:
        work = Path(tmp)
        bundle_infra = work / "infra"
        bundle_infra.mkdir(mode=0o700)
        (work / "postgres").mkdir()
        (work / "volumes").mkdir()
        # Capture configuration, credentials and scripts without source/build trees.
        for entry in infra.iterdir():
            if entry.is_file() and entry.suffix not in (".log",):
                shutil.copy2(entry, bundle_infra / entry.name)
        for directory in ("scripts", "certs", "mosquitto_passwd", "backup-secrets"):
            source = infra / directory
            if source.exists():
                shutil.copytree(source, bundle_infra / directory, symlinks=True,
                                ignore=shutil.ignore_patterns("__pycache__", "*.pyc"))
        (bundle_infra / "scripts").mkdir(exist_ok=True)
        for name in ("full_backup.py", "full_restore.py", "upload_backup.py"):
            shutil.copy2(SCRIPT_DIR / name, bundle_infra / "scripts" / name)
        shutil.copy2(SCRIPT_DIR / "full_restore.py", work / "restore.py")
        if (SCRIPT_DIR.parent / "BACKUPS.md").is_file():
            shutil.copy2(SCRIPT_DIR.parent / "BACKUPS.md", bundle_infra / "BACKUPS.md")
        elif (SCRIPT_DIR / "BACKUPS.md").is_file():
            shutil.copy2(SCRIPT_DIR / "BACKUPS.md", bundle_infra / "BACKUPS.md")
        host_paths = ["/etc/nginx/nginx.conf", "/etc/nginx/mime.types", "/etc/nginx/snippets",
                      "/etc/letsencrypt/options-ssl-nginx.conf", "/etc/letsencrypt/ssl-dhparams.pem"]
        for name in ("esp.conf", "s3-api.conf", "s3-console.conf"):
            host_paths.append(f"/etc/nginx/conf.d/{name}")
        for domain in os.environ.get("BACKUP_TLS_DOMAINS", "yoowee.ru s3.yoowee.ru s3-console.yoowee.ru").split():
            for directory in ("live", "archive"):
                host_paths.append(f"/etc/letsencrypt/{directory}/{domain}")
            host_paths.append(f"/etc/letsencrypt/renewal/{domain}.conf")
        included_host_paths = []
        for path in host_paths:
            source = Path(path)
            if not source.exists():
                continue
            target = work / "host" / source.relative_to("/")
            target.parent.mkdir(parents=True, exist_ok=True)
            if source.is_dir():
                shutil.copytree(source, target, symlinks=True)
            else:
                shutil.copy2(source, target)
            included_host_paths.append(path)
        mounts, images = {}, {}
        try:
            # Save exact deployed images, including the one-shot migration service.
            for service, spec in configuration["services"].items():
                if service in deployed:
                    image_id = deployed[service]["Image"]
                else:
                    # A completed migration container may have been pruned. Reuse
                    # the exact running backend image when its configured image matches.
                    matching = [c["Image"] for c in containers if c["Config"]["Image"] == spec["image"]]
                    if not matching:
                        raise RuntimeError(f"Cannot capture deployed image for {service}")
                    image_id = matching[0]
                if image_id not in images:
                    tag = f"radiometer-backup/{service}:{stamp.lower()}"
                    run("docker", "image", "tag", image_id, tag)
                    tags.append(tag)
                    images[image_id] = tag
                spec["image"] = images[image_id]
                spec.pop("build", None)
                for mount in spec.get("volumes", []):
                    if mount["type"] == "bind":
                        original = Path(mount["source"])
                        if not original.is_relative_to(infra):
                            raise RuntimeError(f"Bind mount outside infra requires explicit support: {original}")
                        mount["source"] = "./" + str(original.relative_to(infra))
                for mount in deployed.get(service, {}).get("Mounts", []):
                    if mount["Type"] != "volume" or service == "db":
                        continue
                    key = mount["Name"]
                    if key not in mounts:
                        mounts[key] = {"archive": f"volume-{len(mounts)}.tar", "source": mount["Source"],
                                       "targets": []}
                    mounts[key]["targets"].append({"service": service, "target": mount["Destination"]})
            (bundle_infra / "compose.restore.json").write_text(json.dumps(configuration, indent=2) + "\n")
            log("Saving exact Docker images while services are online")
            run("docker", "image", "save", "--output", str(work / "images.tar"), *tags)
            writers = [deployed[s]["Id"] for s in ("backend", "worker", "arq-worker", "mosquitto", "minio", "redis")
                       if deployed[s]["State"]["Running"]]
            snapshot_started = dt.datetime.now(dt.timezone.utc)
            # Record every running container before stop, so errors also resume it.
            stopped.extend(writers)
            state.write_text(json.dumps(writers) + "\n")
            log("Stopping radiometer writers for a consistent snapshot")
            run("docker", "stop", "--time", "45", *writers, stdout=subprocess.DEVNULL)
            log("Dumping PostgreSQL")
            for filename, command in (
                ("globals.sql", 'pg_dumpall -U "$POSTGRES_USER" --globals-only'),
                ("database.dump", 'pg_dump -U "$POSTGRES_USER" -d "$POSTGRES_DB" -Fc --no-owner --no-acl'),
            ):
                with (work / "postgres" / filename).open("wb") as stream:
                    run("docker", "exec", deployed["db"]["Id"], "sh", "-ec", command, stdout=stream)
            log("Saving complete MinIO, MQTT, Redis and export volumes")
            for mount in mounts.values():
                run("tar", "--numeric-owner", "-C", mount["source"], "-cf",
                    str(work / "volumes" / mount["archive"]), ".")
        finally:
            if stopped:
                log("Resuming radiometer services")
                # Dependencies first. Start the same containers, without changing images/migrations.
                start_order = ("minio", "redis", "mosquitto", "backend", "worker", "arq-worker")
                errors = []
                for service in start_order:
                    cid = deployed[service]["Id"]
                    if cid in stopped:
                        try:
                            run("docker", "start", cid, stdout=subprocess.DEVNULL)
                        except subprocess.CalledProcessError as exc:
                            errors.append(exc)
                if errors:
                    raise RuntimeError("Failed to resume one or more radiometer services") from errors[0]
                state.unlink(missing_ok=True)
            for tag in tags:
                run("docker", "image", "rm", tag, stdout=subprocess.DEVNULL)
        downtime = (dt.datetime.now(dt.timezone.utc) - snapshot_started).total_seconds()
        log(f"Snapshot complete; service interruption {downtime:.1f}s")
        for mount in mounts.values():
            mount.pop("source")
        manifest = {"format": 1, "created_utc": stamp, "project": configuration["name"],
                    "source_infra": str(infra), "volumes": mounts, "host_paths": included_host_paths,
                    "interruption_seconds": downtime}
        (work / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
        (work / "RESTORE.txt").write_text(
            "Install Docker Compose v2, Python >=3.10 and zstd on Linux.\n"
            "Verify SHA256, then: tar --zstd -xf ARCHIVE -C EMPTY_DIRECTORY\n"
            "sudo python3 EMPTY_DIRECTORY/restore.py --target /home/devuser/radiometers_esp/infra\n"
            "Add --host-config to install saved Nginx/TLS files on a fresh server.\n"
            "Existing target directories and Docker volumes are refused.\n"
            "See infra/BACKUPS.md for DNS, TLS renewal and backup reauthorization.\n"
            "Archive contains production data, passwords and private TLS keys. Keep private.\n")
        log("Compressing archive with services online")
        partial = Path(str(archive) + ".partial")
        try:
            run("tar", "-C", str(work), "-I", "zstd -T2 -3", "-cf", str(partial), ".")
            run("zstd", "--test", "--quiet", str(partial))
            partial.rename(archive)
        finally:
            partial.unlink(missing_ok=True)
    digest_file = Path(str(archive) + ".sha256")
    digest_file.write_text(f"{checksum(archive)}  {archive.name}\n")
    log(f"Archive created: {archive.name} ({archive.stat().st_size} bytes)")
    if upload:
        run("python3", str(SCRIPT_DIR / "upload_backup.py"), "upload", str(archive), str(digest_file))
    # Retention only removes successful copies. Failed/unuploaded copies remain.
    success = Path(str(archive) + ".success")
    success.touch(mode=0o600)
    status = {"created_utc": stamp, "archive": str(archive), "bytes": archive.stat().st_size,
              "google_drive_verified": upload, "interruption_seconds": downtime}
    status_path = state.parent / "last-success.json"
    temporary_status = status_path.with_suffix(".tmp")
    temporary_status.write_text(json.dumps(status, indent=2) + "\n")
    temporary_status.replace(status_path)
    keep = int(os.environ.get("BACKUP_LOCAL_KEEP", "2"))
    if keep < 1:
        raise RuntimeError("BACKUP_LOCAL_KEEP must be at least 1")
    completed = sorted(destination.glob("radiometer-full-*.tar.zst.success"), reverse=True)
    for marker in completed[keep:]:
        old = Path(str(marker).removesuffix(".success"))
        old.unlink(missing_ok=True)
        Path(str(old) + ".sha256").unlink(missing_ok=True)
        Path(str(old) + ".drive.json").unlink(missing_ok=True)
        marker.unlink()
    log("Full backup completed successfully")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--infra", type=Path, default=Path(__file__).resolve().parent.parent)
    parser.add_argument("--destination", type=Path, default=Path(os.environ.get("BACKUP_DIR", str(SCRIPT_DIR.parent / "backups"))))
    parser.add_argument("--no-upload", action="store_true", help="Explicit local-only run")
    parser.add_argument("--resume-only", action="store_true", help="Resume writers after a terminated backup")
    args = parser.parse_args()
    os.umask(0o077)
    if os.geteuid() != 0:
        parser.error("Run as root to capture volume ownership and TLS credentials")
    signal.signal(signal.SIGTERM, lambda *_: (_ for _ in ()).throw(KeyboardInterrupt()))
    lock_path = Path(os.environ.get("BACKUP_LOCK", str(DEFAULT_STATE_DIR / "backup.lock")))
    lock_path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    with lock_path.open("w") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        if args.resume_only:
            resume_stopped()
        else:
            backup(args.infra, args.destination, not args.no_upload)


if __name__ == "__main__":
    main()
