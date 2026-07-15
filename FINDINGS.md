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

## Practical takeaway (provisional, pending the completed 1440p/3.5k numbers)

The 1080p numbers (0.13ms forward / 0.12ms inverse) are dramatically faster
than the naive single-dispatch proxy suggested, and comfortably beat the
current ~8.5ms HEVC hardware decode path if they hold up at higher resolutions
— but 1080p halves the per-level pixel count fastest, so it's the
*easiest* case for this workload. Whether the same margin survives at 1440p
and 3.5k (the resolutions actually in scope, per current interest — 4K is
explicitly not a target right now) is the open question the interrupted run
was meant to answer.
