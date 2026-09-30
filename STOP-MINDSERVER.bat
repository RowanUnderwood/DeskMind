@echo off
rem Stop MindServer, ComfyUI (port 8188) and NInfer (terminates its WSL distro).
setlocal
echo Stopping MindServer (port 8286) and ComfyUI (port 8188)...
powershell -NoProfile -Command "foreach ($p in 8286, 8188) { Get-NetTCPConnection -LocalPort $p -State Listen -ErrorAction SilentlyContinue | ForEach-Object { Stop-Process -Id $_.OwningProcess -Force -ErrorAction SilentlyContinue } }"
call "H:\Ninfer Qwen\stop-ninfer.bat"
endlocal
