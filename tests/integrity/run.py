#!/usr/bin/env python3
"""Run each case in a fresh process, preserving full output and device identity."""
import argparse
import datetime
import hashlib
import json
from pathlib import Path
import shlex
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument("--serial", required=True)
parser.add_argument("--out", type=Path, required=True)
args = parser.parse_args()
source = Path(__file__).resolve().parent
bundle = source / "build"
manifest = json.loads((bundle / "manifest.json").read_text())
for name, expected in manifest.items():
    if hashlib.sha256((bundle / name).read_bytes()).hexdigest() != expected:
        raise SystemExit(f"stale build manifest: {name}")
remote = "/data/local/tmp/hybris-integrity-test"
adb = ["adb", "-s", args.serial]
def shell(command, **kwargs):
    return subprocess.run(adb + ["shell", command], text=True, **kwargs)
args.out.mkdir(parents=True, exist_ok=True)
shell(f"mkdir -p {remote}", check=True)
subprocess.run(adb + ["push", str(bundle) + "/.", remote + "/"], check=True, capture_output=True)
shell(f"chmod -R u+rx {remote}", check=True)
properties = {name: shell("getprop " + name, check=True, capture_output=True).stdout.strip()
              for name in ("ro.product.model", "ro.build.fingerprint", "ro.build.version.sdk")}
hash_command = (f"cd {remote} && sha256sum /system/lib64/libcrypto.so /system/lib64/libssl.so "
                "./libintegrity-*.so ./libcrypto.so ./libssl.so "
                "./hybris/libhybris/linker/q.so ./hybris/libhybris-common.so.1")
before = shell(hash_command, check=True, capture_output=True).stdout
results = []
for backend in ("native", "hybris"):
    cases = [("system", "system")]
    cases += [(name, f"./{name} accept") for name in
              ("libcrypto.so", "libssl.so", "libintegrity-shared.so", "libintegrity-static.so",
               "libintegrity-legacy.so")]
    if backend == "hybris":
        cases += [(name, f"./{name} reject") for name in
                  ("libintegrity-shared-corrupt-code.so", "libintegrity-shared-corrupt-hash.so",
                   "libintegrity-legacy-corrupt-code.so", "libintegrity-legacy-corrupt-hash.so",
                   "libintegrity-missing-bounds.so", "libintegrity-ambiguous.so")]
    for name, operands in cases:
        executable = "./probe-bionic"
        env = ""
        if backend == "hybris":
            env = ("HYBRIS_LINKER_DIR=$PWD/hybris/libhybris/linker "
                   f"HYBRIS_ANDROID_SDK_VERSION={shlex.quote(properties['ro.build.version.sdk'])} ")
            executable = "./glibc/ld-linux-aarch64.so.1 --library-path ./hybris:./glibc ./probe-glibc"
        command = f"cd {remote} && {env}{executable} {backend} {operands} 2>&1"
        name = f"{backend}-{name}"
        try:
            result = shell(command, capture_output=True, timeout=90)
            output, code = result.stdout + result.stderr, result.returncode
        except subprocess.TimeoutExpired as error:
            output, code = str(error), 124
        (args.out / f"{name}.log").write_text(output)
        passed = code == 0 and "PASS " in output and "FAIL " not in output
        if "corrupt" in name or "ambiguous" in name:
            passed &= "original module HMAC" in output
        if "missing-bounds" in name:
            passed &= "lacks exported integrity metadata" in output
        results.append(dict(name=name, passed=passed, returncode=code, command=command))
        print(f"{'PASS' if passed else 'FAIL'} {name}", flush=True)
        if not passed:
            print(output[-4000:], flush=True)
report = dict(serial=args.serial, properties=properties,
              time=datetime.datetime.now(datetime.timezone.utc).isoformat(),
              manifest=manifest, build_info=json.loads((bundle / "build-info.json").read_text()), results=results)
after = shell(hash_command, check=True, capture_output=True).stdout
report.update(files_before=before, files_after=after, disk_unchanged=before == after)
(args.out / "results.json").write_text(json.dumps(report, indent=2) + "\n")
raise SystemExit(0 if before == after and all(r["passed"] for r in results) else 1)
