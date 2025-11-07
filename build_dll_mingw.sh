#!/bin/bash
# Script build DLL cho Pikafish với MinGW trên Windows
# 
# Sử dụng:
#   1. Trong MSYS2: Mở "MinGW64" terminal (không phải MSYS terminal)
#   2. cd vào thư mục PikafishSource
#   3. chạy: bash build_dll_mingw.sh
#
# Hoặc chạy từ bất kỳ đâu: cd /d/PikafishVsEleeye/PikafishSource && bash build_dll_mingw.sh

set -e

# Lưu thư mục hiện tại và cd vào thư mục chứa script
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# Kiểm tra xem có đúng thư mục PikafishSource không
if [ ! -f "src/Makefile" ]; then
    echo "ERROR: Script phải chạy từ thư mục PikafishSource!"
    echo "Current directory: $(pwd)"
    echo "Please cd vào thư mục PikafishSource trước khi chạy script."
    exit 1
fi

# Thư mục gốc
SRC_DIR="src"
C_API_DIR="c_api"
BUILD_DIR="build_dll"

# Kiểm tra và tìm MinGW compiler
CXX_COMPILER=""

# Kiểm tra xem đang chạy trong MSYS2
if [ -n "$MSYSTEM" ]; then
    echo "Detected MSYS2 environment: $MSYSTEM"
    
    # Trong MSYS2, thử tìm compiler theo thứ tự
    if [ "$MSYSTEM" = "MINGW64" ]; then
        # MinGW64 terminal - compiler nằm ở /mingw64/bin
        if command -v "/mingw64/bin/x86_64-w64-mingw32-g++" &> /dev/null; then
            CXX_COMPILER="/mingw64/bin/x86_64-w64-mingw32-g++"
        elif command -v "/mingw64/bin/g++" &> /dev/null; then
            CXX_COMPILER="/mingw64/bin/g++"
        fi
    elif [ "$MSYSTEM" = "MSYS" ]; then
        # MSYS terminal - thử tìm trong các thư mục MinGW
        if command -v "/mingw64/bin/x86_64-w64-mingw32-g++" &> /dev/null; then
            CXX_COMPILER="/mingw64/bin/x86_64-w64-mingw32-g++"
        elif command -v "/mingw64/bin/g++" &> /dev/null; then
            CXX_COMPILER="/mingw64/bin/g++"
        fi
    fi
fi

# Nếu chưa tìm thấy, thử các tên phổ biến trong PATH
if [ -z "$CXX_COMPILER" ]; then
    if command -v "x86_64-w64-mingw32-g++" &> /dev/null; then
        CXX_COMPILER="x86_64-w64-mingw32-g++"
    elif command -v "mingw-w64-x86_64-g++" &> /dev/null; then
        CXX_COMPILER="mingw-w64-x86_64-g++"
    elif command -v "g++" &> /dev/null && [ -n "$MSYSTEM" ]; then
        # Trong MSYS2 có thể dùng g++ trực tiếp (native compiler)
        CXX_COMPILER="g++"
    fi
fi

# Kiểm tra kết quả
if [ -z "$CXX_COMPILER" ]; then
    echo ""
    echo "ERROR: MinGW-w64 compiler not found!"
    echo ""
    if [ -n "$MSYSTEM" ]; then
        echo "Bạn đang chạy trong MSYS2 ($MSYSTEM)."
        echo ""
        echo "Giải pháp:"
        echo "  1. Đảm bảo đã cài: pacman -S mingw-w64-x86_64-gcc"
        echo "  2. Chạy script trong 'MinGW64' terminal (không phải MSYS terminal)"
        echo "     - Mở MSYS2 MinGW64 từ Start Menu"
        echo "     - Hoặc chạy: /mingw64/bin/bash"
        echo "  3. Hoặc thử restart terminal sau khi cài gcc"
    else
        echo "Cần cài MinGW-w64. Các cách:"
        echo "  1. MSYS2: pacman -S mingw-w64-x86_64-gcc"
        echo "  2. MinGW-w64 standalone: https://www.mingw-w64.org/downloads/"
        echo "  3. Hoặc thêm MinGW-w64 vào PATH"
    fi
    echo ""
    echo "Checking common locations..."
    [ -f "/mingw64/bin/x86_64-w64-mingw32-g++.exe" ] && echo "  Found: /mingw64/bin/x86_64-w64-mingw32-g++.exe"
    [ -f "/mingw64/bin/g++.exe" ] && echo "  Found: /mingw64/bin/g++.exe"
    echo ""
    echo "Current PATH:"
    echo "$PATH"
    exit 1
