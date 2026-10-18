import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch


def module(name):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).resolve().parents[1] / f"{name}.py")
    loaded = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(loaded)
    return loaded


backup = module("full_backup")
restore = module("full_restore")


class BackupSafetyTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.infra = self.root / "infra"
        self.infra.mkdir()
        (self.infra / ".env").write_text("POSTGRES_USER=postgres\n")
        self.services = ("db", "minio", "mosquitto", "redis", "backend", "worker", "arq-worker", "frontend")
        self.containers = [{"Id": name, "Image": f"sha256:{name}", "State": {"Running": True},
                            "Config": {"Labels": {"com.docker.compose.service": name}, "Image": name},
                            "Mounts": []} for name in self.services]
        self.config = {"name": "infra", "services": {name: {"image": name} for name in self.services}}
        self.config["services"]["migrate"] = {"image": "backend"}
        self.state = self.root / "state/stopped.json"
        env = patch.dict(os.environ, {"BACKUP_STATE": str(self.state), "BACKUP_MIN_FREE_BYTES": "0"})
        env.start()
        self.addCleanup(env.stop)

    def output(self, *args):
        if "config" in args:
            return json.dumps(self.config)
        if "inspect" in args:
            return json.dumps(self.containers)
        return " ".join(self.services)

    def assert_online_failure(self, failure):
        calls = []

        def run(*args, **kwargs):
            calls.append(args)
            if failure(args):
                raise subprocess.CalledProcessError(1, args)
            return subprocess.CompletedProcess(args, 0)

        with patch.object(backup, "output", side_effect=self.output), patch.object(backup, "run", side_effect=run):
            with self.assertRaises(subprocess.CalledProcessError):
                backup.backup(self.infra, self.root / "backups", False)
        self.assertFalse(any(args[:2] in (("docker", "stop"), ("docker", "start")) for args in calls))
        self.assertFalse(self.state.exists())
        self.assertFalse(list((self.root / "backups").glob("*.success")))
        # A pruned one-shot migration container uses the captured running backend image.
        tags = [args for args in calls if args[:3] == ("docker", "image", "tag")]
        self.assertEqual(len(tags), len(self.services))

    def test_database_dump_failure_leaves_all_services_running(self):
        self.assert_online_failure(lambda args: args[:2] == ("docker", "exec"))

    def test_drive_auth_failure_does_not_stop_services(self):
        with patch.object(backup, "output", side_effect=self.output), patch.object(
            backup, "run", side_effect=subprocess.CalledProcessError(1, "upload preflight")
        ) as run:
            with self.assertRaises(subprocess.CalledProcessError):
                backup.backup(self.infra, self.root / "backups", True)
        self.assertEqual(run.call_count, 1)
        self.assertNotIn("stop", run.call_args.args)

    def test_resume_after_forced_termination_keeps_state_on_failure(self):
        self.state.parent.mkdir()
        self.state.write_text(json.dumps(["minio", "worker"]))
        with patch.object(backup, "run", side_effect=subprocess.CalledProcessError(1, "docker start")):
            with self.assertRaises(subprocess.CalledProcessError):
                backup.resume_stopped()
        self.assertTrue(self.state.exists())
        with patch.object(backup, "run") as run:
            backup.resume_stopped()
        run.assert_called_once_with("docker", "start", "minio", "worker", stdout=subprocess.DEVNULL)
        self.assertFalse(self.state.exists())

    def test_zero_retention_removes_only_drive_verified_archives(self):
        destination = self.root / "backups"
        destination.mkdir()
        verified = destination / "radiometer-full-20261013T010000Z.tar.zst"
        local_only = destination / "radiometer-full-20261013T020000Z.tar.zst"
        for archive in (verified, local_only):
            archive.write_bytes(b"archive")
            Path(str(archive) + ".sha256").write_text("checksum")
            Path(str(archive) + ".success").touch()
        Path(str(verified) + ".drive.json").write_text("[]")

        backup.prune_local_archives(destination, 0)

        self.assertFalse(verified.exists())
        self.assertFalse(Path(str(verified) + ".sha256").exists())
        self.assertFalse(Path(str(verified) + ".drive.json").exists())
        self.assertTrue(local_only.exists())
        self.assertTrue(Path(str(local_only) + ".sha256").exists())


class RestoreSafetyTests(unittest.TestCase):
    def test_existing_project_is_refused_before_any_mutation(self):
        with tempfile.TemporaryDirectory() as tmp:
            bundle = Path(tmp)
            (bundle / "infra").mkdir()
            (bundle / "manifest.json").write_text(json.dumps({"format": 1, "volumes": {}}))
            (bundle / "infra/compose.restore.json").write_text(json.dumps({"services": {}}))
            with patch.object(restore, "output", return_value="production-container"), patch.object(restore, "run") as run:
                with self.assertRaisesRegex(RuntimeError, "already has containers"):
                    restore.restore(bundle, bundle / "target", "infra", False, False)
            run.assert_not_called()
            self.assertFalse((bundle / "target").exists())

    def test_existing_directory_is_refused_before_image_load(self):
        with tempfile.TemporaryDirectory() as tmp:
            bundle = Path(tmp)
            (bundle / "infra").mkdir()
            (bundle / "manifest.json").write_text(json.dumps({"format": 1, "volumes": {}}))
            (bundle / "infra/compose.restore.json").write_text(json.dumps({"services": {}}))
            target = bundle / "target"
            target.mkdir()
            (target / "important").write_text("existing production data")
            with patch.object(restore, "output", return_value=""), patch.object(restore, "run") as run:
                with self.assertRaisesRegex(RuntimeError, "new or empty"):
                    restore.restore(bundle, target, "fresh-project", False, False)
            run.assert_not_called()
            self.assertEqual((target / "important").read_text(), "existing production data")


if __name__ == "__main__":
    unittest.main()
