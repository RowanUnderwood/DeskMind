#!/bin/sh
# Rebuild vm/dm_test.img: the parent project's 86Box base image + DeskMind test files.
# Run from WSL in the project folder:  sh tools/build_vm.sh
set -e
I=vm/dm_test.img
cp "../86Box/test/base.img" "$I"
export MTOOLS_SKIP_CHECK=1
mcopy -o -i "$I@@32256" vm/files/CONFIG.SYS ::/CONFIG.SYS
mcopy -o -i "$I@@32256" vm/files/AUTOEXEC.BAT ::/AUTOEXEC.BAT
mmd -i "$I@@32256" ::/DMTEST 2>/dev/null || true
for f in vm/files/DMTEST/* dos/out/*.EXE; do [ -f "$f" ] && mcopy -o -i "$I@@32256" "$f" ::/DMTEST/; done
for d in PICS PICSCGA CHATS; do if [ -d vm/files/DMTEST/$d ]; then
  mmd -i "$I@@32256" ::/DMTEST/$d 2>/dev/null </dev/null || true
  for f in vm/files/DMTEST/$d/*; do mcopy -o -i "$I@@32256" "$f" ::/DMTEST/$d/; done
fi; done
mdir -i "$I@@32256" ::/DMTEST
# The AT/CGA VM (vm/dm_at: 286, real CGA card) gets its own copy of the same disk
cp "$I" vm/dm_at.img
