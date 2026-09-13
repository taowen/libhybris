# BoringSSL integrity after TLS relocation

The Q loader rewrites AArch64 `MRS TPIDR_EL0` instructions so Android code can
use bionic TLS inside a glibc process. BoringSSL's FIPS startup test hashes its
mapped code and therefore detects those legitimate loader changes. Skipping
all `libcrypto.so` and `libssl.so` constructors also skips unrelated
initialization and algorithm self-tests.

The loader now records each original instruction and its replacement. After
ELF relocation, before constructors, it reconstructs the original protected
text for HMAC verification. Only if that original HMAC is valid does it update
the private expected digest for the transformed image. The original
`BORINGSSL_integrity_test` and every constructor run normally. No function is
forced to return success and the file on disk is unchanged.

## Formats and limits

Protected ranges come from the module's own exported `BORINGSSL_bcm_*`
symbols. Shared format hashes little-endian 64-bit lengths plus text and
rodata; static format hashes its combined text region without lengths. Both
use BoringSSL's zero-key HMAC-SHA256 format.

`FIPS_module_hash` is the preferred public pointer accessor. Older releases,
including the tested OnePlus 8T system library, lack that accessor. For those,
the loader searches separate read-only data for the complete reconstructed
original 256-bit digest and requires exactly one match. This is content-based
resolution, not a firmware offset or an instruction-pattern patch. Missing or
ambiguous matches are rejected, and the module's native integrity test still
decides whether its actual expected digest is correct.

Invalid region bounds, inconsistent relocation records, changed original
contents, writable/executable hash storage, or a hash page shared with writable
or executable segments cause a load error. There is no constructor-skipping
fallback. Libraries without the recognized integrity metadata retain ordinary
loader behavior. Hash pages are writable only during the private update and
are restored to read-only. TLS patching itself retains its existing mechanism.

This preserves a meaningful integrity check for a loader-transformed image;
it does **not** preserve or claim FIPS certification. It is not authentication
of an untrusted DSO: both code and its embedded reference hash originate from
that DSO, as in the original BoringSSL check. Generated TLS thunks are outside
the module's original integrity region.

The private SHA-256 implementation is the MIT-licensed BearSSL
`src/hash/sha2small.c` subset at commit
`7bea48e5e850ab4cafbe68d3765cdaba13a86d6f`, with private names and bytewise endian
helpers, without SHA-224 or vtables. Its license is retained in the source.
It does not call the Android cryptographic library it is verifying.

Upstream format references:

- [BoringSSL bcm.c, fips-20240407](https://boringssl.googlesource.com/boringssl/+/refs/heads/fips-20240407/crypto/fipsmodule/bcm.c)
- [BoringSSL bcm.c, fips-20180730](https://boringssl.googlesource.com/boringssl/+/refs/heads/fips-20180730/crypto/fipsmodule/bcm.c)
- [BearSSL SHA-256 source](https://www.bearssl.org/gitweb/?p=BearSSL;a=blob;f=src/hash/sha2small.c;hb=7bea48e5e850ab4cafbe68d3765cdaba13a86d6f)

## Reproduce

Requires Python 3.11+, pyelftools, a host C compiler, the repository's Podman
builder, Android NDK and an AArch64 Android device with ADB.

```sh
python3 tests/integrity/test_sha256.py
tools/build-aarch64.sh --incremental
ANDROID_NDK_HOME=/path/to/ndk python3 tests/integrity/build.py
python3 tests/integrity/run.py --serial DEVICE --out tests/integrity/results/DEVICE
```

The SHA test compares 267 lengths, boundary padding, streaming chunk sizes and
repeated finalization with Python's independent hash implementation. Fixture
digests are injected by Python's HMAC implementation after linking.

Each device case runs in a fresh process. Native and hybris cases exercise the
real system crypto/SSL libraries, native integrity and algorithm self-tests,
SHA-256 known answers and random generation on nine threads, and SSL context
creation. Separate DSOs with SONAME `libcrypto.so` and `libssl.so` prove that
their constructors actually run. Shared, static and legacy FIPS fixtures
require constructor execution and integrity results `1 → 0 → 1` across
runtime code corruption/restoration. Negative fixtures cover changed input
code, changed expected hashes, missing bounds and ambiguous legacy hashes.
Public hash pointers must remain in read-only private mappings. Device file
hashes before and after the suite must be identical.

The constructor counter is volatile and the build requires a nonempty
`.init_array`: Clang can otherwise pre-evaluate this simple constructor and
fold the accessor into a constant. As a negative control, both SONAME fixtures
fail the constructor assertion under the pre-change `641e615` linker and pass
under the fixed linker.

The deliberate runtime code corruption uses an anonymous private page copy:
Android otherwise forbids restoring execute permission to a modified file
mapping. Production loader code does not use this test mechanism.

Results contain source and binary hashes, the production build manifest,
device fingerprints, complete commands and separate logs. GPU/TLS regression
uses the existing baseline suite:

```sh
python3 tests/baseline/run.py --serial DEVICE \
  --out tests/integrity/results/gpu-DEVICE \
  --case native-vk --case hybris-vk --case native-2 --case hybris-2 \
  --case native-3 --case hybris-3 --case hybris-tls-mrs
```

## Verified 2026-09-13

[Recorded results and source/binary hashes](verified-2026-09-13.json).

| Device | Integrity suite | Vulkan, GLES 2/3, TLS baseline |
| --- | --- | --- |
| Redmi M2012K11AC | 18/18 | 7/7 |
| vivo X300 V2509A | 18/18 | 7/7 |
| OnePlus 8T KB2000 | 18/18, including legacy system format | 7/7 |
| OnePlus PJZ110 | 18/18 | Native graphics and TLS pass; existing hybris vendor mapping failures |

PJZ110's `/vendor/lib64/libziparchive.so` and
`/vendor/lib64/libaconfig_storage_read_api_cc.so` mappings are denied under the
ADB shell. Vulkan exits 2 and GLES 2/3 exit 139. An independently built
pre-change commit `641e61594eb611c1074af33af8bf74eaf5b073b1` has the same errors
and exit states. Those failures are not counted as passes.

The subsequent [Android namespace fix](../baseline/namespaces.md) resolves
these PJZ110 graphics failures and records a fresh four-device integrity run.
The table above remains the historical result for the integrity-only change.
