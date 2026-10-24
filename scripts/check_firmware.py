#!/usr/bin/env python3
"""Offline firmware checks and optional Nano compile; never uploads or sends RF."""
from __future__ import annotations

import argparse
import json
import re
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "src" / "firmware"


def run(arguments: list[str]) -> None:
    print("+ " + " ".join(map(str, arguments)), flush=True)
    subprocess.run(list(map(str, arguments)), cwd=ROOT, check=True)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--arduino-cli", default=shutil.which("arduino-cli"))
    parser.add_argument("--build", action="store_true", help="also compile for the Nano ESP32")
    parser.add_argument("--output-dir", type=Path, help="required for --build; keep outside src/")
    parser.add_argument("--asan", action="store_true", help="add AddressSanitizer (may need execution outside sandbox)")
    args = parser.parse_args()
    if not args.arduino_cli:
        parser.error("locate Arduino CLI and pass --arduino-cli")
    if not shutil.which("g++") or not shutil.which("node"):
        parser.error("native tests require g++ and node")
    lock = json.loads((SOURCE / "dependencies.json").read_text())
    libraries = json.loads(subprocess.check_output([args.arduino_cli, "lib", "list", "--format", "json"]))
    installed = {entry["library"]["name"]: entry["library"] for entry in libraries["installed_libraries"]}
    for name, version in lock["libraries"].items():
        if installed.get(name, {}).get("version") != version:
            parser.error(f"install pinned dependency {name}@{version}")
    include = installed["ArduinoJson"]["source_dir"]
    if args.build:
        if not args.output_dir:
            parser.error("--build requires --output-dir")
        output = args.output_dir.resolve()
        if output == SOURCE or SOURCE in output.parents:
            parser.error("build output must be outside src/firmware")
        platforms = json.loads(subprocess.check_output([args.arduino_cli, "core", "list", "--format", "json"]))
        versions = {entry["id"]: entry.get("installed_version") for entry in platforms["platforms"]}
        if versions.get(lock["core"]) != lock["core_version"]:
            parser.error(f"install {lock['core']}@{lock['core_version']} from {lock['index_url']}")
    with tempfile.TemporaryDirectory(prefix="connex-firmware-check-") as directory:
        temp = Path(directory)
        flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-fno-omit-frame-pointer", "-fno-sanitize-recover=all",
                 "-fsanitize=address,undefined" if args.asan else "-fsanitize=undefined",
                 "-I" + str(SOURCE), "-I" + include]
        for name, sources in {
            "device_info": [ROOT / "tests" / "firmware_device_info_test.cpp"],
            "status": [ROOT / "tests" / "firmware_status_test.cpp"],
            "access": [SOURCE / "access.cpp", ROOT / "tests" / "firmware_access_test.cpp"],
            "domain": [SOURCE / "domain.cpp", ROOT / "tests" / "firmware_domain_test.cpp"],
            "schedule": [SOURCE / "domain.cpp", ROOT / "tests" / "firmware_schedule_test.cpp"],
            "api": [SOURCE / "domain.cpp", SOURCE / "api.cpp", SOURCE / "dns.cpp", ROOT / "tests" / "firmware_api_test.cpp"],
        }.items():
            binary = temp / name
            run(["g++", *flags, *sources, "-o", binary])
            run([binary])
        run(["node", ROOT / "scripts" / "build_firmware_web.cjs", "--check"])
        run(["node", ROOT / "tests" / "firmware_web_test.cjs"])
        for name in ["README.md", "AGENTS.md",
                     *(str(path.relative_to(ROOT)) for path in sorted((ROOT / "doc").rglob("*.md")))]:
            document = ROOT / name
            for link in re.findall(r"\[[^\]]*\]\(([^)]+)\)", document.read_text()):
                if "://" in link or link.startswith("#"):
                    continue
                target = (document.parent / link.split("#", 1)[0]).resolve()
                for private in (ROOT / "data", ROOT / "plans"):
                    if target == private or private in target.parents:
                        raise RuntimeError(f"Published documentation links to a local-only archive in {name}: {link}")
                if not target.exists():
                    raise RuntimeError(f"Broken link in {name}: {link}")
        print("PASS: firmware documentation relative links", flush=True)
        if args.build:
            # ESP32 3.3.12 savehex hooks also export beside the sketch, even with
            # --output-dir. Stage a copy so those artifacts never enter source.
            staged = temp / "firmware"
            shutil.copytree(SOURCE, staged, ignore=shutil.ignore_patterns("build"))
            run([args.arduino_cli, "compile", "--fqbn", lock["fqbn"], "--warnings", "all",
                 "--build-path", temp / "build", "--output-dir", output, staged])
    print("Firmware checks passed. No upload or RF transmission performed.")


if __name__ == "__main__":
    main()
