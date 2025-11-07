@echo off
REM Copy MinGW runtime DLLs vào Unity Plugins/x86_64/
REM Cần thiết khi build DLL với runtime dynamic (/MD)

set UNITY_PLUGINS=D:\PikafishVsEleeye\XiangqiUnity3D\Assets\Plugins\x86_64

REM Tự động tìm MinGW root
set MINGW_ROOT=
if exist "C:\msys64\mingw64\bin\libgcc_s_seh-1.dll" set MINGW_ROOT=C:\msys64\mingw64
if exist "D:\msys64\mingw64\bin\libgcc_s_seh-1.dll" set MINGW_ROOT=D:\msys64\mingw64
if exist "%USERPROFILE%\msys64\mingw64\bin\libgcc_s_seh-1.dll" set MINGW_ROOT=%USERPROFILE%\msys64\mingw64

REM Nếu vẫn không tìm thấy, thử tìm từ PATH của compiler
if "%MINGW_ROOT%"=="" (
    for /f "tokens=*" %%i in ('where x86_64-w64-mingw32-g++ 2^>nul') do (
        set "COMPILER_PATH=%%i"
        goto :found_compiler
    )
    :found_compiler
    if defined COMPILER_PATH (
        REM Lấy thư mục bin từ compiler path
        for %%p in ("%COMPILER_PATH%") do set "MINGW_BIN=%%~dp"
        set MINGW_ROOT=%MINGW_BIN%..
        if not exist "%MINGW_ROOT%\bin\libgcc_s_seh-1.dll" set MINGW_ROOT=
    )
)

REM Kiểm tra lại
if "%MINGW_ROOT%"=="" (
    echo ERROR: Cannot find MinGW root directory!
    echo Please manually set MINGW_ROOT in this script or install MSYS2
    echo Expected locations:
    echo   C:\msys64\mingw64
    echo   D:\msys64\mingw64
    echo   %%USERPROFILE%%\msys64\mingw64
    exit /b 1
)

echo Found MinGW root: %MINGW_ROOT%

REM Tạo thư mục Unity Plugins nếu chưa có
if not exist "%UNITY_PLUGINS%" mkdir "%UNITY_PLUGINS%"

echo Copying MinGW runtime DLLs...
echo From: %MINGW_ROOT%\bin
echo To: %UNITY_PLUGINS%

REM Copy các runtime DLLs cần thiết
if exist "%MINGW_ROOT%\bin\libgcc_s_seh-1.dll" (
    copy /Y "%MINGW_ROOT%\bin\libgcc_s_seh-1.dll" "%UNITY_PLUGINS%\" >nul 2>&1
    echo   [OK] libgcc_s_seh-1.dll
) else if exist "%MINGW_ROOT%\bin\libgcc_s_dw2-1.dll" (
    copy /Y "%MINGW_ROOT%\bin\libgcc_s_dw2-1.dll" "%UNITY_PLUGINS%\" >nul 2>&1
    echo   [OK] libgcc_s_dw2-1.dll
) else (
    echo   [WARNING] libgcc_s_*.dll not found
)

if exist "%MINGW_ROOT%\bin\libstdc++-6.dll" (
    copy /Y "%MINGW_ROOT%\bin\libstdc++-6.dll" "%UNITY_PLUGINS%\" >nul 2>&1
    echo   [OK] libstdc++-6.dll
) else (
    echo   [WARNING] libstdc++-6.dll not found
)

if exist "%MINGW_ROOT%\bin\libwinpthread-1.dll" (
    copy /Y "%MINGW_ROOT%\bin\libwinpthread-1.dll" "%UNITY_PLUGINS%\" >nul 2>&1
    echo   [OK] libwinpthread-1.dll
) else (
    echo   [WARNING] libwinpthread-1.dll not found
)

echo.
echo Done! Runtime DLLs copied to Unity Plugins.
echo Please reload Unity project and test again.

