@echo off
setlocal

set "ROOT_DIR=%~dp0"
set "CORE_DIR=%ROOT_DIR%AlpacaCore"
set "HTTP_DIR=%ROOT_DIR%AlpacaHTTP"
if "%ALPACAHTTP_USE_BOOST_BEAST%"=="" set "ALPACAHTTP_USE_BOOST_BEAST=OFF"
if "%ALPACACORE_ENABLE_ALL_VENDORS%"=="" set "ALPACACORE_ENABLE_ALL_VENDORS=ON"

if not exist "%CORE_DIR%" (
  echo AlpacaCore not found at %CORE_DIR%
  exit /b 1
)

if not exist "%HTTP_DIR%" (
  echo AlpacaHTTP not found at %HTTP_DIR%
  exit /b 1
)

if exist "%CORE_DIR%build" rmdir /s /q "%CORE_DIR%build"
if exist "%HTTP_DIR%build" rmdir /s /q "%HTTP_DIR%build"

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
cmake -S "%CORE_DIR%" -B "%CORE_DIR%build" -DALPACACORE_ENABLE_ALL_VENDORS=%ALPACACORE_ENABLE_ALL_VENDORS%
if errorlevel 1 exit /b 1
cmake --build "%CORE_DIR%build" --target clean %BUILD_CONFIG_ARG%
if errorlevel 1 exit /b 1
cmake --build "%CORE_DIR%build" %BUILD_CONFIG_ARG% %PARALLEL_ARG%
if errorlevel 1 exit /b 1

echo == AlpacaHTTP ==
cmake -S "%HTTP_DIR%" -B "%HTTP_DIR%build" -DALPACAHTTP_USE_BOOST_BEAST=%ALPACAHTTP_USE_BOOST_BEAST% -DALPACACORE_ENABLE_ALL_VENDORS=%ALPACACORE_ENABLE_ALL_VENDORS%
if errorlevel 1 exit /b 1
cmake --build "%HTTP_DIR%build" --target clean %BUILD_CONFIG_ARG%
if errorlevel 1 exit /b 1
cmake --build "%HTTP_DIR%build" %BUILD_CONFIG_ARG% %PARALLEL_ARG%
if errorlevel 1 exit /b 1

set "SERVER_EXE=%HTTP_DIR%build\alpacahttp_server.exe"
if exist "%SERVER_EXE%" (
  echo AlpacaHTTP is running. Open http://localhost:6800/ in your browser.
  "%SERVER_EXE%"
  exit /b %ERRORLEVEL%
)

if not "%ALPACA_BUILD_CONFIG%"=="" (
  set "SERVER_EXE=%HTTP_DIR%build\%ALPACA_BUILD_CONFIG%\alpacahttp_server.exe"
  if exist "%SERVER_EXE%" (
    echo AlpacaHTTP is running. Open http://localhost:6800/ in your browser.
    "%SERVER_EXE%"
    exit /b %ERRORLEVEL%
  )
)

echo Could not find alpacahttp_server.exe in %HTTP_DIR%build
exit /b 1

endlocal
