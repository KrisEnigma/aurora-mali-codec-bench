# Investigated: GPU-compute video codec (PyroWave-style) on Mali — not currently viable

Opening this as a documentation issue rather than a bug or feature request,
so this doesn't get re-investigated from scratch by the next person curious
about it (myself included, in six months).

## Short version

Looked into whether a PyroWave-style GPU-compute codec could give a
meaningful latency win over the current HEVC hardware decode path on LG
webOS TVs (Mali-G510, driver 46.0.0 tested specifically). Conclusion: **not
currently viable**, for a specific, well-isolated reason — not because the
GPU itself is too slow.

## What we found

1. **Confirmed driver bug:** any Vulkan compute shader with more than one
   `barrier()` call silently produces zero output on this Mali/driver
   combination, independent of conditional logic. No API error surfaces
   anywhere. Full diagnostic matrix and repro in the linked repo below.
2. **The actual per-pixel compute cost is fine** — roughly comparable to
   existing decode (~11ms vs ~8.5ms at 1080p). The problem is the
   workaround this bug forces: splitting a 4-stage transform into 40
   separate dispatches instead of 8 fused ones, and per-dispatch overhead
   on this driver turned out to be surprisingly high (~1.87ms fixed cost
   each), which is what actually makes the total 10x worse than decode.
3. **Cross-validated via both Vulkan and OpenGL ES** independently — same
   result both times, ruling out an API-specific bug in our test harness.
4. **Why this probably hasn't come up before:** checked Aurora's and
   `ss4s`'s actual source — video decode on webOS never touches the GPU at
   all (`SS4S_VIDEO_CAP_TRANSFORM_UI_COMPOSITING`, direct hardware overlay
   plane). No shipping webOS app exercises multi-barrier compute shaders,
   so there's been no reason for this bug to surface.

## Full writeup, tools, and repro

Everything (findings doc, benchmark source, the diagnostic suite that
isolated the bug, build instructions) is here:
**https://github.com/KrisEnigma/aurora-mali-codec-bench**

Also filed upstream with ARM's Mali developer community, since the root
cause is a driver bug, not anything on Aurora's side:
`[link once posted]`

## Bottom line for this project

The existing pipeline (NVENC host encode, HEVC hardware decode + the
existing 1-reference-frame tuning, direct-to-overlay display) is already
close to the practical optimum for this hardware generation. This isn't a
dead end from lack of effort, it's a genuine driver-maturity gap: no
shipping webOS app has needed sustained GPU compute before, so nobody's hit
this. Worth revisiting if/when ARM ships a driver update, or if Panfrost/
PanVK ever gets solid support for this Mali generation, but not something
actionable from Aurora's side today.
