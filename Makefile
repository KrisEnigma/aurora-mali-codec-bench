# Cross-compiles for the LG webOS TV's actual userland: 32-bit ARM,
# softfloat EABI (armel), despite the aarch64 kernel. Confirmed via:
#   - /lib/ld-linux.so.3 exists (armel loader name; armhf would be
#     ld-linux-armhf.so.3)
#   - /lib/libc.so.6 and /usr/lib/libmali.so are both ELFCLASS32
#   - max GLIBC symbol version on-device: 2.35
#
# One-time setup (Ubuntu/Debian):
#   sudo apt install gcc-arm-linux-gnueabi glslang-tools
#   ./scripts/fetch_headers.sh
#
# Build:
#   make            # builds both benchmarks
#   make dwt_bench_armel
#   make mali_vk_bench_armel

CC := arm-linux-gnueabi-gcc
CFLAGS := -O2 -std=c11 -D_POSIX_C_SOURCE=200809L -Ivulkan_inc
LDFLAGS := -ldl -static-libgcc
STRIP := arm-linux-gnueabi-strip

.PHONY: all clean check-glibc

all: dwt_bench_armel mali_vk_bench_armel sentinel_test_armel tile_bisect_test_armel

src/dwt_shader_spv.h: shaders/dwt_lifting.comp scripts/embed_shader.sh
	./scripts/embed_shader.sh shaders/dwt_lifting.comp src/dwt_shader_spv.h dwt_lifting

src/wavelet_shader_spv.h: shaders/wavelet_bench.comp scripts/embed_shader.sh
	./scripts/embed_shader.sh shaders/wavelet_bench.comp src/wavelet_shader_spv.h wavelet_bench

dwt_bench_armel: src/dwt_bench.c src/dwt_shader_spv.h
	$(CC) $(CFLAGS) -DSHADER_HEADER='"dwt_shader_spv.h"' -Isrc -o $@ src/dwt_bench.c $(LDFLAGS)
	$(STRIP) $@

mali_vk_bench_armel: src/mali_vk_bench.c src/wavelet_shader_spv.h
	$(CC) $(CFLAGS) -DSHADER_HEADER='"wavelet_shader_spv.h"' -Isrc -o $@ src/mali_vk_bench.c $(LDFLAGS)
	$(STRIP) $@

src/sentinel_shader_spv.h: shaders/sentinel_test.comp scripts/embed_shader.sh
	./scripts/embed_shader.sh shaders/sentinel_test.comp src/sentinel_shader_spv.h sentinel_test

sentinel_test_armel: src/sentinel_test.c src/sentinel_shader_spv.h
	$(CC) $(CFLAGS) -DSHADER_HEADER='"sentinel_shader_spv.h"' -Isrc -o $@ src/sentinel_test.c $(LDFLAGS)
	$(STRIP) $@

src/tile_bisect_shader_spv.h: shaders/tile_bisect.comp scripts/embed_shader.sh
	./scripts/embed_shader.sh shaders/tile_bisect.comp src/tile_bisect_shader_spv.h tile_bisect

tile_bisect_test_armel: src/tile_bisect_test.c src/tile_bisect_shader_spv.h
	$(CC) $(CFLAGS) -DSHADER_HEADER='"tile_bisect_shader_spv.h"' -Isrc -o $@ src/tile_bisect_test.c $(LDFLAGS)
	$(STRIP) $@

# Sanity check: fails the build if a required GLIBC symbol version exceeds
# what the TV actually has (2.35). Run after building.
check-glibc: dwt_bench_armel mali_vk_bench_armel sentinel_test_armel tile_bisect_test_armel
	@for f in $^; do \
		echo "== $$f =="; \
		arm-linux-gnueabi-objdump -T $$f | grep -o 'GLIBC_2\.[0-9]*' | sort -Vu; \
	done

clean:
	rm -f dwt_bench_armel mali_vk_bench_armel sentinel_test_armel tile_bisect_test_armel \
	      src/dwt_shader_spv.h src/wavelet_shader_spv.h src/sentinel_shader_spv.h src/tile_bisect_shader_spv.h
