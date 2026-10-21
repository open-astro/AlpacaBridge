@echo off
setlocal enabledelayedexpansion

REM One-click configure + build (Release) for this demo.
REM Usage:
REM   build_release.bat
REM Optional env vars:
REM   BUILD_DIR   (default: build)
REM   CONFIG      (default: Release)
REM   TARGETS     (default: test_burst test_single)

where cmake >nul 2>nul
if errorlevel 1 (
  echo [ERROR] cmake not found in PATH.
  exit /b 1
)

if "%BUILD_DIR%"=="" set "BUILD_DIR=build"
if "%CONFIG%"=="" set "CONFIG=Release"
if "%TARGETS%"=="" set "TARGETS=test_burst test_single"

REM Require bundled OpenCV under .\opencv\build (no external dependency/fallback in CMake).
if not exist "%~dp0opencv\build\include\opencv2\core.hpp" (
  echo [ERROR] Bundled OpenCV not found: "%~dp0opencv\build\include\opencv2\core.hpp"
  echo         Please place OpenCV under ".\opencv\build" first, then re-run.
  exit /b 1
)

echo [INFO] Configure: cmake -S . -B "%BUILD_DIR%"
cmake -S . -B "%BUILD_DIR%"
if errorlevel 1 (
  echo [ERROR] CMake configure failed.
  exit /b 1
)

echo [INFO] Build: cmake --build "%BUILD_DIR%" --config "%CONFIG%" --target %TARGETS%
cmake --build "%BUILD_DIR%" --config "%CONFIG%" --target %TARGETS%
if errorlevel 1 (
  echo [ERROR] Build failed.
  exit /b 1
)

echo [OK] Build finished. Output: "%BUILD_DIR%\%CONFIG%\"
exit /b 0

