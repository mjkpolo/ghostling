import hashlib
import importlib.machinery
import importlib.util
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock


def load_gmuxctl():
    path = Path(__file__).resolve().parents[1] / "gmuxctl"
    loader = importlib.machinery.SourceFileLoader("gmuxctl", str(path))
    spec = importlib.util.spec_from_loader(loader.name, loader)
    module = importlib.util.module_from_spec(spec)
    loader.exec_module(module)
    return module


class ReleaseDownloadTest(unittest.TestCase):
    def test_download_is_verified_and_cached(self):
        gmuxctl = load_gmuxctl()
        with tempfile.TemporaryDirectory() as release_temp:
            with tempfile.TemporaryDirectory() as cache_temp:
                release = Path(release_temp)
                cache = Path(cache_temp)
                payload = b"published gmux binary\n"
                (release / "gmux").write_bytes(payload)
                digest = hashlib.sha256(payload).hexdigest()
                (release / "SHA256SUMS").write_text(f"{digest}  gmux\n")
                with mock.patch.object(gmuxctl, "RELEASE_BASE",
                                       release.as_uri()), \
                        mock.patch.object(gmuxctl, "cache_dir",
                                          return_value=cache), \
                        mock.patch("builtins.print") as output:
                    downloaded = gmuxctl.release_asset(
                        "gmux", executable=True)
                    self.assertEqual(downloaded.read_bytes(), payload)
                    self.assertEqual(
                        downloaded.stat().st_mode & 0o777, 0o700)
                    self.assertTrue(output.called)
                    output.reset_mock()
                    (release / "gmux").unlink()
                    self.assertEqual(
                        gmuxctl.release_asset("gmux", executable=True),
                        downloaded)
                    output.assert_not_called()

    def test_existing_remote_terminfo_is_not_copied(self):
        gmuxctl = load_gmuxctl()
        responses = [
            SimpleNamespace(stdout="/tmp/gmux-terminfo\n", returncode=0),
            SimpleNamespace(stdout="", returncode=0),
        ]
        with mock.patch.object(gmuxctl, "run_ssh",
                               side_effect=responses) as run_ssh, \
                mock.patch.object(gmuxctl, "release_asset") as download:
            gmuxctl.provision_terminfo("example")
        self.assertEqual(run_ssh.call_count, 2)
        download.assert_not_called()


if __name__ == "__main__":
    unittest.main()
