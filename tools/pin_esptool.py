# PlatformIO pre-script for the ESP32-C5 projects: keeps the esptool in
# PlatformIO's Python environment (penv) at the version this platform comes
# with (its tool-esptoolpy package, 5.0.2 for pioarduino 55.03.30-2).
#
# All projects share penv, so another project (or a PlatformIO update) can
# leave a different esptool there, and this platform then uses whatever it
# finds. Before each build or upload this checks the version and, when it
# differs, installs the platform's own copy again (from the package already
# on disk, no download).
import json
import os
import subprocess

Import("env")  # noqa: F821  (provided by PlatformIO)

package = env.PioPlatform().get_package_dir("tool-esptoolpy")  # noqa: F821
python = env.subst("$PYTHONEXE")  # noqa: F821


def installed_version():
    r = subprocess.run([python, "-c", "import esptool; print(esptool.__version__)"],
                       capture_output=True, text=True)
    return r.stdout.strip() if r.returncode == 0 else None


if package and os.path.isfile(os.path.join(package, "package.json")):
    with open(os.path.join(package, "package.json"), encoding="utf-8") as f:
        wanted = json.load(f).get("version", "").split("+")[0]
    have = installed_version()
    if wanted and have != wanted:
        print(f"esptool {have or 'missing'} in PlatformIO's Python, installing {wanted} from {package}")
        ok = subprocess.run([python, "-m", "pip", "install", "--quiet", "--disable-pip-version-check",
                             package]).returncode == 0
        if not ok:  # penv made by uv may have no pip
            uv = os.path.join(os.path.dirname(python), "uv.exe" if os.name == "nt" else "uv")
            if os.path.isfile(uv):
                ok = subprocess.run([uv, "pip", "install", "--quiet", "--python", python, package]).returncode == 0
        print(f"esptool now {installed_version()}" if ok else
              f"Could not install esptool {wanted}; run: {python} -m pip install esptool=={wanted}")
