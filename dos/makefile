# DeskMind DOS build (Open Watcom wmake).  Run through build.bat, which
# starts wmake inside dos\obj so all paths here are relative to obj\.

MTCP = ..\..\tools\mtcp\mTCP-src_2025-01-10
OUT  = ..\out

# Same code-generation options as mTCP's own apps (-zp2 must match the
# library).  -0 = 8086 code, so the EXEs also run on 86Box's Tandy SL/2.
CFLAGS = -zq -bt=dos -ml -0 -s -oh -ok -ot -oa -ei -zp2 -zpw -ob -ol+ -oi+ &
         -DCFG_H="dmtcp.cfg" -i=..\src -i=$(MTCP)\TCPINC -i=$(MTCP)\INCLUDE

TCPOBJS = packet.obj arp.obj eth.obj ip.obj tcp.obj tcpsockm.obj udp.obj &
          utils.obj dns.obj timer.obj ipasm.obj trace.obj
LIBOBJS = video.obj fonthi.obj mouse.obj sound.obj music.obj jukebox.obj sys.obj gui.obj cfg.obj tpi.obj slide.obj
APPOBJS = app.obj scr_chat.obj scr_create.obj scr_gallery.obj scr_music.obj deskmind.obj

# Header changes: .AUTODEPEND once missed a struct change in gui.h (stale guitest.obj,
# garbage on screen), and listing headers as object dependencies stopped wmake from
# noticing .cpp changes.  So: any header newer than hdrs.chk deletes all objects.
HDRS = ..\src\video.h ..\src\mouse.h ..\src\sound.h ..\src\sys.h ..\src\gui.h &
       ..\src\cfg.h ..\src\tpi.h ..\src\net.h ..\src\app.h ..\src\scr.h ..\src\slide.h ..\src\music.h ..\src\jukebox.h ..\src\dmtcp.cfg

LINK = wlink system dos option quiet option map option eliminate option stack=8192

.cpp: ..\src;..\spike;$(MTCP)\TCPLIB
.asm: $(MTCP)\TCPLIB

all: hdrs.chk $(OUT)\DESKMIND.EXE $(OUT)\SLIDES.EXE $(OUT)\GUITEST.EXE $(OUT)\V640.EXE $(OUT)\NETTEST.EXE $(OUT)\SNDTEST.EXE $(OUT)\MUSTEST.EXE $(OUT)\SAVETEST.EXE .SYMBOLIC

hdrs.chk: $(HDRS)
  @if exist *.obj del *.obj
  @echo headers changed: full rebuild > hdrs.chk

$(OUT)\DESKMIND.EXE: $(TCPOBJS) net.obj $(LIBOBJS) $(APPOBJS)
  $(LINK) name $@ file { $< }

$(OUT)\SLIDES.EXE: video.obj fonthi.obj mouse.obj music.obj jukebox.obj sys.obj cfg.obj tpi.obj slide.obj slides.obj
  $(LINK) name $@ file { $< }

$(OUT)\GUITEST.EXE: $(TCPOBJS) net.obj $(LIBOBJS) guitest.obj
  $(LINK) name $@ file { $< }

$(OUT)\V640.EXE: video.obj fonthi.obj mouse.obj sound.obj v640.obj
  $(LINK) name $@ file { $< }

$(OUT)\NETTEST.EXE: $(TCPOBJS) net.obj nettest.obj
  $(LINK) name $@ file { $< }

$(OUT)\SNDTEST.EXE: $(TCPOBJS) net.obj video.obj fonthi.obj sound.obj sys.obj sndtest.obj
  $(LINK) name $@ file { $< }

$(OUT)\MUSTEST.EXE: video.obj fonthi.obj music.obj sys.obj mustest.obj
  $(LINK) name $@ file { $< }

$(OUT)\SAVETEST.EXE: $(LIBOBJS) savetest.obj
  $(LINK) name $@ file { $< }

# sound.cpp holds an interrupt handler that runs on other programs' stacks: -zu (SS is not
# DGROUP) makes every function there reach its data through DS.  Without it the helpers
# used SS, and ticks inside DOS, the disk BIOS or the packet driver corrupted foreign memory.
sound.obj: ..\src\sound.cpp .AUTODEPEND
  wpp $[@ $(CFLAGS) -zu

# music.cpp: the IRQ0 song player, same reason
music.obj: ..\src\music.cpp .AUTODEPEND
  wpp $[@ $(CFLAGS) -zu

.cpp.obj: .AUTODEPEND
  wpp $[@ $(CFLAGS)

.asm.obj:
  wasm -zq -0 -ml $[@

clean: .SYMBOLIC
  @if exist *.obj del *.obj
  @if exist *.map del *.map
  @if exist *.err del *.err
