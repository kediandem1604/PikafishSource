#!/bin/bash
# Build script để tạo libpikafish.so cho Android ARM64
# Yêu cầu: Android NDK đã được cài đặt

set -e

# Cấu hình NDK path (thay đổi theo máy bạn)
if [ -z "$ANDROID_NDK" ]; then
    # Tự động tìm NDK trong Android SDK
    if [ -d "$HOME/AppData/Local/Android/Sdk/ndk" ]; then
        # Windows (Git Bash)
        NDK_DIR="$HOME/AppData/Local/Android/Sdk/ndk"
    elif [ -d "$HOME/Library/Android/sdk/ndk" ]; then
        # macOS
        NDK_DIR="$HOME/Library/Android/sdk/ndk"
    elif [ -d "$HOME/Android/Sdk/ndk" ]; then
        # Linux
        NDK_DIR="$HOME/Android/Sdk/ndk"
    else
        echo "Error: ANDROID_NDK not set and cannot find NDK automatically"
        echo "Please set ANDROID_NDK environment variable or install Android NDK"
        exit 1
    fi
    
    # Tìm NDK version mới nhất
    LATEST_NDK=$(ls -1 "$NDK_DIR" | sort -V | tail -1)
    ANDROID_NDK="$NDK_DIR/$LATEST_NDK"
fi

echo "Using Android NDK: $ANDROID_NDK"

# Toolchain paths
TOOLCHAIN="$ANDROID_NDK/toolchains/llvm/prebuilt"
if [ -d "$TOOLCHAIN/windows-x86_64" ]; then
    TOOLCHAIN="$TOOLCHAIN/windows-x86_64"
elif [ -d "$TOOLCHAIN/darwin-x86_64" ]; then
    TOOLCHAIN="$TOOLCHAIN/darwin-x86_64"
elif [ -d "$TOOLCHAIN/linux-x86_64" ]; then
    TOOLCHAIN="$TOOLCHAIN/linux-x86_64"
else
    echo "Error: Cannot find toolchain in $TOOLCHAIN"
    exit 1
fi

CXX="$TOOLCHAIN/bin/aarch64-linux-android21-clang++"
CC="$TOOLCHAIN/bin/aarch64-linux-android21-clang"
SYSROOT="$TOOLCHAIN/sysroot"

if [ ! -f "$CXX" ]; then
    echo "Error: C++ compiler not found at $CXX"
    exit 1
fi

echo "Using CXX: $CXX"
echo "Using SYSROOT: $SYSROOT"

# Compiler flags
CXXFLAGS="-std=c++17 -O3 -DNDEBUG -fPIC -fvisibility=hidden"
CXXFLAGS="$CXXFLAGS -I$SYSROOT/usr/include"
CXXFLAGS="$CXXFLAGS -I./src -I./src/nnue -I./src/nnue/features -I./src/nnue/layers"
CXXFLAGS="$CXXFLAGS -I./src/external/common -I./src/external/decompress"
CXXFLAGS="$CXXFLAGS -DANDROID -DIS_64BIT -DNNUE_EMBEDDING_OFF"

# SIMD: Pikafish chọn đường code NNUE theo define USE_*, KHÔNG tự dò theo -march. Thiếu USE_NEON thì NNUE chạy code
# vô hướng (bản ship tới 2026-10 bị vậy: chậm ~2.5x ở cùng độ sâu). Cờ dưới đây khớp Makefile gốc (ARCH=armv8 /
# armv8-dotprod). Bản mặc định chạy được trên mọi CPU arm64; bản dotprod chỉ cho ARMv8.2+ có lệnh dot product.
VARIANT="${1:-armv8}"
case "$VARIANT" in
    armv8)         CXXFLAGS="$CXXFLAGS -march=armv8-a+simd -DUSE_POPCNT -DUSE_NEON=8" ;;
    armv8-dotprod) CXXFLAGS="$CXXFLAGS -march=armv8.2-a+dotprod -DUSE_POPCNT -DUSE_NEON=8 -DUSE_NEON_DOTPROD" ;;
    *) echo "Unknown variant '$VARIANT' (armv8 | armv8-dotprod)"; exit 1 ;;
esac
CXXFLAGS="$CXXFLAGS -funroll-loops -flto"

# Linker flags
LDFLAGS="-shared -fPIC -llog -latomic -flto -O3"
LDFLAGS="$LDFLAGS -Wl,-soname,libpikafish.so"

# Output directory
OUTPUT_DIR="./build_android/$VARIANT"
mkdir -p "$OUTPUT_DIR"

# Source files từ c_api
C_API_SRC="./c_api/pikafish_c.cpp"

# Source files từ src (cần compile tất cả .cpp files)
SRC_DIR="./src"

# Collect all source files (dùng đường dẫn tương đối từ root project)
SOURCES=$(find "$SRC_DIR" -path "$SRC_DIR/universal" -prune -o -name "*.cpp" ! -name "main.cpp" -print | sort)
# Thêm c_api wrapper
SOURCES="$SOURCES ./c_api/pikafish_c.cpp"

echo "Compiling source files..."
OBJ_FILES=()

for src in $SOURCES; do
    obj_name=$(echo "${src#./}" | sed 's#/#_#g; s#\.cpp$##').o
    obj_path="$OUTPUT_DIR/$obj_name"
    
    echo "  Compiling $src -> $obj_path"
    "$CXX" $CXXFLAGS -c "$src" -o "$obj_path" || exit 1
    OBJ_FILES+=("$obj_path")
done

echo "Linking libpikafish.so..."
"$CXX" "${OBJ_FILES[@]}" $LDFLAGS -o "$OUTPUT_DIR/libpikafish.so" || exit 1

# Copy to Unity project
UNITY_PLUGIN_DIR="../XiangqiUnity3D/Assets/Plugins/Android/arm64-v8a"
if [ "$VARIANT" != "armv8" ]; then
    echo "Variant $VARIANT: not copied to Unity (the app loads libpikafish.so only)."
elif [ -d "$UNITY_PLUGIN_DIR" ]; then
    echo "Copying to Unity project..."
    cp "$OUTPUT_DIR/libpikafish.so" "$UNITY_PLUGIN_DIR/"
    echo "✓ Copied to $UNITY_PLUGIN_DIR/libpikafish.so"
else
    echo "Warning: Unity plugin directory not found: $UNITY_PLUGIN_DIR"
    echo "Built library is at: $OUTPUT_DIR/libpikafish.so"
fi

echo ""
echo "Build successful!"
echo "Output: $OUTPUT_DIR/libpikafish.so"
if [ -f "$OUTPUT_DIR/libpikafish.so" ]; then
    ls -lh "$OUTPUT_DIR/libpikafish.so"
fi

