@echo off
rem Build the DeskMind DOS programs into dos\out\ with the bundled Open Watcom v2.
rem   build          build everything that changed
rem   build clean    delete the object files first

setlocal
set "WATCOM=%~dp0..\tools\ow2"
set "PATH=%WATCOM%\binnt64;%WATCOM%\binnt;%PATH%"
set "INCLUDE=%WATCOM%\h"

if not exist "%~dp0obj" mkdir "%~dp0obj"
if not exist "%~dp0out" mkdir "%~dp0out"

pushd "%~dp0obj"
if /i "%1"=="clean" wmake -h -f ..\makefile clean
wmake -h -f ..\makefile all
set RC=%ERRORLEVEL%
popd
endlocal & exit /b %RC%
