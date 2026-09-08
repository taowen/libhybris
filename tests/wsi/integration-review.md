# Historical WSI fixture and X11 resize review, 2026-09-08

> Historical fixture evidence. The APK/server builder and private Xwayland
> lifecycle described below have been removed. Current tests attach to an
> installed, already running compositor and its external display; see
> [current instructions](README.md). These historical runs do not validate
> the new external-service connection workflow.


## Current entry point

`tests/wsi/run.py --backend hybris|turnip --platform wayland|xcb|xlib`
attaches to an installed, already running compositor; the default package is
`io.taowen.ardesk`. Ardesk owns anlabwc, Xwayland and their build/installation.
The runner stages clients and their selected Vulkan runtime, records service
identity and collects evidence. It does not build a private APK/server or
start and stop those services. Package, socket and display arguments select
the externally supplied endpoints. See [README.md](README.md) and the
[current product-backend evidence](product-backends.md).

## Retired fixture used for the records below

At the time of these runs, three window entry points had been collapsed into
one runner that still owned APK startup/cleanup and server extraction. That
fixture's APK contained anlabwc and a bundled protocol-enabled Xwayland; an
alternate server override was available for its missing-protocol control.
The fixture builder, service lifecycle and alternate-server override have
since been deleted. Its hashes and run IDs below describe that old APK only.

The obsolete `probe_icd_surface.c` executable was also removed in that batch;
its applicable surface checks were folded into the Wayland client. Historical
missing-protocol and frontend-window runs below are not current coverage.
Current missing-protocol checks require an externally supplied display without
TAWC-DRI. Further historical observations remain in [history.md](history.md).

## Historical X11 resize correction

The previous ICD queried X geometry for capabilities but continued acquiring
old-size images after a real X ConfigureWindow. The corrected ICD in that batch recorded its
extent and a sticky out-of-date/surface-lost state. Acquire checks before
changing the output index or signaling synchronization objects. Native dequeue
also notices TAWC-DRI ConfigureNotify while waiting. Creation rejects a stale
fixed extent. Retirement remains distinct, preserving previously acquired
retired-image behavior when the surface is compatible.

