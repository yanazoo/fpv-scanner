"""Merges a built PlatformIO environment into one image to write at 0x0.

    python tools/merge_firmware.py <project dir> <env> <chip> <output.bin>

The bootloader, partition table and boot_app0 come from PlatformIO's project
metadata (their offsets differ per chip), the program goes to 0x10000.
Needs a finished build and esptool (pip install esptool).
"""
import json
import os
import subprocess
import sys

project, env, chip, output = sys.argv[1:5]
meta = subprocess.run(["pio", "project", "metadata", "-d", project, "-e", env, "--json-output"],
                      check=True, capture_output=True, text=True).stdout
data = json.loads(meta[meta.index("{"):])[env]
extra = data.get("extra", {})
firmware = os.path.join(os.path.dirname(data["prog_path"]), "firmware.bin")
parts = []
for image in extra.get("flash_images", []):
    parts += [image["offset"], image["path"]]
parts += [extra.get("application_offset", "0x10000"), firmware]
cmd = [sys.executable, "-m", "esptool", "--chip", chip, "merge-bin", "-o", output, *parts]
print(" ".join(cmd), flush=True)
subprocess.run(cmd, check=True)
