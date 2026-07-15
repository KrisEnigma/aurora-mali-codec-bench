FROM ubuntu:24.04

RUN apt-get update && apt-get install -y --no-install-recommends \
    gcc-arm-linux-gnueabi \
    libc6-dev-armel-cross \
    glslang-tools \
    python3 \
    curl \
    make \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /work

# NOTE: this image intentionally does NOT COPY project files or fetch
# headers at build time. Every real usage bind-mounts the host checkout over
# /work at `docker run` time (`-v "$(pwd)":/work`), which replaces anything
# baked into /work during the build anyway — so a build-time fetch would
# just be silently shadowed and never actually used. Fetch headers and build
# at `docker run` time instead (see README.md).
