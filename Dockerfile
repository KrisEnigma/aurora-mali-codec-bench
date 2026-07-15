FROM ubuntu:24.04

RUN apt-get update && apt-get install -y --no-install-recommends \
    gcc-arm-linux-gnueabi \
    glslang-tools \
    python3 \
    curl \
    make \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /work

# Headers are fetched at build time so the image doesn't need to be rebuilt
# when only source/shader files change (they're cached in a separate layer).
COPY scripts/fetch_headers.sh scripts/fetch_headers.sh
RUN ./scripts/fetch_headers.sh

COPY . .

CMD ["make", "check-glibc"]
