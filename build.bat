@echo off
title Build xpt.asi

echo [1/2] Configuring CMake for 32-bit (x86)...
cmake -B build -A Win32
if %errorlevel% neq 0 (
    echo [ERROR] CMake configuration failed! Need CMake + Visual Studio with C++ workload.
    pause
    exit /b %errorlevel%
)

echo.
echo [2/2] Building in Release mode...
cmake --build build --config Release
if %errorlevel% neq 0 (
    echo [ERROR] Build failed!
    pause
    exit /b %errorlevel%
)

echo.
echo [SUCCESS] xpt.asi is in the game root. Protect it with VMProtect (DLL mode).
pause
