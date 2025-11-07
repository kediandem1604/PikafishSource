@echo off
REM Script build DLL cho Pikafish với MinGW trên Windows
REM Sử dụng: cd vào thư mục PikafishSource trước, rồi chạy: build_dll_mingw.bat
REM Hoặc chạy từ bất kỳ đâu: cd D:\PikafishVsEleeye\PikafishSource && build_dll_mingw.bat

setlocal enabledelayedexpansion

REM Lưu thư mục hiện tại và cd vào thư mục chứa script
set SCRIPT_DIR=%~dp0
cd /d "%SCRIPT_DIR%"

REM Kiểm tra xem có đúng thư mục PikafishSource không
if not exist "src\Makefile" (
    echo ERROR: Script phải chạy từ thư mục PikafishSource!
    echo Current directory: %CD%
    echo Please cd vào thư mục PikafishSource trước khi chạy script.
    exit /b 1
)

set SRC_DIR=src
set C_API_DIR=c_api
set BUILD_DIR=build_dll

REM Kiểm tra và tìm MinGW compiler
set CXX_COMPILER=x86_64-w64-mingw32-g++
where %CXX_COMPILER% >nul 2>&1
if errorlevel 1 (
    echo WARNING: %CXX_COMPILER% not found in PATH
    echo Trying alternative names...
    
    REM Thử các tên khác
    where mingw-w64-x86_64-g++ >nul 2>&1
    if not errorlevel 1 (
        set CXX_COMPILER=mingw-w64-x86_64-g++
        echo Found: %CXX_COMPILER%
    ) else (
        where g++ >nul 2>&1
        if not errorlevel 1 (
            REM Trên MSYS2/MinGW có thể dùng g++ trực tiếp nếu đang ở MSYS2
            echo Checking if running in MSYS2...
            if defined MSYSTEM (
                set CXX_COMPILER=g++
                echo Found: %CXX_COMPILER% ^(MSYS2 MinGW^)
            ) else (
                goto :compiler_not_found
            )
        ) else (
            goto :compiler_not_found
        )
    )
)

echo Using compiler: %CXX_COMPILER%
goto :compiler_check_done

:compiler_not_found
echo.
echo ERROR: MinGW-w64 compiler not found!
echo.
echo Cần cài MinGW-w64. Các cách:
echo   1. MSYS2: pacman -S mingw-w64-x86_64-gcc
echo   2. MinGW-w64 standalone: https://www.mingw-w64.org/downloads/
echo   3. Hoặc thêm MinGW-w64 vào PATH
echo.
echo Current PATH:
echo %PATH%
exit /b 1

:compiler_check_done

REM Clean trước khi build
echo Cleaning...
cd %SRC_DIR%
call make clean
cd ..

REM Tạo thư mục build
if not exist %BUILD_DIR% mkdir %BUILD_DIR%
cd %SRC_DIR%

REM Build với MinGW, thêm -fPIC và macro XIANGQI
echo Building object files with MinGW...
call make clean

REM Makefile build executable nhưng ta chỉ cần .o files
REM Build executable trước để đảm bảo tất cả dependencies được build
REM Sau đó sẽ collect tất cả .o files (trừ main.o)
echo Compiling all source files...
REM Thêm -DNDEBUG để disable assertions trong Release build
REM KHÔNG thêm -static-libstdc++ vào CXXFLAGS (chỉ dùng ở linker) vì:
REM - CXXFLAGS chỉ ảnh hưởng compile time, không ảnh hưởng linking
REM - seekpos/seekg cần dynamic stdlib để có implementation đầy đủ
call make ARCH=x86-64-modern COMP=mingw CXXFLAGS="-fPIC -DXIANGQI -DNDEBUG" optimize=yes debug=no

REM Copy tất cả .o files sang build_dll (recursive)
echo Collecting object files...
cd ..
if exist %BUILD_DIR%\*.o del /q %BUILD_DIR%\*.o
cd %SRC_DIR%

REM Tìm và copy tất cả .o files (Makefile tạo .o trong cùng thư mục với source)
for /r . %%f in (*.o) do copy "%%f" ..\%BUILD_DIR%\ >nul 2>&1

REM Kiểm tra số lượng .o files
set OBJ_COUNT=0
for %%f in (..\%BUILD_DIR%\*.o) do set /a OBJ_COUNT+=1
echo Collected %OBJ_COUNT% object files

REM Liệt kê một số .o files quan trọng để debug
echo Checking for key object files:
if exist "..\%BUILD_DIR%\engine.o" (echo   [OK] engine.o) else (echo   [MISSING] engine.o!)
if exist "..\%BUILD_DIR%\misc.o" (echo   [OK] misc.o) else (echo   [MISSING] misc.o!)
if exist "..\%BUILD_DIR%\thread.o" (echo   [OK] thread.o) else (echo   [MISSING] thread.o!)
if exist "..\%BUILD_DIR%\position.o" (echo   [OK] position.o) else (echo   [MISSING] position.o!)
if exist "..\%BUILD_DIR%\search.o" (echo   [OK] search.o) else (echo   [MISSING] search.o!)

REM Compile C API
cd ..\%C_API_DIR%
echo Compiling C API...
if exist pikafish_c.o del pikafish_c.o
REM Thêm -DNDEBUG để disable assertions trong Release build
if not exist pikafish_c.cpp (
    echo ERROR: pikafish_c.cpp not found in %C_API_DIR%
    cd ..
    exit /b 1
)
%CXX_COMPILER% -fPIC -DXIANGQI -DNDEBUG -O3 -std=c++17 -I../src -c pikafish_c.cpp -o pikafish_c.o
if not exist pikafish_c.o (
    echo ERROR: Failed to compile pikafish_c.cpp
    cd ..
    exit /b 1
)
copy pikafish_c.o %BUILD_DIR%\ >nul 2>&1

REM Link thành DLL (exclude main.o vì main() không cần trong DLL)
cd ..\%BUILD_DIR%
echo Linking DLL...
REM Exclude main.o nếu có
if exist main.o del main.o
REM Link DLL với runtime tĩnh để không phụ thuộc vào DLL ngoài
REM Lưu ý: Nếu gặp lỗi thiếu symbol với seekg/seekpos, hãy bỏ 2 cờ static dưới đây quay lại dynamic
%CXX_COMPILER% -shared -o pikafish.dll *.o ^
    -Wl,--out-implib,libpikafish.dll.a ^
    -static-libstdc++ -static-libgcc ^
    -lwinpthread -lws2_32

echo Done! DLL: %BUILD_DIR%\pikafish.dll
cd ..

