@echo off
rem One-time setup: let the Tandy reach MindServer (TCP 8286) from the local network only.
rem Asks for administrator rights.
net session >nul 2>&1
if errorlevel 1 (
    powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b
)
netsh advfirewall firewall delete rule name="MindServer (DeskMind)" >nul 2>&1
netsh advfirewall firewall add rule name="MindServer (DeskMind)" dir=in action=allow protocol=TCP localport=8286 remoteip=localsubnet profile=any
echo.
echo Done: TCP port 8286 is open to this PC's local subnet.
pause
