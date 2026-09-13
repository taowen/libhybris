#!/usr/bin/env python3
"""Compare the private SHA with Python/OpenSSL, including streaming and padding."""
import ctypes as c
import hashlib
from pathlib import Path
import random
import subprocess
import tempfile

SOURCE = Path(__file__).resolve().parents[2] / "hybris/common/q/integrity_sha256.c"
class Context(c.Structure):
    _fields_ = [("buf", c.c_ubyte * 64), ("count", c.c_uint64), ("val", c.c_uint32 * 8)]

with tempfile.TemporaryDirectory() as directory:
    so = Path(directory) / "sha256.so"
    subprocess.run(["cc", "-std=c99", "-O2", "-Wall", "-Wextra", "-Werror",
                    "-fPIC", "-shared", str(SOURCE), "-o", str(so)], check=True)
    sha = c.CDLL(str(so))
    sha.hybris_sha256_init.argtypes = [c.POINTER(Context)]
    sha.hybris_sha256_update.argtypes = [c.POINTER(Context), c.c_void_p, c.c_size_t]
    sha.hybris_sha256_out.argtypes = [c.POINTER(Context), c.c_void_p]
    rng = random.Random(731)
    lengths = list(range(260)) + [511, 512, 513, 4095, 4096, 4097, 1000000]
    for length in lengths:
        data = rng.randbytes(length)
        expected = hashlib.sha256(data).digest()
        for step in (1, 7, 63, 64, 65, 1024, max(1, length)):
            if length > 4097 and step < 1024:
                continue
            ctx, result = Context(), c.create_string_buffer(32)
            sha.hybris_sha256_init(c.byref(ctx))
            sha.hybris_sha256_update(c.byref(ctx), b"", 0)
            for pos in range(0, length, step):
                chunk = data[pos:pos + step]
                sha.hybris_sha256_update(c.byref(ctx), chunk, len(chunk))
            sha.hybris_sha256_out(c.byref(ctx), result)
            assert result.raw == expected, (length, step)
            # Finalization must not mutate the streaming context.
            sha.hybris_sha256_update(c.byref(ctx), b"abc", 3)
            sha.hybris_sha256_out(c.byref(ctx), result)
            assert result.raw == hashlib.sha256(data + b"abc").digest()
    print(f"PASS SHA256: {len(lengths)} lengths, padding, chunking, repeat finalization")
