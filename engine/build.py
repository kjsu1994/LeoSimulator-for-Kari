"""Development-only cross-build. The resulting Windows engine has no WSL dependency."""
from pathlib import Path
import shutil, subprocess, tempfile, shlex, hashlib, json, sys
root=Path(__file__).resolve().parent
with tempfile.TemporaryDirectory(prefix="leo-build-") as tmp:
    shutil.copytree(root,tmp,dirs_exist_ok=True)
    source_hashes={p.relative_to(Path(tmp)).as_posix():hashlib.sha256(p.read_bytes()).hexdigest() for p in Path(tmp).rglob('*') if p.is_file() and p.suffix in ('.h','.cpp','.cc')}
    linux=shlex.quote("/mnt/"+tmp[0].lower()+tmp[2:].replace("\\","/"))
    subprocess.run(["wsl","-d","Ubuntu","-e","bash","-c",f"cd {linux} && x86_64-w64-mingw32-g++ -std=gnu++17 -O2 -static -Ivendor main.cpp vendor/Global/Utilities/libnorad/cOrbitA.cc vendor/Global/Utilities/libnorad/cNoradBaseA.cc vendor/Global/Utilities/libnorad/cNoradSGP4A.cc vendor/Global/Utilities/libnorad/cEci.cc vendor/Global/Utilities/libnorad/cJulian.cc vendor/Global/Utilities/libnorad/cSite.cc vendor/Global/Utilities/libnorad/cVector.cc vendor/Global/Utilities/libnorad/ccoord.cc vendor/Global/Utilities/libnorad/globals.cc vendor/Global/Utilities/libnorad/cEcef.cc -o engine.exe"],check=True,cwd="C:/")
    shutil.copy2(Path(tmp)/"engine.exe",root/"engine.exe")
    subprocess.run(["wsl","-d","Ubuntu","-e","bash","-c",f"cd {linux} && x86_64-w64-mingw32-g++ -std=gnu++17 -O2 -static test_algorithms.cpp -o test_algorithms.exe"],check=True,cwd="C:/")
    subprocess.run([str(Path(tmp)/"test_algorithms.exe")],check=True)
    shutil.copy2(Path(tmp)/"test_algorithms.exe",root/"test_algorithms.exe")

    subprocess.run(["wsl","-d","Ubuntu","-e","bash","-c",f"cd {linux} && x86_64-w64-mingw32-g++ -std=gnu++17 -O2 -static test_control.cpp -o test_control.exe"],check=True,cwd="C:/")
    subprocess.run([str(Path(tmp)/"test_control.exe")],check=True)
    shutil.copy2(Path(tmp)/"test_control.exe",root/"test_control.exe")

    # Use the same orbit translation units as the engine, not unrelated vendored tools.
    compile_engine="x86_64-w64-mingw32-g++ -std=gnu++17 -O2 -static -Ivendor tests/test_kernel.cpp "+" ".join('vendor/Global/Utilities/libnorad/'+name for name in ['cOrbitA.cc','cNoradBaseA.cc','cNoradSGP4A.cc','cEci.cc','cJulian.cc','cSite.cc','cVector.cc','ccoord.cc','globals.cc','cEcef.cc'])+" -o test_kernel.exe"
    subprocess.run(['wsl','-d','Ubuntu','-e','bash','-c',f'cd {linux} && {compile_engine}'],check=True,cwd='C:/')
    subprocess.run([str(Path(tmp)/'test_kernel.exe'),tmp],check=True)
    for name in ('test_kernel','oracle_driver'):
        if name=='oracle_driver':
            command=f'cd {linux} && x86_64-w64-mingw32-g++ -std=gnu++17 -O2 -static tests/oracle_driver.cpp -o oracle_driver.exe'
            subprocess.run(['wsl','-d','Ubuntu','-e','bash','-c',command],check=True,cwd='C:/')
        shutil.copy2(Path(tmp)/(name+'.exe'),root/(name+'.exe'))
    for source in ([] if '--engine-only' in sys.argv else sorted((root/'tests/reference').glob('*.cpp'))):
        name=source.stem
        command=f"cd {linux} && x86_64-w64-mingw32-g++ -std=gnu++17 -O2 -static -Itests/include -Ivendor/routing tests/reference/{source.name} -o {name}.exe"
        subprocess.run(['wsl','-d','Ubuntu','-e','bash','-c',command],check=True,cwd='C:/')
        subprocess.run([str(Path(tmp)/(name+'.exe'))],check=True)
        shutil.copy2(Path(tmp)/(name+'.exe'),root/(name+'.exe'))
(root/'build-info.json').write_text(json.dumps({'version':'1.0.0','compiler':'x86_64-w64-mingw32-g++ 13-win32','sources':source_hashes,'sha256':hashlib.sha256((root/'engine.exe').read_bytes()).hexdigest()},indent=2),encoding='utf-8')
