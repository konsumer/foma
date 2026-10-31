@echo off
rem foma on Windows (Docker Desktop): fm1 new NAME | dev | build | shell | image
setlocal
cd /d "%~dp0"
set IMAGE=konsumer/foma-toolchain
if "%PORT%"=="" set PORT=8000
set CMD=%1
if "%CMD%"=="" set CMD=build
if "%CMD%"=="flash" echo fm1 flash: macOS and Linux only for now. On Windows, install from the page: fm1 dev, then its Install tab & goto :eof
docker image inspect %IMAGE% >nul 2>&1 || docker pull --platform linux/amd64 %IMAGE% || docker build --platform linux/amd64 -t %IMAGE% .
if "%CMD%"=="image" docker build --platform linux/amd64 -t %IMAGE% . & goto :eof
if "%CMD%"=="build" docker run --rm --init -it --platform linux/amd64 -v "%CD%:/work" %IMAGE% python3 tools/build.py & goto :eof
if "%CMD%"=="dev" docker run --rm --init -it --platform linux/amd64 -p %PORT%:%PORT% -v "%CD%:/work" %IMAGE% python3 tools/build.py --dev %PORT% & goto :eof
if "%CMD%"=="new" docker run --rm --init -it --platform linux/amd64 -v "%CD%:/work" %IMAGE% python3 tools/new.py %2 %3 %4 %5 %6 & goto :eof
if "%CMD%"=="shell" docker run --rm -it --platform linux/amd64 -v "%CD%:/work" %IMAGE% bash & goto :eof
echo usage: fm1 new NAME ^| dev ^| build ^| shell ^| image
