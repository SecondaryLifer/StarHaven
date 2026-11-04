#!/bin/bash
# Generated headers that differ between ROM versions, for a non-PAL build:
# writes port/3ds/gen/<version>/, which the Makefile puts first in the
# include path (the PAL ones live in include/ from earlier milestones).
#
#   message_ids.h, item_data.inc.c   the decomp's own rules (./configure <ver>
#                                     then ninja; PAL has extra messages and one
#                                     item script differs)
#   icon_offsets.h                    only if it isn't PAL's (US: the same)
#   sprite/player.h, sprite/npc/*.h   the port's generators, as for PAL
#
# Needs ver/<ver>/baserom.z64 split first (tools/extract_assets.py).
# usage: gen_version_headers.sh us|jp|ique
set -euo pipefail
VER=${1:?version (us, jp or ique)}
PORT=$(realpath "$(dirname "$0")/..")
ROOT=$(realpath "$PORT/../..")
OUT=$PORT/gen/$VER
export PATH="$ROOT/venv/bin:$PATH" PYTHONPATH="$ROOT/tools/build:$ROOT/tools"

cd "$ROOT"
./configure "$VER" --no-split-assets > /dev/null 2>&1
command ninja "ver/$VER/build/include/message_ids.h" "ver/$VER/build/include/item_data.inc.c" > /dev/null
mkdir -p "$OUT/sprite/npc"
cp "ver/$VER/build/include/message_ids.h" "ver/$VER/build/include/item_data.inc.c" "$OUT/"
# the HUD icons' offsets in their ROM block (tools/build/icons.py): include/ has
# PAL's, which US shares; a version whose icons are laid out otherwise (iQue?)
# gets its own, first in the include path
command ninja "ver/$VER/build/include/icon_offsets.h" > /dev/null
if cmp -s "ver/$VER/build/include/icon_offsets.h" include/icon_offsets.h; then
  rm -f "$OUT/icon_offsets.h"
else
  echo "icon_offsets.h differs from include/ for $VER: using its own"
  cp "ver/$VER/build/include/icon_offsets.h" "$OUT/"
fi
python3 "$PORT/tools/gen_player_sprite_header.py" "assets/$VER/sprite/player.xml" "$OUT/sprite/player.h"
python3 "$PORT/tools/gen_npc_sprite_headers.py" "assets/$VER/sprite/npc.xml" "$OUT/sprite/npc"
echo "headers for $VER in $OUT: $(find "$OUT" -type f | wc -l) files"
