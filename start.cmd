@echo off
setlocal EnableExtensions

REM ---------------------------------------------------------------------------
REM start.cmd - launch the nyaxus daemon and the web dev server together.
REM
REM Usage: start.cmd
REM        (runs from any folder; paths resolve relative to this file)
REM
REM The daemon runs in the background (logs: nyaxus-daemon.log,
REM nyaxus-daemon-err.log) and is stopped automatically when the web
REM server exits (Ctrl+C in this window).
REM ---------------------------------------------------------------------------

set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"

REM mingw runtime libraries for the daemon exe
set "PATH=C:\ProgramData\mingw64\mingw64\bin;%PATH%"

REM A stale daemon from a previous session would win the port; start clean.
taskkill /f /im nyaxus_daemon.exe >nul 2>&1

set "DAEMON=%ROOT%\harness\nyaxus_daemon.exe"
if not exist "%DAEMON%" set "DAEMON=%ROOT%\harness\build-mingw\nyaxus_daemon.exe"
if not exist "%DAEMON%" (
    echo daemon binary not found, looked in harness and harness\build-mingw.
    echo build it first:  cd harness\build-mingw ^&^& mingw32-make nyaxus_daemon -j8
    exit /b 1
)

set "PORT=8787"
if defined NYAXUS_PORT set "PORT=%NYAXUS_PORT%"

echo == daemon ==
echo "%DAEMON%"
start /b "nyaxus-daemon" "%DAEMON%" > "%ROOT%\nyaxus-daemon.log" 2> "%ROOT%\nyaxus-daemon-err.log"

set "TRIES=0"
:wait_health
ping 127.0.0.1 -n 2 >nul
curl -sf "http://127.0.0.1:%PORT%/api/v1/health" >nul 2>&1
if %errorlevel% equ 0 goto healthy
set /a TRIES+=1
if %TRIES% lss 30 goto wait_health
echo daemon did not become healthy, see "%ROOT%\nyaxus-daemon-err.log"
taskkill /f /im nyaxus_daemon.exe >nul 2>&1
exit /b 1

:healthy
echo daemon healthy on :%PORT%

echo == web ==
rem Prod is the daily driver (the dev React build allocates several times
rem more per render and balloons the renderer over long sessions); --dev
rem restores the dev server for development.
set "VITE_HARNESS_MODE=http"
cd /d "%ROOT%\web"
if "%~1"=="--dev" (
    echo mode: dev
    npm run dev
) else (
    echo mode: prod ^(build + preview^)
    call npm run build || (
        echo build failed, use start.cmd --dev for the dev server
        taskkill /f /im nyaxus_daemon.exe >nul 2>&1
        exit /b 1
    )
    npm run preview
)
echo (web server exited)

taskkill /f /im nyaxus_daemon.exe >nul 2>&1
echo daemon stopped
endlocal
