# LG G5 (webOS) Platform Findings — PyroWave/Mali GPU Feasibility

Investigation log for whether a PyroWave-style GPU-compute video codec could
run on this TV's Mali GPU, done in the context of Aurora (webOS Moonlight
fork) + Vibepollo (Sunshine fork host).

## Platform facts

| Item | Value | How found |
|---|---|---|
| Kernel arch | `aarch64` | `uname -m` |
| Userland arch | **32-bit ARM, softfloat (armel)** | `/lib/ld-linux.so.3` present (armel loader name; armhf would be `ld-linux-armhf.so.3`). `libc.so.6` and `libmali.so` both confirmed ELFCLASS32 via `od -An -tx1 -j 4 -N 1`. |
| Max GLIBC symbol version | `GLIBC_2.35` | `strings /lib/libc.so.6 \| grep -o "GLIBC_2\.[0-9]*" \| sort -Vu` |
| GPU | Mali-G510 | Reported by `vkGetPhysicalDeviceProperties` |
| Mali driver version | 46.0.0 | Same |
| Vulkan API version | 1.3.260 (instance), ICD manifest claims 1.3.216 | `vkEnumerateInstanceVersion` vs `mali.json` |
| `libmali.so` location | `/usr/lib/libmali.so` (also bind-mounted into every app's palm jail) | `find / -iname libmali.so \| grep -v jail` |
| Vulkan ICD manifest | `/usr/share/vulkan/icd.d/mali.json`, points to `libmali.so` | Present at system level, not just per-jail |
| Vulkan loader | **Absent.** No `libvulkan.so*` anywhere on the system. | `find / -iname "libvulkan.so*"` returned nothing |
| DRM render node | `/dev/dri/card0`, `/dev/dri/renderD128` present | `ls /dev/dri/` |
| GPU kernel driver | Live Mali driver confirmed via dmesg (`mali f5000000.gpu: Tiler heap reclaim...`) | `dmesg \| grep -i mali` |
| GLES/EGL | Bundled per-app inside each palm jail (`libEGL.so`, `libGLESv2.so`, Starfish EGL integration plugin) | `find / -iname libGLESv2*` |
| `timestampPeriod` | 20.0 ns/tick | `VkPhysicalDeviceProperties.limits.timestampPeriod` |

## Key implications for building anything native for this TV

1. **No Vulkan loader.** Any app wanting Vulkan must `dlopen("/usr/lib/libmali.so")`
   directly and pull `vkGetInstanceProcAddr` (or, more reliably, the formal ICD
   entry point `vk_icdGetInstanceProcAddr` — Mali exports the ICD-interface name,
   not the plain loader-facing one) via `dlsym`. From there, bootstrap instance →
   physical device → logical device → queue manually, no `libvulkan.so` involved
   at any point.
2. **Cross-compile target is `arm-linux-gnueabi` (softfloat), not
   `aarch64-linux-gnu` and not `arm-linux-gnueabihf`.** Both of those were tried
   first and failed:
   - `aarch64-linux-gnu` build: loads, but `wrong ELF class: ELFCLASS64` (kernel
     is 64-bit, userland isn't).
   - `arm-linux-gnueabihf` (hardfloat) build: correct 32-bit class, but failed
     with `internal error` from `ld-linux.so.3`, an ABI mismatch symptom (the
     loader itself is the softfloat variant, and the embedded interpreter path
     it expects — `ld-linux-armhf.so.3` — doesn't exist by that name here).
   - `arm-linux-gnueabi` (softfloat): interpreter string matches exactly
     (`/lib/ld-linux.so.3`), loads and runs correctly. **This is the one to use.**
3. **Cross-compile against an old-enough GLIBC baseline.** Default cross
   toolchains on newer host distros (e.g. Ubuntu 24.04's glibc 2.39) can silently
   produce binaries requiring symbol versions the TV doesn't have (max is 2.35).
   In practice, sticking to a small set of long-stable libc calls (dlopen/dlsym,
   clock_gettime, malloc, printf) kept the actual requirement at `GLIBC_2.34`,
   safely under the ceiling — but this should be checked per-build with
   `arm-linux-gnueabi-objdump -T <binary> | grep GLIBC`, not assumed.
4. **Static linking is not an option** for anything that needs `dlopen`
   (glibc explicitly doesn't support dlopen from fully static binaries), so these
   binaries are dynamically linked and depend on the ABI facts above holding.

## Benchmark results so far

All benchmarks: Vulkan compute via the direct-`dlopen` bootstrap above, GPU-side
timing via `VK_QUERY_TYPE_TIMESTAMP`, `timestampPeriod` = 20 ns.

### Round 1 — naive single-dispatch proxy (8 chained multiply-adds/pixel)
Not a real transform, just a rough per-pixel arithmetic-cost probe.

| Resolution | Avg GPU dispatch time |
|---|---|
| 1440p | 3.89 ms |
| 4K | 8.44 ms |

Reference point: real webOS HEVC hardware decode (Aurora, 1 ref frame) measured
independently at ~8.5 ms; NVENC host-side encode measured at 5-7 ms across
1440p-3.5k on host GPU (RTX 3070 Ti).

### Round 2 — real CDF 9/7 lifting, 4 decomposition levels, shared-memory tile fusion
Proper lifting coefficients, ping-pong buffers to avoid cross-workgroup races,
forward (encode-equivalent) and inverse (decode-equivalent) timed separately.

| Resolution | Forward (encode-equiv) | Inverse (decode-equiv) |
|---|---|---|
| 1080p | 0.134 ms | 0.123 ms |
|1440p | *(run interrupted — see below)* | |
| 3.5k | *(not yet reached)* | |

**Status: in progress.** The 1440p/3.5k runs were still executing after ~2
minutes with no output, longer than the 1080p case took for its (identical
iteration count) full run. Two live hypotheses, not yet distinguished:
- Legitimate: Mali's tile-based architecture may have per-barrier stall costs
  that scale worse than linearly with resolution across 480 dispatches per
  resolution/direction pair.
- Possible bug: a genuine GPU-side stall/hang with no validation layer
  available on-device to catch a synchronization mistake before it manifests
  as a stall rather than a clean error.

Next step: rerun bounded with `timeout`, and/or reduce `N_ITERS` for the
higher resolutions to isolate whether it's slow-but-progressing or actually
stuck.

## Confirmed driver bug: multi-barrier compute shaders fail silently

**Root cause, confirmed via `diag_suite`:** any compute shader containing
more than one `barrier()` call produces **zero writes** on this Mali-G510 /
driver 46.0.0, regardless of whether the surrounding logic is conditional or
unconditional. This is independent of the wavelet math entirely — a trivial
shader that just does `x += 1.0; barrier(); x += 2.0;` fails exactly the same
way as the real 4-stage lifting shader did.

Diagnostic matrix (16 known samples, compared against hand-computed expected
values):

| Variant | Barriers | Conditional writes | Result |
|---|---|---|---|
| Baseline | 1 | No | **PASS** |
| Count test | 2 | No | **FAIL** (no write at all — buffer stayed at pre-dispatch sentinel) |
| Conditional test | 2 | Yes (isOdd) | **FAIL** (same failure mode) |
| Real-shader-shaped | 4 | Yes (isOdd) | **FAIL** (same failure mode) |

The failure mode is not wrong math or a hang — it's a **complete absence of
any write to the output buffer**, as verified by pre-filling the destination
buffer with a distinct sentinel value (`-1.0`) that stays completely
untouched after dispatch. Every Vulkan API call up to and including
`vkWaitForFences` returns `VK_SUCCESS`; there is no error surfaced anywhere
in the API. This makes it a specifically dangerous bug to develop against,
since nothing about the API usage signals that anything is wrong.

**Workaround:** split each lifting stage into its own dispatch (one
`barrier()` each, the proven-working pattern), chained via buffer ping-pong
between GPU-side `vkCmdPipelineBarrier` calls (a different, much more
standard synchronization primitive than in-shader `barrier()`, not
implicated by this bug) instead of fusing multiple stages with multiple
in-shader barriers. This is what `dwt_lifting_staged.comp` / the current
`dwt_bench.c` does. Cost: 5x more dispatches per axis/level (predict1,
update1, predict2, update2, scale, each as a separate dispatch) instead of
1 fused dispatch, which will show up as real, higher latency in the timing
numbers — but those numbers will be trustworthy, unlike the earlier fused
ones.

**Not yet independently verified:** whether *multiple separate dispatches
within one command buffer*, synchronized via `vkCmdPipelineBarrier` (a
command-buffer-level primitive, not the in-shader `barrier()` that's
confirmed broken), also has issues on this driver. This is an extremely
standard, near-universal pattern (unlike multiple in-shader barriers, which
is comparatively rare), so it's a reasonable working assumption, but it
hasn't been directly isolated the way the in-shader barrier count was. If
timing numbers from the staged benchmark look implausible again, this
assumption is the next thing to test in isolation.

## Practical implication for Aurora/Vibepollo

If a real PyroWave-style port is ever attempted for LG webOS TVs on Mali
GPUs, **fused multi-stage shared-memory compute kernels are not a safe
default assumption** on all Mali driver versions in the field. A production
implementation would need either: (a) a driver-version check with a
fallback to the staged/non-fused dispatch pattern, or (b) confirmation from
ARM/Mali driver release notes on whether this is a known, fixed-in-a-later-
version issue. Worth a search on Mali driver bug trackers or the Sunshine/
Aurora issue trackers for prior reports of this pattern before assuming it's
undiscovered.