Rejected presents still call Android's image release operation to consume the
application waits; they cancel the buffer with that release fence rather than
send an obsolete-size frame. This follows the
[Vulkan present failure contract](https://docs.vulkan.org/refpages/latest/refpages/source/vkQueuePresentKHR.html).
The resize probe holds an image across the X resize, checks acquire and present
OUT_OF_DATE (including pResults), untouched acquire output and unsignaled fence,
then re-signals and waits on the same binary semaphore with a bounded fence.
It creates the replacement with oldSwapchain, verifies old image handles remain
stable until destruction, and renders the new dimensions.

TAWC-DRI already supplies ConfigureNotify and BufferRelease, so this did not
require changing the protocol patch or anlabwc. Its lack of a submitted GPU
fence still requires a host fence wait; that is a separate asynchronous-submit
limitation. That batch made no Mesa changes; later product Turnip corrections are recorded
in [product-backends.md](product-backends.md).

## Historical device evidence

Devices are Redmi `29854870` / Adreno vendor HAL and X300
`10AFA31610002QH` / Mali vendor HAL with explicit MMUD loader quirk.
OnePlus results from before the user's device switch are historical only.
The fixture APK used by these runs had SHA256
`a1ca044447f70e392cc9fc7178dea85471d656de1b709f0cbe82d61f096f0f73`;
its bundled Xwayland had SHA256
`601cd4c1d7a32dbd2bd5378bcdea5b7be347d29fd25fff974d3e2ec93d2ecc08`.
The retained APK manifest also records the exact backend, source input and dependencies.

The first unified matrix below completed 26 invocations / 28 client processes:
XCB/Xlib each run present, resize, missing protocol, acquire timeout and control;
Wayland runs VVL/SyncVal swapchain review, two frontend clients under one process,
and separate capture/replay. All passed with stable compositor identity and no
cleanup errors. Each resize has 24 exact GPU readbacks, six exact screenshots,
24 protocol presents and 23 observed releases; unsupported/full-conformance
claims are not inferred from those checks.

| Run | Device | Platform / case | Clients |
| --- | --- | --- | --- |
| 20260908T071331-4d30efde | Redmi | xcb / present | 1 |
| 20260908T071333-096696de | Mali | xcb / present | 1 |
| 20260908T071342-33ef0ab4 | Mali | xcb / resize | 1 |
| 20260908T071343-f410a1d6 | Redmi | xcb / resize | 1 |
| 20260908T071402-74ebfabf | Mali | xcb / missing-protocol | 1 |
| 20260908T071405-4b45f17a | Redmi | xcb / missing-protocol | 1 |
| 20260908T071406-cf9145fc | Mali | xcb / acquire-timeout | 1 |
| 20260908T071410-d6b8d258 | Mali | xcb / control | 1 |
| 20260908T071411-c424361a | Redmi | xcb / acquire-timeout | 1 |
| 20260908T071413-775d0ad0 | Mali | xlib / present | 1 |
| 20260908T071417-6e566691 | Redmi | xcb / control | 1 |
| 20260908T071421-5e4e958e | Redmi | xlib / present | 1 |
| 20260908T071423-49a08a5a | Mali | xlib / resize | 1 |
| 20260908T071432-f4ade067 | Redmi | xlib / resize | 1 |
| 20260908T071443-d135d871 | Mali | xlib / missing-protocol | 1 |
| 20260908T071447-aae1882c | Mali | xlib / acquire-timeout | 1 |
| 20260908T071451-9534c3e2 | Mali | xlib / control | 1 |
| 20260908T071454-26bafd9b | Mali | wayland / swapchain-review | 1 |
| 20260908T071454-e97eefe6 | Redmi | xlib / missing-protocol | 1 |
| 20260908T071500-53cddf25 | Redmi | xlib / acquire-timeout | 1 |
| 20260908T071506-71ce5ddd | Redmi | xlib / control | 1 |
| 20260908T071510-f1fe4ba2 | Redmi | wayland / swapchain-review | 1 |
| 20260908T071515-b1e08c35 | Mali | wayland / present (frontend) | 2 |
| 20260908T071534-1a32ca4b | Redmi | wayland / present (frontend) | 2 |
| 20260908T071550-deb8da2e | Mali | wayland / present (capture) | 1 |
| 20260908T071615-b870945e | Redmi | wayland / present (capture) | 1 |

All artifacts are under `tests/wsi/build/results/<run>/`. Each child result
retains the source/ELF manifests and detailed logs; the outer result records
isolation, FD snapshots, status and timing. Present took 9.3–11.2 seconds,
resize 19.4–21.3 seconds and X11 controls 2.6–5.8 seconds including staging and
screenshots on these connections. These are developer turnaround times, not
GPU or presentation performance.

After retaining the old surface-only probe's explicit application-selected
extent invariant, the final Wayland client was rebuilt and all affected paths
were rerun on both devices. These six invocations / eight clients also passed:

| Final run | Path / device |
| --- | --- |
| 20260908T072310-0bfe2a0b | capture-mali |
| 20260908T072318-586b3054 | capture-redmi |
| 20260908T072234-845b4170 | frontend-mali |
| 20260908T072237-b4a3b776 | frontend-redmi |
| 20260908T072214-113f3f67 | review-mali |
| 20260908T072213-92a70434 | review-redmi |

No production change followed the tested clean 48.9-second AArch64 build.
All changed production files match its source copy, and the ICD still exports
only the three loader entries. Client source copies, APK contents, staged
libraries and runtime hashes were also checked against their manifests.

## Preserved failures and limits

The pre-resize library (`ff7505a`) is retained under the ignored
`tests/x11/build/pre-resize` input. Both APIs on Mali and Redmi reject the new
resize probe because old acquire incorrectly returns SUCCESS after ConfigureWindow.
The original raw failures remain in `tests/x11/build/results` and the
`/tmp/hybris-x11-resize-before-*-{redmi,mali}.log` logs.

An early Redmi new-library run `20260908T065704-685fb9c1` passed Vulkan checks
but failed screenshots: sampling 0.1 seconds after enqueue still saw green at
the red-frame marker. The same production library and Xwayland passed diagnostic
run `20260908T065954-a3393138` with a 0.6-second screenshot delay and two-second
client pause. The shared collector now uses that explicit settling interval;
full-image and green-to-red checks are unchanged. The earlier failure is not
reclassified. Frame enqueue and buffer release alone do not prove physical
presentation completion.

The deliberate five-second host-timeout run `20260908T071734-ed7f65f9` reports
TIMEOUT 124, reaps the owned X11 session, preserves evidence, retains the same
compositor identity until cleanup, and leaves no test package process. It is
an expected negative result, not one of the 26 passing invocations.
Headless ICD version, GDPA, lifetime and direct allocator checks passed on the
same production build: Redmi `20260908T071756-9f21b000`, Mali
`20260908T071757-01469ae3`.

Resize during an already-blocked acquire, present-first resize detection,
stale-extent creation races, rootless placement, multiple X11 windows, minimize,
disconnect recovery and long-running FD accounting are not established by this
matrix. X11 capture/replay and CTS remain open. Wayland capture retains the
previous virtual-swapchain replay scope. G11 and the full gaps objective remain
open.
