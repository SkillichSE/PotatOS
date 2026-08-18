@echo off
if "%~1"=="__RUNSERVER__" goto :runserver

setlocal enabledelayedexpansion
title GLaDOS Launcher
color 0E

set "PYTHON_EXE=C:\Users\Ardor\AppData\Local\Python\bin\python.exe"
set "SERVER_SCRIPT=G:\projects\PotatOS\main\server\server.py"
set "LMSTUDIO_EXE=C:\Users\Ardor\AppData\Local\Programs\LM Studio\LM Studio.exe"
set "TAILSCALE_EXE=tailscale"
set "FUNNEL_HOST=steellegend.taila511f4.ts.net"
set "LOCAL_WS_PORT=8765"
set "LMSTUDIO_URL=http://localhost:1234/v1/models"

set "LOG_DIR=%~dp0logs"
if not exist "%LOG_DIR%" mkdir "%LOG_DIR%"
set "SERVER_LOG=%LOG_DIR%\server_%RANDOM%.log"

echo STARTING GLaDOS  -  %DATE% %TIME%
echo.

echo [0/4] Stopping leftover processes (if any)...
taskkill /F /T /FI "WINDOWTITLE eq GLaDOS Server - LIVE LOG*" >nul 2>&1
powershell -NoProfile -Command "Get-CimInstance Win32_Process | Where-Object { $_.CommandLine -like '*server.py*' } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }" >nul 2>&1
powershell -NoProfile -Command "Get-CimInstance Win32_Process | Where-Object { $_.CommandLine -like '*Tee-Object*' } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }" >nul 2>&1
powershell -NoProfile -Command "Get-NetTCPConnection -LocalPort %LOCAL_WS_PORT% -ErrorAction SilentlyContinue | Select-Object -ExpandProperty OwningProcess -Unique | ForEach-Object { Stop-Process -Id $_ -Force -ErrorAction SilentlyContinue }" >nul 2>&1
timeout /t 3 >nul
echo       done.
echo.

echo [1/4] Starting LM Studio (model on localhost:1234)...
tasklist /FI "IMAGENAME eq LM Studio.exe" 2>NUL | find /I "LM Studio.exe" >nul
if errorlevel 1 (
    start "" "%LMSTUDIO_EXE%"
) else (
    echo       LM Studio was already running.
)

set LM_OK=0
for /l %%i in (1,1,60) do (
    curl -s -o nul -w "%%{http_code}" %LMSTUDIO_URL% > "%TEMP%\lmcode.txt" 2>nul
    set /p LMCODE=<"%TEMP%\lmcode.txt"
    if "!LMCODE!"=="200" (
        set LM_OK=1
        goto lm_done
    )
    timeout /t 2 >nul
)
:lm_done
if "!LM_OK!"=="1" (
    echo       [OK] LM Studio server is responding.
) else (
    echo       [FAIL] LM Studio did not respond within 2 minutes!
    echo       Check in LM Studio: Developer -^> Local Server -^> "Serve on Local Network"
    echo       and that meta-llama-3.1-8b-instruct is the default model.
)
echo.

echo [2/4] Checking Tailscale Funnel (%FUNNEL_HOST%)...
set FUNNEL_OK=0
for /l %%i in (1,1,10) do (
    "%TAILSCALE_EXE%" funnel status > "%TEMP%\funnelstatus.txt" 2>nul
    findstr /I "Funnel on" "%TEMP%\funnelstatus.txt" >nul
    if not errorlevel 1 (
        set FUNNEL_OK=1
        goto funnel_done
    )
    echo       Funnel not active yet, trying to bring it back up...
    "%TAILSCALE_EXE%" funnel --bg %LOCAL_WS_PORT% >nul 2>&1
    timeout /t 2 >nul
)
:funnel_done
if "!FUNNEL_OK!"=="1" (
    echo       [OK] Funnel is up: https://%FUNNEL_HOST%
) else (
    echo       [FAIL] Funnel did not come up! Run "tailscale funnel status" manually.
    echo       Common cause: Tailscale service not running, or you're logged out.
)
echo.

echo [3/4] Starting GLaDOS server ^(loading Whisper+TTS, 1-3 minutes^)...
start "GLaDOS Server - LIVE LOG" cmd /k call "%~f0" __RUNSERVER__ "%PYTHON_EXE%" "%SERVER_SCRIPT%" "%SERVER_LOG%"

set SERVER_OK=0
for /l %%i in (1,1,90) do (
    powershell -NoProfile -Command "if (Get-NetTCPConnection -LocalPort %LOCAL_WS_PORT% -State Listen -ErrorAction SilentlyContinue) { 'READY' } else { 'WAIT' }" > "%TEMP%\portcheck.txt" 2>nul
    findstr /I "READY" "%TEMP%\portcheck.txt" >nul
    if not errorlevel 1 (
        set SERVER_OK=1
        goto server_done
    )
    powershell -NoProfile -Command "if (Get-CimInstance Win32_Process | Where-Object { $_.CommandLine -like '*server.py*' }) { 'ALIVE' } else { 'DEAD' }" > "%TEMP%\procstate.txt" 2>nul
    findstr /I "ALIVE" "%TEMP%\procstate.txt" >nul
    if errorlevel 1 (
        goto server_done
    )
    timeout /t 2 >nul
)
:server_done
if "!SERVER_OK!"=="1" (
    echo       [OK] Server is listening on websocket port %LOCAL_WS_PORT%.
) else (
    echo       [FAIL] Server did not come up. Check the "GLaDOS Server - LIVE LOG" window
    echo       and %SERVER_LOG% for the actual error.
)
echo.

if "!LM_OK!"=="1" if "!FUNNEL_OK!"=="1" if "!SERVER_OK!"=="1" (
    color 0A
    echo.
    echo    ALL READY. GLaDOS IS ONLINE. YOU CAN CLOSE ANYDESK NOW.
    echo.
) else (
    color 0C
    echo.
    echo    PROBLEM DETECTED - CHECK [FAIL] ABOVE AND LOGS IN THE logs FOLDER
    echo.
)
echo.
echo Logs for reference:
echo   %SERVER_LOG%
echo.
echo A separate window titled "GLaDOS Server - LIVE LOG" is now open and
echo shows everything the server hears and replies, live, in real time.
echo Leave that window open to watch it. This launcher window is safe
echo to close - the server and LM Studio keep running regardless, and Tailscale
echo Funnel stays up in the background on its own.
echo.
pause
goto :eof

:runserver
chcp 65001 >nul
"%~2" -u "%~3" 2>&1 | powershell -NoProfile -Command "$input | Tee-Object -FilePath '%~4'"
goto :eof