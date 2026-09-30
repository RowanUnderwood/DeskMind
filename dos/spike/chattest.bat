@echo off
rem Build CHATTEST.EXE (needs the objects from dos\build.bat).  Output: dos\out\CHATTEST.EXE
setlocal
set "WATCOM=%~dp0..\..\tools\ow2"
set "PATH=%WATCOM%\binnt64;%WATCOM%\binnt;%PATH%"
set "INCLUDE=%WATCOM%\h"
pushd "%~dp0..\obj"
wpp ..\spike\chattest.cpp -fo=chattest.obj -zq -bt=dos -ml -0 -s -oh -ok -ot -oa -ei -zp2 -zpw -ob -ol+ -oi+ -DCFG_H="dmtcp.cfg" -i=..\src -i=..\spike -i=..\..\tools\mtcp\mTCP-src_2025-01-10\TCPINC -i=..\..\tools\mtcp\mTCP-src_2025-01-10\INCLUDE
wlink system dos option quiet option stack=8192 name ..\out\CHATTEST.EXE file { packet.obj arp.obj eth.obj ip.obj tcp.obj tcpsockm.obj udp.obj utils.obj dns.obj timer.obj ipasm.obj trace.obj net.obj video.obj mouse.obj sound.obj sys.obj gui.obj cfg.obj tpi.obj slide.obj app.obj scr_create.obj scr_gallery.obj chattest.obj }
popd
