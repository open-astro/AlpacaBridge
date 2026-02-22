@echo off
setlocal EnableExtensions EnableDelayedExpansion

for %%I in ("%~dp0.") do set "ROOT_DIR=%%~fI"
set "CORE_DIR=%ROOT_DIR%\AlpacaCore"
set "HTTP_DIR=%ROOT_DIR%\AlpacaHTTP"
set "CORE_BUILD_DIR=%CORE_DIR%\build"
set "HTTP_BUILD_DIR=%HTTP_DIR%\build"
if "%ALPACAHTTP_USE_BOOST_BEAST%"=="" set "ALPACAHTTP_USE_BOOST_BEAST=OFF"
if "%ALPACACORE_ENABLE_ALL_VENDORS%"=="" set "ALPACACORE_ENABLE_ALL_VENDORS=ON"
if "%ALPACABRIDGE_ENABLE_VCPKG%"=="" set "ALPACABRIDGE_ENABLE_VCPKG=ON"

if not exist "%CORE_DIR%" (
  echo AlpacaCore not found at %CORE_DIR%
  exit /b 1
)

if not exist "%HTTP_DIR%" (
  echo AlpacaHTTP not found at %HTTP_DIR%
  exit /b 1
)

set "USE_VCPKG=0"
if /I "%ALPACABRIDGE_ENABLE_VCPKG%"=="ON" if "%VCPKG_ROOT%"=="" if not "%USERPROFILE%"=="" set "VCPKG_ROOT=%USERPROFILE%\vcpkg"
if /I "%ALPACABRIDGE_ENABLE_VCPKG%"=="ON" if "%VCPKG_ROOT%"=="" set "VCPKG_ROOT=%HOMEDRIVE%%HOMEPATH%\vcpkg"
if /I "%ALPACABRIDGE_ENABLE_VCPKG%"=="ON" (
  set "VCPKG_EXE=!VCPKG_ROOT!\vcpkg.exe"
  set "VCPKG_TOOLCHAIN=!VCPKG_ROOT!\scripts\buildsystems\vcpkg.cmake"

  if not exist "!VCPKG_EXE!" (
    where git >nul 2>&1
    if errorlevel 1 (
      echo Git is required to bootstrap vcpkg.
      echo Install Git for Windows, or set VCPKG_ROOT to an existing vcpkg install.
      exit /b 1
    )

    if not exist "!VCPKG_ROOT!" (
      echo == Installing vcpkg to !VCPKG_ROOT! ==
      git clone https://github.com/microsoft/vcpkg "!VCPKG_ROOT!"
      if errorlevel 1 exit /b 1
    )

    echo == Bootstrapping vcpkg ==
    call "!VCPKG_ROOT!\bootstrap-vcpkg.bat"
    if errorlevel 1 exit /b 1
  )

  if "!VCPKG_DEFAULT_TRIPLET!"=="" set "VCPKG_DEFAULT_TRIPLET=x64-windows"
  set "USE_VCPKG=1"
  echo == Using vcpkg !VCPKG_DEFAULT_TRIPLET! ==
)

if exist "%CORE_BUILD_DIR%" rmdir /s /q "%CORE_BUILD_DIR%"
if exist "%HTTP_BUILD_DIR%" rmdir /s /q "%HTTP_BUILD_DIR%"

set "BUILD_CONFIG_ARG="
set "CTEST_CONFIG_ARG="
if "%ALPACA_BUILD_CONFIG%"=="" set "ALPACA_BUILD_CONFIG=Debug"
set "BUILD_CONFIG_ARG=--config %ALPACA_BUILD_CONFIG%"
set "CTEST_CONFIG_ARG=-C %ALPACA_BUILD_CONFIG%"

echo == AlpacaCore ==
if "!USE_VCPKG!"=="1" (
  cmake -S "%CORE_DIR%" -B "%CORE_BUILD_DIR%" -DCMAKE_TOOLCHAIN_FILE="!VCPKG_TOOLCHAIN!" -DVCPKG_MANIFEST_DIR="%ROOT_DIR%" -DVCPKG_TARGET_TRIPLET=!VCPKG_DEFAULT_TRIPLET! -DALPACACORE_BUILD_TESTS=ON -DALPACACORE_ENABLE_ALL_VENDORS=%ALPACACORE_ENABLE_ALL_VENDORS%
) else (
  cmake -S "%CORE_DIR%" -B "%CORE_BUILD_DIR%" -DALPACACORE_BUILD_TESTS=ON -DALPACACORE_ENABLE_ALL_VENDORS=%ALPACACORE_ENABLE_ALL_VENDORS%
)
if errorlevel 1 exit /b 1
cmake --build "%CORE_BUILD_DIR%" %BUILD_CONFIG_ARG%
if errorlevel 1 exit /b 1
ctest --test-dir "%CORE_BUILD_DIR%" --output-on-failure %CTEST_CONFIG_ARG%
if errorlevel 1 exit /b 1

echo == AlpacaHTTP ==
if "!USE_VCPKG!"=="1" (
  cmake -S "%HTTP_DIR%" -B "%HTTP_BUILD_DIR%" -DCMAKE_TOOLCHAIN_FILE="!VCPKG_TOOLCHAIN!" -DVCPKG_MANIFEST_DIR="%ROOT_DIR%" -DVCPKG_TARGET_TRIPLET=!VCPKG_DEFAULT_TRIPLET! -DALPACAHTTP_BUILD_TESTS=ON -DALPACAHTTP_USE_BOOST_BEAST=%ALPACAHTTP_USE_BOOST_BEAST% -DALPACACORE_ENABLE_ALL_VENDORS=%ALPACACORE_ENABLE_ALL_VENDORS%
) else (
  cmake -S "%HTTP_DIR%" -B "%HTTP_BUILD_DIR%" -DALPACAHTTP_BUILD_TESTS=ON -DALPACAHTTP_USE_BOOST_BEAST=%ALPACAHTTP_USE_BOOST_BEAST% -DALPACACORE_ENABLE_ALL_VENDORS=%ALPACACORE_ENABLE_ALL_VENDORS%
)
if errorlevel 1 exit /b 1
cmake --build "%HTTP_BUILD_DIR%" %BUILD_CONFIG_ARG%
if errorlevel 1 exit /b 1
ctest --test-dir "%HTTP_BUILD_DIR%" --output-on-failure %CTEST_CONFIG_ARG%
if errorlevel 1 exit /b 1

endlocal
