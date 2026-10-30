# BC kernels

[MaliBridge by ASEbrahim](https://github.com/ASEbrahim/MaliBridge).
This is a modified integration in libhybris, not an official MaliBridge build.
The upstream additional attribution/name terms are retained in NOTICE.MaliBridge.

The block decoder and ETC2/EAC/ASTC encoders come from
[MaliBridge](https://github.com/ASEbrahim/MaliBridge/tree/95a1e600a51299294e06bb09db2db8cc509fe6a3/shaders),
commit `95a1e600a51299294e06bb09db2db8cc509fe6a3`, by ASEbrahim and MaliBridge
contributors, under GPL-3.0-or-later. Keep their copyright and SPDX notices.
The BC tables/algorithms derive from Sergii Kudlai's bcdec (MIT/Unlicense);
see LICENSE.bcdec. The encoder files carry their additional attribution.
The GPL text is in the repository root LICENSE.GPL3. The Vulkan libraries
linking these kernels must be distributed under GPL-3.0-or-later, together
with corresponding source. This does not relicense unrelated upstream files;
their existing notices remain in place.

The application path follows MaliBridge's default format policy: BC1 uses ETC2,
BC2/3/7 ASTC 4x4, BC4/5 EAC, and BC6H ASTC 4x4 HDR where the device supports
the required native features and image parameters. Native ASTC HDR is enabled
through Vulkan 1.3 or its extension during device creation. Unsupported targets
fall back to RGBA8, R8, RG8 or RGBA16F; eligible non-mutable BC1 UNORM images
can use R5G6B5 or A1R5G5B5. As upstream, BC6H signed values lose negative
components when transcoded to ASTC HDR. `HYBRIS_BC_TRANSCODE=0` selects decoding.

`HYBRIS_BC_TRANSCODE_QUALITY` accepts `auto` (default), `fast`, `default` and
`high`. Quality uses the upstream specialization constant. Auto selects fast
for mip levels of at least 1,048,576 texels and default for smaller levels;
all pieces of a mip use the same quality. Optional upstream ASTC 6x6/8x8
transcoding is not exposed by this adapter.

The layer exposes the decoder family after checking its common backing formats;
native BC support takes precedence. Optional image capabilities are queried per
image instead of disabling every BC format for an unsupported optional usage.
Work is scheduled per 4x4 block with explicit padded scratch rows. The existing
raw BC backing remains authoritative for copies and readback, which upstream
does not implement. RGB8/RGB16F output variants remain internal diagnostic
facilities and do not select application image formats. No robustness feature
is emulated or advertised by this port.

`generate-bc.py` is the existing offline kernel generator. It uses glslc and
spirv-val (GLSLC/SPIRV_VAL may select executables) and writes bc_decode.inc.
The normal product build consumes this committed output.
