import os
from pathlib import Path
import subprocess
import tempfile
import unittest


class VersionTest(unittest.TestCase):
    def test_ci_revision_does_not_require_git_access_to_checkout(self):
        script = Path(__file__).resolve().parents[1] / "ci/version.cmake"
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "version.h"
            # No Git repository here, just as an ownership error makes Git
            # unavailable inside the Rocky CI container.
            revision = "1234567890abcdef1234567890abcdef12345678"
            subprocess.run(["cmake", f"-DSOURCE={temporary}", f"-DOUTPUT={output}",
                            "-P", str(script)], check=True,
                           env={**os.environ, "GITHUB_SHA": revision})
            self.assertEqual(output.read_text(), f'#define GMUX_VERSION "{revision}"\n')
