"""Development-only cross-build. The resulting Windows engine has no WSL dependency."""
from pathlib import Path
import shutil, subprocess, tempfile, shlex
root=Path(__file__).resolve().parent
with tempfile.TemporaryDirectory(prefix="leo-build-") as tmp:
    shutil.copytree(root,tmp,dirs_exist_ok=True)
    linux=shlex.quote("/mnt/"+tmp[0].lower()+tmp[2:].replace("\\","/"))
    subprocess.run(["wsl","-d","Ubuntu","-e","bash","-c",f"cd {linux} && x86_64-w64-mingw32-g++ -std=c++17 -O2 -static -Ivendor main.cpp vendor/Global/Utilities/libnorad/cOrbitA.cc vendor/Global/Utilities/libnorad/cNoradBaseA.cc vendor/Global/Utilities/libnorad/cNoradSGP4A.cc vendor/Global/Utilities/libnorad/cEci.cc vendor/Global/Utilities/libnorad/cJulian.cc vendor/Global/Utilities/libnorad/cSite.cc vendor/Global/Utilities/libnorad/cVector.cc vendor/Global/Utilities/libnorad/ccoord.cc vendor/Global/Utilities/libnorad/globals.cc vendor/Global/Utilities/libnorad/cEcef.cc -o engine.exe"],check=True)
    shutil.copy2(Path(tmp)/"engine.exe",root/"engine.exe")
    subprocess.run(["wsl","-d","Ubuntu","-e","bash","-c",f"cd {linux} && x86_64-w64-mingw32-g++ -std=c++17 -O2 -static test_algorithms.cpp -o test_algorithms.exe"],check=True)
    subprocess.run([str(Path(tmp)/"test_algorithms.exe")],check=True)
