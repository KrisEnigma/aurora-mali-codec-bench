# Mali GPU / PyroWave feasibility benchmarks (LG G5 webOS TV)

Vulkan compute benchmarks for probing whether a PyroWave-style GPU-compute
codec is viable on this TV's Mali-G510, for Aurora/Vibepollo development.

**Investigation complete.** Start here depending on what you need:

- **[`FINDINGS.md`](FINDINGS.md)** — the full writeup: platform facts, the
  confirmed driver bug, benchmark results, and practical conclusions.
- **[`BUG_REPORT.md`](BUG_REPORT.md)** — self-contained report ready to post
  to ARM's Mali developer community forum (the root cause is a driver bug,
  not anything in this project).
- **[`ISSUE_AURORA.md`](ISSUE_AURORA.md)** — short summary ready to post as
  a GitHub issue on Aurora's own repo, so this doesn't get re-investigated
  from scratch later.

## One-time setup

### Option A — Docker (recommended for Mac, also works on Windows/Linux)

Your Mac won't have an `arm-linux-gnueabi` cross-toolchain readily available via
Homebrew, and hunting for one wastes time. Docker sidesteps that entirely by
using the exact Ubuntu 24.04 + `gcc-arm-linux-gnueabi` setup already confirmed
working:

```sh
docker build -t mali-pyrowave .
# First run: fetch headers into the HOST-mounted vulkan_inc/, not just the
# image's build-time copy — the -v bind mount below replaces /work's
# contents at runtime, which shadows whatever fetch_headers.sh did during
# `docker build`. Skipping this step gives a confusing
# "vulkan/vulkan_core.h: No such file" error.
docker run --rm -v "$(pwd)":/work mali-pyrowave sh -c "./scripts/fetch_headers.sh && make check-glibc"

# Subsequent runs (headers now persist on your host disk, gitignored):
docker run --rm -v "$(pwd)":/work mali-pyrowave make
```

The `-v "$(pwd)":/work` mounts your local checkout into the container, so
built binaries (`dwt_bench_armel`, `mali_vk_bench_armel`) land right back in
your project folder, ready to `scp` to the TV.

### Option B — Native toolchain (Linux, or Windows via WSL2)

Your Linux minibox, or a Windows PC with WSL2 (Ubuntu), can install the
toolchain directly, same as this was originally built:

```sh
sudo apt install gcc-arm-linux-gnueabi glslang-tools
./scripts/fetch_headers.sh
```

WSL2 + Ubuntu behaves identically to a native Linux box for this purpose,
`apt` works the same way, no cross-cross-compilation weirdness.

## Build

```sh
make                    # builds both binaries
make check-glibc        # confirms neither binary needs a newer GLIBC than the TV has (max 2.35)
```

## Run (on the TV, over SSH)

```sh
scp dwt_bench_armel mali_vk_bench_armel root@<tv-ip>:/tmp/
ssh root@<tv-ip>
chmod +x /tmp/dwt_bench_armel /tmp/mali_vk_bench_armel
/tmp/dwt_bench_armel /usr/lib/libmali.so
```

## Layout

```
shaders/            GLSL compute shaders (source of truth)
src/                Host C programs (Vulkan bootstrap + dispatch/timing logic)
scripts/
  fetch_headers.sh   pulls Khronos Vulkan-Headers (needed once)
  embed_shader.sh    compiles a .comp -> SPIR-V -> embedded C header
Makefile             cross-compiles for the TV's actual ABI (arm-linux-gnueabi, softfloat)
FINDINGS.md           platform facts + benchmark results log
```

## Why the build is arm-linux-gnueabi (softfloat), not aarch64 or armhf

Short version: the TV has a 64-bit kernel but a 32-bit softfloat userland.
Full detective work is in FINDINGS.md, but the tell is `/lib/ld-linux.so.3`
existing (the softfloat/armel loader name) rather than
`ld-linux-aarch64.so.1` or `ld-linux-armhf.so.3`.
