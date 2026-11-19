# SPDX-License-Identifier: GPL-3.0-only
# Modified/adapted 2026-11-16; see THIRD_PARTY_NOTICES.txt.
from pathlib import Path
import tempfile
import unittest
import build_release


class ReleaseTests(unittest.TestCase):
    def test_source_licenses_and_settings_are_included_without_user_results(self):
        with tempfile.TemporaryDirectory() as tmp:
            package = build_release.stage(Path(tmp) / "delivery", build_release.ROOT / "sim-ui.exe")
            self.assertEqual({p.name for p in (package / "data").iterdir()}, {"earth.png"})
            for file in ("LICENSE", "THIRD_PARTY_NOTICES.txt", "licenses/LGPL-3.0.txt",
                         "licenses/Microsoft-WebView2-1.0.3856.49.txt", "engine/main.cpp",
                         "engine/algorithms.h", "engine/vendor/json.hpp", "server.py", "sim-ui.spec",
                         "manual/index.html",
                         "manual/assets/overview.png"):
                self.assertTrue((package / file).is_file(), file)
            ini = "settings/simulations/Network/omnetpp.ini"
            self.assertEqual((package / ini).read_bytes(), (build_release.ROOT / ini).read_bytes())
            self.assertFalse((package / ".git").exists())
            self.assertFalse((package / "build").exists())
            self.assertFalse((package / "dist").exists())
            with self.assertRaises(FileExistsError):
                build_release.stage(package, build_release.ROOT / "sim-ui.exe")


if __name__ == "__main__":
    unittest.main()
