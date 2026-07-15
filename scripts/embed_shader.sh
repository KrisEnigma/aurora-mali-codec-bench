#!/bin/sh
# Usage: ./scripts/embed_shader.sh shaders/dwt_lifting.comp src/shader_spv.h dwt_lifting
# Compiles a .comp shader to SPIR-V and embeds it as a C array named
# <name>_spv / <name>_spv_len in the given output header.
set -e
SRC="$1"
OUT="$2"
NAME="$3"
if [ -z "$SRC" ] || [ -z "$OUT" ] || [ -z "$NAME" ]; then
    echo "usage: $0 <shader.comp> <output.h> <array_name>"
    exit 1
fi
SPV="$(mktemp /tmp/shaderXXXXXX.spv)"
glslangValidator -V "$SRC" -o "$SPV"
python3 - "$SPV" "$OUT" "$NAME" <<'EOF'
import struct, sys
spv_path, out_path, name = sys.argv[1], sys.argv[2], sys.argv[3]
data = open(spv_path, 'rb').read()
words = struct.unpack('<%dI' % (len(data)//4), data)
with open(out_path, 'w') as f:
    f.write(f'// Auto-generated from {name}\n')
    f.write(f'static const uint32_t {name}_spv[] = {{\n')
    for i in range(0, len(words), 8):
        f.write('  ' + ', '.join(str(w) for w in words[i:i+8]) + ',\n')
    f.write('};\n')
    f.write(f'static const uint32_t {name}_spv_len = {len(words)};\n')
EOF
rm -f "$SPV"
echo "Wrote $OUT"
