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
if not "%ALPACA_BUILD_CONFIG%"=="" (
  set "BUILD_CONFIG_ARG=--config %ALPACA_BUILD_CONFIG%"
)

set "PARALLEL_ARG="
if not "%NUMBER_OF_PROCESSORS%"=="" (
  set "PARALLEL_ARG=--parallel %NUMBER_OF_PROCESSORS%"
)

echo == AlpacaCore ==
if "!USE_VCPKG!"=="1" (
  cmake -S "%CORE_DIR%" -B "%CORE_BUILD_DIR%" -DCMAKE_TOOLCHAIN_FILE="!VCPKG_TOOLCHAIN!" -DVCPKG_MANIFEST_DIR="%ROOT_DIR%" -DVCPKG_TARGET_TRIPLET=!VCPKG_DEFAULT_TRIPLET! -DALPACACORE_ENABLE_ALL_VENDORS=%ALPACACORE_ENABLE_ALL_VENDORS%
) else (
  cmake -S "%CORE_DIR%" -B "%CORE_BUILD_DIR%" -DALPACACORE_ENABLE_ALL_VENDORS=%ALPACACORE_ENABLE_ALL_VENDORS%
)
if errorlevel 1 exit /b 1
cmake --build "%CORE_BUILD_DIR%" --target clean %BUILD_CONFIG_ARG%
if errorlevel 1 exit /b 1
cmake --build "%CORE_BUILD_DIR%" %BUILD_CONFIG_ARG% %PARALLEL_ARG%
if errorlevel 1 exit /b 1

echo == AlpacaHTTP ==
if "!USE_VCPKG!"=="1" (
  cmake -S "%HTTP_DIR%" -B "%HTTP_BUILD_DIR%" -DCMAKE_TOOLCHAIN_FILE="!VCPKG_TOOLCHAIN!" -DVCPKG_MANIFEST_DIR="%ROOT_DIR%" -DVCPKG_TARGET_TRIPLET=!VCPKG_DEFAULT_TRIPLET! -DALPACAHTTP_USE_BOOST_BEAST=%ALPACAHTTP_USE_BOOST_BEAST% -DALPACACORE_ENABLE_ALL_VENDORS=%ALPACACORE_ENABLE_ALL_VENDORS%
) else (
  cmake -S "%HTTP_DIR%" -B "%HTTP_BUILD_DIR%" -DALPACAHTTP_USE_BOOST_BEAST=%ALPACAHTTP_USE_BOOST_BEAST% -DALPACACORE_ENABLE_ALL_VENDORS=%ALPACACORE_ENABLE_ALL_VENDORS%
)
if errorlevel 1 exit /b 1
cmake --build "%HTTP_BUILD_DIR%" --target clean %BUILD_CONFIG_ARG%
if errorlevel 1 exit /b 1
cmake --build "%HTTP_BUILD_DIR%" %BUILD_CONFIG_ARG% %PARALLEL_ARG%
if errorlevel 1 exit /b 1

set "SERVER_EXE=%HTTP_BUILD_DIR%\alpacahttp_server.exe"
if exist "%SERVER_EXE%" (
  echo AlpacaHTTP is running. Open http://localhost:6800/ in your browser.
  "%SERVER_EXE%"
  exit /b %ERRORLEVEL%
)

if not "%ALPACA_BUILD_CONFIG%"=="" (
  set "SERVER_EXE=%HTTP_BUILD_DIR%\%ALPACA_BUILD_CONFIG%\alpacahttp_server.exe"
  if exist "%SERVER_EXE%" (
    echo AlpacaHTTP is running. Open http://localhost:6800/ in your browser.
    "%SERVER_EXE%"
    exit /b %ERRORLEVEL%
  )
 ) else (
  for %%C in (Debug Release RelWithDebInfo MinSizeRel) do (
    if exist "%HTTP_BUILD_DIR%\%%C\alpacahttp_server.exe" (
      echo AlpacaHTTP is running. Open http://localhost:6800/ in your browser.
      "%HTTP_BUILD_DIR%\%%C\alpacahttp_server.exe"
      exit /b %ERRORLEVEL%
    )
  )
)

echo Could not find alpacahttp_server.exe in %HTTP_BUILD_DIR%
exit /b 1

endlocal
