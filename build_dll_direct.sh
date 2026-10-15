#!/bin/bash
# Build DLL cho Windows x64 bằng MinGW - compile trực tiếp, không cần make
# Chạy: bash build_dll_direct.sh (trong Git Bash)

set -e

CXX="x86_64-w64-mingw32-g++"
OUTPUT_DIR="./build_dll"
UNITY_PLUGIN_DIR="../XiangqiUnity3D/Assets/Plugins/x86_64"

# Compiler flags (giống build_dll_mingw.bat nhưng dùng bash)
CXXFLAGS="-std=c++17 -O3 -DNDEBUG -fPIC -DXIANGQI -DIS_64BIT"
CXXFLAGS="$CXXFLAGS -DNNUE_EMBEDDING_OFF"
CXXFLAGS="$CXXFLAGS -DZSTD_DISABLE_ASM"
# SIMD: Pikafish chọn đường code NNUE theo define USE_*, không tự dò theo -m*. Bản cũ chỉ có -msse4.2 -mpopcnt
# nên NNUE chạy vô hướng (chậm ~2.5x). Cờ dưới khớp Makefile gốc ARCH=x86-64-avx2: cần CPU có AVX2
# (Intel Haswell 2013+, AMD Excavator/Zen 2015+).
CXXFLAGS="$CXXFLAGS -march=x86-64 -mpopcnt -msse2 -mssse3 -msse4.1 -mavx2 -mbmi -funroll-loops"
CXXFLAGS="$CXXFLAGS -DUSE_POPCNT -DUSE_SSE2 -DUSE_SSSE3 -DUSE_SSE41 -DUSE_AVX2"
CXXFLAGS="$CXXFLAGS -I./src -I./src/nnue -I./src/nnue/features -I./src/nnue/layers"
CXXFLAGS="$CXXFLAGS -I./src/external/common -I./src/external/decompress"
CXXFLAGS="$CXXFLAGS -DUSE_PTHREADS"

mkdir -p "$OUTPUT_DIR"

# Thu thập tất cả .cpp files (trừ main.cpp)
SOURCES=$(find ./src -path ./src/universal -prune -o -name "*.cpp" ! -name "main.cpp" -print | sort)
SOURCES="$SOURCES ./c_api/pikafish_c.cpp"

echo "=== Compiling source files ==="
OBJ_FILES=()

for src in $SOURCES; do
    obj_name=$(echo "${src#./}" | sed 's#/#_#g; s#\.cpp$##')_win.o
    obj_path="$OUTPUT_DIR/$obj_name"
    echo "  $src -> $obj_path"
    "$CXX" $CXXFLAGS -c "$src" -o "$obj_path" || { echo "FAILED: $src"; exit 1; }
    OBJ_FILES+=("$obj_path")
done

echo ""
echo "=== Linking pikafish.dll ==="
"$CXX" "${OBJ_FILES[@]}" \
    -shared \
    -o "$OUTPUT_DIR/pikafish.dll" \
    -Wl,--out-implib,"$OUTPUT_DIR/libpikafish.dll.a" \
    -static-libstdc++ -static-libgcc \
    -s \
    -lwinpthread -lws2_32

echo "Done! Size: $(du -sh "$OUTPUT_DIR/pikafish.dll" | cut -f1)"

# Copy sang Unity
if [ -d "$UNITY_PLUGIN_DIR" ]; then
    cp "$OUTPUT_DIR/pikafish.dll" "$UNITY_PLUGIN_DIR/pikafish.dll"
    echo "✓ Copied to $UNITY_PLUGIN_DIR/pikafish.dll"
fi
