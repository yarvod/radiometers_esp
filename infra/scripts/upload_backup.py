#!/usr/bin/env python3
"""Upload full backups through a private, server-side rclone Google Drive remote."""
import argparse
import json
import os
from pathlib import Path
import subprocess


def run(*args, **kwargs):
    return subprocess.run(args, check=True, **kwargs)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("check", "upload"))
    parser.add_argument("files", nargs="*", type=Path)
    args = parser.parse_args()
    remote = os.environ.get("BACKUP_RCLONE_REMOTE", "radiometer-gdrive:radiometer-backups")
    config = Path(os.environ.get("RCLONE_CONFIG", str(Path(__file__).resolve().parent.parent / "backup-secrets" / "rclone.conf")))
    if not config.is_file():
        parser.error("Google Drive is not connected: rclone config is missing")
    base = ["rclone", "--config", str(config), "--retries", "5", "--low-level-retries", "10",
            "--contimeout", "30s", "--timeout", "5m"]
    # Check authentication without requesting Drive-wide about/storage permissions.
    run(*base, "lsf", remote.split(":", 1)[0] + ":", "--max-depth", "1", stdout=subprocess.DEVNULL)
    if args.action == "check":
        return
    if not args.files:
        parser.error("upload requires at least one file")
    run(*base, "mkdir", remote)
    uploaded = []
    for path in args.files:
        if not path.is_file():
            parser.error(f"Missing archive/checksum: {path}")
        target = remote.rstrip("/") + "/" + path.name
        run(*base, "copyto", str(path), target, "--checksum", "--drive-chunk-size", "32M")
        # Google Drive MD5 + length readback: no success marker until verified.
        result = run(*base, "lsjson", target, "--stat", "--hash", stdout=subprocess.PIPE, text=True)
        metadata = json.loads(result.stdout)
        import hashlib
        digest = hashlib.md5()
        with path.open("rb") as stream:
            for block in iter(lambda: stream.read(8 * 1024 * 1024), b""):
                digest.update(block)
        if metadata.get("Size") != path.stat().st_size or metadata.get("Hashes", {}).get("MD5", "").lower() != digest.hexdigest():
            raise RuntimeError(f"Google Drive size/hash verification failed for {path.name}")
        uploaded.append({"name": path.name, "id": metadata.get("ID"), "bytes": metadata["Size"]})
        print(f"Google Drive verified: {path.name} ({metadata['Size']} bytes)", flush=True)
    receipt = Path(str(args.files[0]) + ".drive.json")
    receipt.write_text(json.dumps(uploaded, indent=2) + "\n")
    receipt.chmod(0o600)


if __name__ == "__main__":
    main()
