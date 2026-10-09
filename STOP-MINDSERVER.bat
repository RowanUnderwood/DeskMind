@echo off
rem Stop MindServer, ComfyUI (port 8188), the music worker (8287) and NInfer (stop-ninfer.bat: Windows server and WSL distro).
setlocal
echo Stopping MindServer (port 8286), ComfyUI (port 8188) and the music worker (port 8287)...
powershell -NoProfile -Command "foreach ($p in 8286, 8188, 8287) { Get-NetTCPConnection -LocalPort $p -State Listen -ErrorAction SilentlyContinue | ForEach-Object { Stop-Process -Id $_.OwningProcess -Force -ErrorAction SilentlyContinue } }"
call "H:\Ninfer Qwen\stop-ninfer.bat"
endlocal
