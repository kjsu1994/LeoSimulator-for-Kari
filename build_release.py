# SPDX-License-Identifier: GPL-3.0-only
# Modified/adapted 2026-11-16; see THIRD_PARTY_NOTICES.txt.
"""Build and stage a source-inclusive Windows delivery; never copy user runs."""
from datetime import datetime, timezone
from pathlib import Path
from unittest.mock import patch
import shutil
import subprocess
import sys
import uuid

ROOT = Path(__file__).resolve().parent
SOURCE_FILES = sorted({p.name for pattern in ("*.py", "*.js", "*.html", "*.css", "*.bat") for p in ROOT.glob(pattern)} | {
    "README.md", "REPLAY_NOTES.md", "replay-model.json", "LICENSE",
    "THIRD_PARTY_NOTICES.txt", "requirements-build.txt", "sim-ui.spec",
})


def stage(destination, executable):
    destination = Path(destination)
    destination.mkdir(parents=True, exist_ok=False)
    for name in SOURCE_FILES:
        shutil.copy2(ROOT / name, destination / name)
    for name in ("engine", "settings", "licenses", "testdata", "source-dependencies"):
        shutil.copytree(ROOT / name, destination / name,
                        ignore=shutil.ignore_patterns("__pycache__", "*.o", "*.obj"))
    shutil.copy2(executable, destination / "sim-ui.exe")
    (destination / "data").mkdir()
    shutil.copy2(ROOT / "data/earth.png", destination / "data/earth.png")
    shutil.copy2(ROOT / "testdata/bundle/routes.csv", destination / "data/routes.csv")
    return destination


def main():
    env = ROOT / "build/release-env"
    python = env / "Scripts/python.exe"
    if not python.exists():
        subprocess.run([sys.executable, "-m", "venv", str(env)], check=True)
    subprocess.run([str(python), "-m", "pip", "install", "--disable-pip-version-check",
                    "-r", str(ROOT / "requirements-build.txt")], check=True)
    subprocess.run([str(python), str(ROOT / "engine/build.py")], check=True)
    binaries = ROOT / "dist/binaries"
    subprocess.run([str(python), "-m", "PyInstaller", "--noconfirm", "--clean",
                    "--distpath", str(binaries), "--workpath", str(ROOT / "build/release-pyinstaller"),
                    str(ROOT / "sim-ui.spec")], check=True, cwd=ROOT)
    name = "LEO-Network-Simulator-" + datetime.now(timezone.utc).strftime("%Y%m%d-%H%M%S") + "-" + uuid.uuid4().hex[:6]
    delivery = stage(ROOT / "dist" / name, binaries / "sim-ui.exe")
    import server
    with patch.object(server, "BASE_DIR", str(delivery)), patch.object(server, "LEO_DIR", str(delivery / "settings")), patch.object(server, "INI_PATH", str(delivery / "settings/simulations/Dijkstra132/omnetpp.ini")):
        server.run_simulation("Dijkstra132")
    archive = shutil.make_archive(str(delivery), "zip", delivery.parent, delivery.name)
    print("DELIVERY:", delivery)
    print("ARCHIVE:", archive)


if __name__ == "__main__":
    main()
