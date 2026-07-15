# Mali GPU Compute Codec Feasibility on LG webOS TVs — Final Findings

**TL;DR:** A PyroWave-style GPU-compute video codec is not currently viable on
this Mali-G510 / driver 46.0.0 combination. Not because the GPU is too slow —
the actual per-pixel compute cost is roughly comparable to existing hardware
HEVC decode — but because of a confirmed driver bug (any compute shader with
more than one `barrier()` call silently produces zero output) that forces a
workaround architecture whose dispatch overhead alone is ~7x the existing
decode baseline. The existing Aurora + Vibepollo pipeline (hardware HEVC
decode, host NVENC encode) is close to the practical optimum for this
hardware today.

---

## 1. Platform facts

| Item | Value | How found |
|---|---|---|
| TV | LG G5 (webOS) | — |
| Kernel arch | `aarch64` | `uname -m` |
| Userland arch | **32-bit ARM, softfloat (armel)** — despite the 64-bit kernel | `/lib/ld-linux.so.3` is the armel loader name (armhf would be `ld-linux-armhf.so.3`); `libc.so.6` and `libmali.so` both confirmed ELFCLASS32 |
| Max GLIBC symbol version | `GLIBC_2.35` | `strings /lib/libc.so.6 \| grep GLIBC` |
| GPU | Mali-G510 (Valhall architecture) | `vkGetPhysicalDeviceProperties` |
| Driver version | 46.0.0 (`v1.r46p0-01eac1.97e3b3993cfe85dfc8fda5430a365edd`) | `GL_VERSION` string |
| Vulkan API version | 1.3.260 (instance-level); ICD manifest claims 1.3.216 | `vkEnumerateInstanceVersion` vs `mali.json` |
| GLES version | OpenGL ES 3.2 | `GL_VERSION` |
| `libmali.so` location | `/usr/lib/libmali.so` (also bind-mounted into every app's palm jail) | `find / -iname libmali.so \| grep -v jail` |
| Vulkan loader | **Absent system-wide** — no `libvulkan.so*` anywhere | `find / -iname "libvulkan.so*"` |
| Vulkan ICD entry point | Exports `vk_icdGetInstanceProcAddr` (formal ICD interface name), not the plain `vkGetInstanceProcAddr` | direct `dlsym` testing |
| DRM render node | `/dev/dri/card0`, `/dev/dri/renderD128` present, live Mali kernel driver confirmed via `dmesg` | — |

**Practical implication:** any native Vulkan app on this platform must
`dlopen("/usr/lib/libmali.so")` directly and bootstrap via
`vk_icdGetInstanceProcAddr`, since there's no system Vulkan loader. Cross-
compilation must target `arm-linux-gnueabi` (32-bit, softfloat), not
`aarch64-linux-gnu` or `arm-linux-gnueabihf` — both of those were tried first
and failed with, respectively, `wrong ELF class: ELFCLASS64` and an opaque
`internal error` from the loader (an ABI-mismatch symptom, hardfloat binary
vs. softfloat loader).

## 2. The confirmed driver bug

**Root cause:** any compute shader containing more than one `barrier()` call
produces **zero writes to its output buffer** on this Mali-G510 / driver
46.0.0, regardless of whether the surrounding logic is conditional or
unconditional. This is independent of the actual shader math — a trivial
shader doing `x += 1.0; barrier(); x += 2.0;` fails exactly the same way as
the real 4-stage CDF 9/7 wavelet lifting shader did.

### Diagnostic matrix

Built as a single consolidated test (`diag_suite`) so the whole matrix comes
from one run instead of one hypothesis per round-trip. Each variant writes
known input through a shader with a specific barrier count / conditionality,
compared against hand-computed expected output:

| Variant | Barriers | Conditional writes | Result |
|---|---|---|---|
| Baseline | 1 | No | **PASS** |
| Count test | 2 | No | **FAIL** — buffer stayed at pre-dispatch sentinel, no write at all |
| Conditional test | 2 | Yes (`isOdd`) | **FAIL** — same failure mode |
| Real-shader-shaped | 4 | Yes (`isOdd`) | **FAIL** — same failure mode |

**The failure mode is not wrong math or a hang.** It's a complete absence of
any write to the destination buffer, confirmed by pre-filling it with a
distinct sentinel value (`-1.0`) that stays completely untouched after
dispatch. Every Vulkan API call, including `vkWaitForFences`, returns
`VK_SUCCESS`. Nothing in the API surfaces an error. This makes it a
particularly dangerous class of bug to develop against.

### What was ruled out before landing on this

Getting here took several rounds of elimination, each worth keeping as
context for anyone hitting something similar:

1. **Sentinel test** (single binding, no shared memory, no barrier, 1
   dispatch): passed. Confirmed basic dispatch → write → host-readback works
   on this driver at all.
2. **Tile-bisect test** (two-buffer descriptor set, shared-memory tile +
   halo load, exactly 1 barrier, trivial unconditional math): passed.
   Confirmed the two-buffer / shared-memory / halo pattern itself is fine.
3. **No-branch bisect** (real CDF 9/7 coefficients, 4 barriers, `isOdd`
   conditionals, but without the outer `if (mode == 0u) {...} else {...}`
   wrapper the real shader had): failed, identically to the full shader.
   Ruled out the outer mode-branch as the culprit.
4. **Diagnostic suite** (above): isolated it cleanly to barrier count `>1`,
   independent of conditionality.

### Corroborating context

General research on Mali Vulkan drivers confirms a documented history of
synchronization-related bugs, particularly around barrier handling and
image-layout transitions causing tile-hash invalidation issues and
rendering artifacts. This is consistent with — not an isolated fluke next
to — what was found here, though no specific prior report of *this exact*
compute-shader multi-barrier failure was located.

## 3. Why this bug was plausibly never discovered before

Investigated Aurora's actual source (`GuiDev1994/moonlight-tv`) and its
backend library `ss4s` (`mariotaku/ss4s`) directly. Neither uses Vulkan or
GLES compute shaders anywhere. Video decode on webOS bypasses the GPU
entirely:

- `ndl_video.c` sets `capabilities->transform = SS4S_VIDEO_CAP_TRANSFORM_UI_COMPOSITING`
- `FeedVideo` hands raw bitstream data straight to LG's native NDL media
  pipeline — no texture upload, no EGL, no GLES calls anywhere in that file
- Decoded video is composited via a dedicated hardware overlay plane,
  beneath the UI layer, entirely separate from GPU rendering
- The only EGL usage anywhere in `ss4s`'s webOS backend is behind a mock/
  test header, for the UI layer (`lvgl`), not video

**Implication:** existing video decode on this platform is 100%
fixed-function silicon that has never had to share a workload with
sustained GPU compute. A PyroWave-style approach isn't just "a new codec
that happens to hit a Mali quirk" — it introduces a category of workload
(sustained real-time compute shader execution) that this SoC's driver stack
has plausibly never been seriously exercised with by any shipping webOS
application. That's a credible explanation for why a bug this fundamental
could exist, undiscovered, on an otherwise mature, widely-deployed driver.

## 4. The workaround, and what it costs

**Workaround:** split each lifting stage into its own dispatch (exactly 1
`barrier()` each — the proven-safe pattern), chained via buffer ping-pong
using `vkCmdPipelineBarrier` between dispatches. This is a different,
much more standard synchronization primitive than in-shader `barrier()`,
and is not implicated by the bug (command-buffer-level barriers between
separate dispatches are a near-universal pattern; multiple in-shader
barriers within one dispatch are comparatively rare).

**Cost:** this turns 8 fused dispatches (4 levels × 2 axes) into 40
per-stage dispatches (4 levels × 2 axes × 5 stages: predict1/update1/
predict2/update2/scale).

### Benchmark results (1080p, single forward pass, no loops)

| Test | Result |
|---|---|
| Vulkan, staged workaround, 40 dispatches (GPU timestamp) | **85.884 ms** |
| OpenGL ES equivalent, 40 dispatches (wall clock incl. CPU overhead) | **110.734 ms** |
| Vulkan, same 40-dispatch structure, trivial copy instead of lifting math (overhead isolation) | **74.843 ms** |
| → Implied real per-pixel compute cost | **~11 ms** |
| Existing HEVC hardware decode (Aurora, 1 reference frame) | **~8.5 ms** |

Two independent graphics APIs (Vulkan and OpenGL ES), same silicon,
corroborate each other closely — this rules out an API-specific
implementation bug and confirms the GLES path genuinely executed the
transform (output values changed from the constant `0.5` input to real,
non-trivial numbers matching the expected shape).

**The key finding: ~87% of total time (74.8ms of 85.9ms) is pure dispatch/
barrier overhead**, roughly 1.87ms of fixed cost per dispatch — an
extraordinarily high figure by desktop-GPU standards. The actual lifting
compute (~11ms) is only modestly worse than existing decode (~8.5ms), not
the "10x worse" the headline dispatch-bound number initially suggested.

**What this means:** the bottleneck is the workaround's dispatch count,
which is a direct consequence of the barrier bug, not a fundamental limit
of the Mali-G510's compute throughput. A hypothetical fused implementation
(if the barrier bug were fixed) extrapolates to roughly 15-25ms total
(8 dispatches × ~1.87ms overhead + somewhat-more-than-11ms compute due to
redundant memory traffic in a no-shared-memory alternative design) — still
likely slower than existing decode, but a fundamentally different, far less
lopsided picture than the naive 85.9ms number.

## 5. Not independently verified

- **Whether multiple dispatches within one command buffer, synchronized via
  `vkCmdPipelineBarrier`, are fully safe on this driver.** This is an
  extremely standard pattern (unlike multiple in-shader barriers), so it's a
  reasonable working assumption, and the benchmark numbers above don't show
  any sign of a problem — but it was never isolated the way the in-shader
  barrier count was.
- **The exact cause of a hardware watchdog reset** triggered during an
  earlier full-sweep benchmark run (confirmed via `tvpowerd`'s
  `PowerOnReason: cpuAbnormal` and `micomservice`'s
  `power_off_history: OFF_BY_NO_POLLING` in `/var/log/messages`). The
  leading hypothesis is a methodological bug in the benchmark itself: buffer
  contents were never reset between the 15 timed iterations, so repeated
  unbounded forward-only lifting passes (some coefficients have magnitude
  >1) could plausibly diverge into NaN/Inf/extreme-denormal territory,
  and denormal/NaN handling has a well-documented slow path on many GPU
  ALUs. A fix (`vkCmdFillBuffer` reset every iteration) was written and
  shipped, but **the fix was never re-tested against the full multi-
  iteration sweep** — investigation pivoted to safer single-dispatch-chain
  tests instead, given the risk of repeatedly forcing hardware resets. This
  should be treated as an open question, not a closed one, if anyone
  revisits full-sweep benchmarking on real hardware.

## 6. Practical conclusion

For this specific TV and driver version, a GPU-compute video codec
(PyroWave or similar) is not a promising direction. The existing pipeline
(NVENC host-side encode, hardware HEVC decode + 1-reference-frame tuning on
the TV, direct-to-overlay-plane display) is already close to the practical
optimum given the constraints of this hardware generation.

This is specifically a **decode-side (TV)** limitation. Nothing here
suggests anything wrong with **encode-side** (host PC) GPU compute — a
desktop-class GPU with a mature Vulkan driver (e.g., an RTX 3070 Ti) is a
completely different maturity tier of driver support, and PyroWave's
demonstrated sub-millisecond desktop numbers aren't called into question by
anything found in this investigation. The gap is specifically in
TV-class Mali driver maturity for compute workloads, not in the codec
approach itself.

A genuine fix would require either an ARM/LG driver update (unlikely to be
prioritized, since no shipping webOS application currently exercises this
code path) or a sufficiently mature open-source alternative (Panfrost/
PanVK) for this specific Mali generation — both outside the scope of a
single user's ability to fix directly, but worth reporting upstream so the
finding doesn't go undocumented.

## 7. Session note: Vibepollo/Aurora settings review

Separately from the Mali investigation, a full settings review (ABR mode,
NVENC preset, capture method, frame pacing/limiter, TV picture processing)
found every relevant setting already at its optimal value for a wired,
low-packet-loss setup: NVENC preset P1, capture method Automatic, RTSS
async frame limiter, TV game-mode picture processing confirmed clean. ABR
mode (`Low latency`) is a client-side network-adaptive bitrate governor
that hands CBR its target — on a stable wired connection it should sit
inert near the configured bitrate, existing purely as a safety net for
degraded-network scenarios rather than something actively costing latency
today.