fi

echo "Using compiler: $CXX_COMPILER"

# Clean trước khi build
echo "Cleaning..."
cd "$SRC_DIR"
make clean
cd ..

# Tạo thư mục build
mkdir -p "$BUILD_DIR"
cd "$SRC_DIR"

# Build với MinGW, thêm -fPIC và macro XIANGQI
echo "Building object files with MinGW..."
make clean

# Build executable để đảm bảo TẤT CẢ .o files được tạo (Makefile sẽ build dependencies)
# Ta sẽ exclude main.o khi link DLL sau
echo "Compiling all source files..."
# Thêm -DNDEBUG để disable assertions trong Release build
# KHÔNG thêm -static-libstdc++ vào CXXFLAGS vì sẽ gây thiếu seekpos/seekg symbols
make ARCH=x86-64-modern COMP=mingw CXXFLAGS='-fPIC -DXIANGQI -DNDEBUG' optimize=yes debug=no build

# Copy tất cả .o files sang build_dll (recursive, preserve structure)
echo "Collecting object files..."
cd ..
if [ -d "$BUILD_DIR" ]; then
    find "$BUILD_DIR" -name '*.o' -delete 2>/dev/null || true
fi
cd "$SRC_DIR"

# Tìm và copy tất cả .o files (Makefile tạo .o trong cùng thư mục với source)
find . -name '*.o' -type f -exec cp {} ../"$BUILD_DIR"/ \;

# Kiểm tra số lượng .o files
OBJ_COUNT=$(find ../"$BUILD_DIR" -name '*.o' -type f | wc -l)
echo "Collected $OBJ_COUNT object files"

# Liệt kê một số .o files quan trọng để debug
echo "Checking for key object files:"
[ -f "../$BUILD_DIR/engine.o" ] && echo "  ✓ engine.o" || echo "  ✗ engine.o (MISSING!)"
[ -f "../$BUILD_DIR/misc.o" ] && echo "  ✓ misc.o" || echo "  ✗ misc.o (MISSING!)"
[ -f "../$BUILD_DIR/thread.o" ] && echo "  ✓ thread.o" || echo "  ✗ thread.o (MISSING!)"
[ -f "../$BUILD_DIR/position.o" ] && echo "  ✓ position.o" || echo "  ✗ position.o (MISSING!)"
[ -f "../$BUILD_DIR/search.o" ] && echo "  ✓ search.o" || echo "  ✗ search.o (MISSING!)"

# Copy file từ c_api
cd ../"$C_API_DIR"
# Compile C API nếu chưa có .o
if [ ! -f "pikafish_c.o" ]; then
    echo "Compiling C API..."
    # Thêm -DNDEBUG để disable assertions trong Release build
    $CXX_COMPILER -fPIC -DXIANGQI -DNDEBUG -O3 -std=c++17 -I../src -c pikafish_c.cpp -o pikafish_c.o
fi
cp pikafish_c.o ../"$BUILD_DIR"/

# Link thành DLL (exclude main.o vì main() không cần trong DLL)
cd ../"$BUILD_DIR"
echo "Linking DLL..."
# Exclude main.o nếu có
if [ -f "main.o" ]; then
    rm main.o
fi
# Link DLL với runtime dynamic (/MD tương đương) - phù hợp Unity
# KHÔNG dùng -static-libstdc++ vì MinGW static stdlib thiếu seekpos/seekg implementation
# Dùng dynamic linking (libstdc++-6.dll) - Unity đã có sẵn
$CXX_COMPILER -shared -o pikafish.dll *.o \
    -Wl,--out-implib,libpikafish.dll.a \
    -lpthread -lws2_32

echo "Done! DLL: $BUILD_DIR/pikafish.dll"

