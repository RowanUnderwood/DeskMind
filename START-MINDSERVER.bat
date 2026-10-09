@echo off
rem ============================================================
rem  MindServer launcher (DeskMind helper)
rem
rem    START-MINDSERVER            NInfer (5090) + ComfyUI (4090) + music worker (3090) + MindServer
rem    START-MINDSERVER noqwen     ComfyUI + music worker + MindServer (no chat)
rem    START-MINDSERVER server     MindServer only (tests, dithering)
rem
rem  NInfer starts first because it is the pickiest about free
rem  VRAM (it needs ~22 GB free on GPU 0, the display GPU).
rem  Each service gets its own window; STOP-MINDSERVER.bat ends them.
rem  First time on this PC: run setup-firewall.bat once (admin).
rem ============================================================
setlocal
set "HERE=%~dp0"
set "MODE=%~1"
set "PY=%HERE%helper\.venv\Scripts\python.exe"

if not exist "%PY%" (
    echo MindServer's Python environment is missing: %PY%
    echo Create it with:  py -3.13 -m venv helper\.venv
    echo and install:     helper\.venv\Scripts\pip install numpy pillow httpx websocket-client PySide6 git+https://github.com/hbldh/hitherdither
    pause
    exit /b 1
)

if /i "%MODE%"=="server" goto server
if /i "%MODE%"=="noqwen" goto comfy

rem Skip services that already answer (a second copy would fail on its port / database)
rem The Qwen model (thinkingcap or full) is MindServer's ninfer_model setting;
rem launch-ninfer.bat takes a WIN_MODEL preset from the environment.
pushd "%HERE%helper"
call :alive http://127.0.0.1:1234/health
if not errorlevel 1 (
    echo NInfer is already running - not starting it again.
    "%PY%" -m mindserver.services check
) else (
    for /f "usebackq delims=" %%m in (`"%PY%" -m mindserver.services model`) do set "WIN_MODEL=%%m"
    echo Starting NInfer ^(Qwen, RTX 5090^)...
    start "NInfer (5090)" cmd /c call "H:\Ninfer Qwen\launch-ninfer.bat"
    timeout /t 5 /nobreak >nul
)
popd

:comfy
call :alive http://127.0.0.1:8188/system_stats
if not errorlevel 1 (
    echo ComfyUI is already running - not starting it again.
) else (
    echo Starting ComfyUI ^(Krea2, RTX 4090^)...
    start "ComfyUI (4090)" cmd /c call "%HERE%run_comfy_image.bat"
)

rem The music worker (MIDI-GPT on the RTX 3090) composes DeskMind's songs
set "MUSIC=%HERE%Labtext2midi\launch-music-worker.bat"
call :alive http://127.0.0.1:8287/health
if not errorlevel 1 (
    echo The music worker is already running - not starting it again.
) else if exist "%MUSIC%" (
    echo Starting the music worker ^(MIDI-GPT, RTX 3090^)...
    start "Music worker (3090)" cmd /c call "%MUSIC%"
) else (
    echo No music worker in Labtext2midi - DeskMind can play songs but not compose new ones.
)

:server
call :alive http://127.0.0.1:8286/ping
if not errorlevel 1 (
    echo MindServer is already running - not starting it again.
    goto :eof
)
echo Starting MindServer on port 8286...
start "MindServer" /d "%HERE%helper" "%PY%" main.py
endlocal
goto :eof

rem errorlevel 0 if the URL answers with HTTP 200 within 2 seconds
:alive
powershell -NoProfile -Command "try { if ((Invoke-WebRequest -UseBasicParsing -TimeoutSec 2 '%~1').StatusCode -eq 200) { exit 0 } } catch {}; exit 1"
exit /b %ERRORLEVEL%
