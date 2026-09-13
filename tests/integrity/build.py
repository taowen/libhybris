#!/usr/bin/env python3
"""Build independent Android integrity fixtures and native/glibc probes."""
import hashlib
import hmac
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
from elftools.elf.elffile import ELFFile

ROOT = Path(__file__).resolve().parents[2]
SOURCE = Path(__file__).resolve().parent
OUT = SOURCE / "build"
OUT.mkdir(exist_ok=True)
NDK = Path(os.environ["ANDROID_NDK_HOME"])
CC = NDK / "toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android26-clang"
SHA = ROOT / "hybris/common/q/integrity_sha256.c"

def run(*args):
    subprocess.run([str(a) for a in args], check=True)

run(CC, "-O2", "-Wall", "-Wextra", SOURCE / "probe.c", "-ldl", "-o", OUT / "probe-bionic")
image = subprocess.check_output([str(ROOT / "tools/ensure-builder.sh")], text=True).strip()
run("podman", "run", "--rm", "--userns=keep-id", "-v", f"{SOURCE}:/src:Z",
    image, "aarch64-linux-gnu-gcc", "-O2", "-Wall", "-Wextra", "/src/probe.c",
    "-ldl", "-pthread", "-o", "/src/build/probe-glibc")

def sign(path, shared):
    with path.open("rb") as stream:
        elf = ELFFile(stream)
        symbols = {s.name: s["st_value"] for s in elf.get_section_by_name(".symtab").iter_symbols()}
        segments = [dict(s.header) for s in elf.iter_segments() if s["p_type"] == "PT_LOAD"]
    def offset(address):
        for seg in segments:
            if seg["p_vaddr"] <= address < seg["p_vaddr"] + seg["p_filesz"]:
                return address - seg["p_vaddr"] + seg["p_offset"]
        raise ValueError(address)
    data = bytearray(path.read_bytes())
    protected = bytearray()
    for region in ("text", "rodata") if shared else ("text",):
        start = symbols[f"BORINGSSL_bcm_{region}_start"]
        length = symbols[f"BORINGSSL_bcm_{region}_end"] - start
        if shared:
            protected += struct.pack("<Q", length)
        protected += data[offset(start):offset(start) + length]
    digest = hmac.digest(bytes(64), protected, "sha256")
    location = offset(symbols["fixture_expected_hash"])
    data[location:location + 32] = digest
    if "fixture_duplicate_hash" in symbols:
        duplicate = offset(symbols["fixture_duplicate_hash"])
        data[duplicate:duplicate + 32] = digest
    path.write_bytes(data)
    return data, offset(symbols["BORINGSSL_bcm_text_start"]), location

for name, flags in (("shared", []), ("static", ["-DSTATIC_FORMAT"]),
                    ("legacy", ["-DMISSING_GETTER"]),
                    ("ambiguous", ["-DMISSING_GETTER", "-DDUPLICATE_HASH"]),
                    ("missing-bounds", ["-DMISSING_BOUNDS"])):
    path = OUT / f"libintegrity-{name}.so"
    run(CC, "-O2", "-fPIC", "-shared", "-Wl,--rosegment,-z,separate-code", "-DINTEGRITY_FIXTURE", *flags,
        "-I", SHA.parent, SOURCE / "fixture.c", SOURCE / "fixture.S", SHA, "-o", path)
    data, code, expected = sign(path, name != "static")
    if name in ("shared", "legacy"):
        for kind, where in (("code", code), ("hash", expected)):
            corrupted = bytearray(data)
            corrupted[where] ^= 1
            (OUT / f"libintegrity-{name}-corrupt-{kind}.so").write_bytes(corrupted)
for name in ("libcrypto.so", "libssl.so"):
    run(CC, "-O2", "-fPIC", "-shared", "-I", SHA.parent, SOURCE / "fixture.c",
        f"-Wl,-soname,{name}", "-o", OUT / name)
    with (OUT / name).open("rb") as stream:
        constructors = ELFFile(stream).get_section_by_name(".init_array")
        if constructors is None or constructors["sh_size"] == 0:
            raise RuntimeError(f"{name}: compiler removed the constructor fixture")

baseline = ROOT / "tests/baseline/build"
for source, target in ((baseline / "install/usr/lib/hybris", OUT / "hybris"),
                       (baseline / "runtime", OUT / "glibc")):
    if target.exists():
        shutil.rmtree(target)
    shutil.copytree(source, target, symlinks=False)
inputs = [SOURCE / name for name in ("probe.c", "fixture.c", "fixture.S", "build.py", "run.py")]
inputs += [SHA, SHA.with_suffix(".h")]
info = dict(sources={str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs},
            compiler=subprocess.check_output([str(CC), "--version"], text=True),
            builder=image, hybris=json.loads((baseline / "manifest.json").read_text()))
(OUT / "build-info.json").write_text(json.dumps(info, indent=2) + "\n")
files = {str(p.relative_to(OUT)): hashlib.sha256(p.read_bytes()).hexdigest()
         for p in sorted(OUT.rglob("*"))
         if p.is_file() and not p.is_symlink() and p != OUT / "manifest.json"}
(OUT / "manifest.json").write_text(json.dumps(files, indent=2) + "\n")
print(OUT)
