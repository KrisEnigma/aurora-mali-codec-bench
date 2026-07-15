# Vulkan compute shader with >1 barrier() silently produces no output (Mali-G510, driver r46p0)

## Summary

On a Mali-G510 (Valhall) running driver version `46.0.0`
(`v1.r46p0-01eac1.97e3b3993cfe85dfc8fda5430a365edd`), any Vulkan compute
shader containing more than one `barrier()` call produces **zero writes to
its output buffer**, regardless of whether the surrounding control flow is
conditional or unconditional. No Vulkan API call reports an error; every
call in the affected code path, including `vkQueueSubmit` and
`vkWaitForFences`, returns `VK_SUCCESS`.

## Environment

- Device: LG webOS smart TV (2025 OLED lineup)
- GPU: Mali-G510
- Driver: `46.0.0` (`v1.r46p0-01eac1.97e3b3993cfe85dfc8fda5430a365edd`)
- Vulkan API version: 1.3.260 (instance-level); ICD manifest (`mali.json`)
  reports `1.3.216`
- Userland: 32-bit ARM, softfloat EABI (`armel`) on an `aarch64` kernel
- No system Vulkan loader present; the ICD (`libmali.so`) was called
  directly via its `vk_icdGetInstanceProcAddr` entry point

## Reproduction

Full source, build scripts, and the exact diagnostic binary used are here:
**https://github.com/KrisEnigma/aurora-mali-codec-bench**
(see `shaders/diag_*.comp` and `src/diag_suite.c`)

Minimal repro shape:

```glsl
#version 450
layout(local_size_x = 256, local_size_y = 1) in;
layout(std430, binding = 0) readonly buffer Src { float srcData[]; };
layout(std430, binding = 1) writeonly buffer Dst { float dstData[]; };
// ... load into shared memory, one barrier, this part works fine ...
barrier();

tile[i] += 1.0;
barrier();          // <-- second barrier in the same shader invocation
tile[i] += 2.0;

dstData[idx] = tile[i];
```

With exactly one `barrier()` call, this pattern works correctly. Adding a
**second** `barrier()` call anywhere in the same shader — even with
trivial, unconditional, non-divergent code on both sides of it — causes the
final buffer write to silently not happen. The destination buffer is
observed to retain whatever value it held before dispatch (verified by
pre-filling it with a sentinel value distinct from both the input and any
plausible computed output).

## Diagnostic matrix

Four shader variants, isolating barrier count and conditional-vs-
unconditional writes as separate variables, run in a single test binary
against known input with hand-computed expected output:

| Variant | Barriers | Conditional writes | Result |
|---|---|---|---|
| Baseline | 1 | No | PASS |
| Count test | 2 | No | **FAIL** |
| Conditional test | 2 | Yes | **FAIL** |
| Real-workload-shaped | 4 | Yes | **FAIL** |

This rules out conditional/divergent control flow as the trigger — the
failure occurs purely as a function of barrier count exceeding 1, with
completely uniform, non-divergent code on both sides of each barrier.

## What was ruled out

- **Not a descriptor/binding issue** — a two-buffer (src/dst) SSBO
  descriptor set with shared-memory tiling and exactly one barrier works
  correctly (confirmed via a separate isolated test).
- **Not a memory-visibility/synchronization issue on the host side** — an
  explicit `VkMemoryBarrier` from `VK_ACCESS_SHADER_WRITE_BIT` to
  `VK_ACCESS_HOST_READ_BIT` before `vkEndCommandBuffer`, in addition to the
  fence wait, was added and made no difference.
- **Not specific to this shader's math** — the failure reproduces
  identically with trivial arithmetic (`x += constant`) that has no
  relation to the original workload (a wavelet transform).
- **Not an API misuse bug on our end (to the best of our ability to
  verify)** — every Vulkan call in the affected path returns `VK_SUCCESS`;
  no validation layer was available on-device to catch anything further,
  which is itself worth flagging as a gap.

## Workaround in use

Splitting each logical stage into its own dispatch (exactly one `barrier()`
each), synchronized between dispatches via `vkCmdPipelineBarrier` (a
command-buffer-level primitive, distinct from in-shader `barrier()`) rather
than fusing multiple stages into one dispatch. This works correctly, at the
cost of significantly more dispatches (and correspondingly higher fixed
per-dispatch overhead — measured at ~1.87ms/dispatch on this device, which
is itself surprisingly high and may be worth a separate look).

## Context

This was found while investigating GPU-compute video codec feasibility on
this TV, not in isolation. To the best of our research, no shipping webOS
application appears to exercise multi-barrier compute shaders (video
decode on this platform is handled by a separate fixed-function hardware
path with no GPU involvement at all), which may explain why this hasn't
surfaced before despite seeming like a fairly fundamental correctness bug.

Happy to provide the full binary, additional shader variants, or run further
tests against this specific device if useful for reproducing or narrowing
this down further.
