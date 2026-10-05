import concurrent.futures
import tempfile
import unittest
from pathlib import Path
from manager.session_metadata import transact


class MetadataTest(unittest.TestCase):
    def test_rename_move_preserve_identity(self):
        with tempfile.TemporaryDirectory() as root:
            data = transact(root, "folder", {"name": "Research"})
            folder = next(iter(data["folders"]))
            transact(root, "session", {"id": "abc", "name": "Terminal", "folder": folder})
            transact(root, "rename", {"id": "abc", "name": "Notes"})
            transact(root, "rename_folder", {"id": folder, "name": "Work"})
            data = transact(root, "read", {})
            self.assertEqual(data["sessions"]["abc"], {"name": "Notes", "folder": folder})
            self.assertEqual(data["folders"][folder], "Work")
            self.assertEqual(Path(root, "sessions.json").stat().st_mode & 0o777, 0o600)
            data = transact(root, "delete_folder", {"id": folder})
            self.assertEqual(data["folders"], {})
            self.assertEqual(data["sessions"]["abc"], {"name": "Notes", "folder": ""})

    def test_concurrent_updates_are_not_lost(self):
        with tempfile.TemporaryDirectory() as root:
            with concurrent.futures.ProcessPoolExecutor(4) as pool:
                futures = [pool.submit(transact, root, "folder", {"name": str(i)}) for i in range(20)]
                for future in futures:
                    future.result()
            self.assertEqual(len(transact(root, "read", {})["folders"]), 20)

    def test_invalid_name_does_not_replace_catalog(self):
        with tempfile.TemporaryDirectory() as root:
            original = transact(root, "folder", {"name": "Work"})
            with self.assertRaises(ValueError):
                transact(root, "folder", {"name": "bad\nname"})
            self.assertEqual(transact(root, "read", {}), original)
