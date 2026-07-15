# Mali GPU / PyroWave feasibility benchmarks (LG G5 webOS TV)

Vulkan compute benchmarks for probing whether a PyroWave-style GPU-compute
codec is viable on this TV's Mali-G510, for Aurora/Vibepollo development.

See `FINDINGS.md` for the full writeup of what we know about the platform
and results so far.

## One-time setup

```sh
sudo apt install gcc-arm-linux-gnueabi glslang-tools
./scripts/fetch_headers.sh
```

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
