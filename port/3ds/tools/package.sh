#!/bin/bash
# Package a linked game ELF without the ROM: strip the game data the build
# compiled in from the ROM (tools/romdata.py) and build a .3dsx and/or CIA
# whose romfs holds only the version's table (romdata_eu.bin or romdata_us.bin).
# The player puts their own ROM on the
# SD card (/3ds/StarHaven/baserom.z64), see source/platform/pm3ds_rom.c.
#
# usage: package.sh <game.elf> <outdir> [3dsx] [cia]     (default: both)
#   PM3DS_VERSION  pal (default), us, jp or ique, as for make (ique: its own title ID
#            and icon, cia/icon_ique.icn); universal for the ELF of
#            tools/universal.py (both copies stripped, both tables in romfs)
#   BASEROM  the developer's ROM, only to locate the data (ver/<version>/baserom.z64;
#            universal reads ver/pal and ver/us)
#   MAKEROM  makerom binary (port/3ds/local/bin/makerom)
#   ICON, BANNER  CIA icon.icn / banner.bnr (port/3ds/cia/, from tools/make_branding.py)
#   NAME     output file name (StarHaven)
# The .3dsx step runs 3dsxtool in the papermario-3ds-builder image.
set -euo pipefail
ELF=$(realpath "$1"); OUT=$(realpath -m "$2"); shift 2
WANT="${*:-3dsx cia}"
PORT=$(realpath "$(dirname "$0")/..")
ROOT=$(realpath "$PORT/../..")
export PM3DS_VERSION=${PM3DS_VERSION:-pal}
[ "$PM3DS_VERSION" = universal ] || BASEROM=${BASEROM:-$ROOT/ver/$PM3DS_VERSION/baserom.z64}
MAKEROM=${MAKEROM:-$PORT/local/bin/makerom}
if [ "$PM3DS_VERSION" = ique ]; then ICON=${ICON:-$PORT/cia/icon_ique.icn}; else ICON=${ICON:-$PORT/cia/icon.icn}; fi
BANNER=${BANNER:-$PORT/cia/banner.bnr}
case "$PM3DS_VERSION" in
    pal|universal) NAME=${NAME:-StarHaven} ;;
    jp) NAME=${NAME:-StarHaven_JP} ;;
    ique) NAME=${NAME:-StarHaven_iQue} ;;
    *) NAME=${NAME:-StarHaven_US} ;;
esac
SMDH=$(basename "${ELF%.elf}").smdh            # made by make next to the ELF's twin in port/3ds
[ -f "$PORT/$SMDH" ] || SMDH=PaperMario3DS.smdh

mkdir -p "$OUT/romfs"
rm -f "$OUT/romfs/"*
if [ "$PM3DS_VERSION" = universal ]; then
    # the PAL copy (unprefixed symbols), then the US copy (us_ symbols)
    PM3DS_VERSION=pal ROMDATA_PREFIX= python3 "$PORT/tools/romdata.py" \
        "$ELF" "$ROOT/ver/pal/baserom.z64" "$OUT/$NAME.pal.elf" "$OUT/romfs/romdata_eu.bin"
    PM3DS_VERSION=us ROMDATA_PREFIX=us_ python3 "$PORT/tools/romdata.py" \
        "$OUT/$NAME.pal.elf" "$ROOT/ver/us/baserom.z64" "$OUT/$NAME.elf" "$OUT/romfs/romdata_us.bin"
    rm -f "$OUT/$NAME.pal.elf"
else
    TABLE=romdata_$([ "$PM3DS_VERSION" = pal ] && echo eu || echo "$PM3DS_VERSION").bin
    python3 "$PORT/tools/romdata.py" "$ELF" "$BASEROM" "$OUT/$NAME.elf" "$OUT/romfs/$TABLE"
fi

if [[ " $WANT " == *" 3dsx "* ]]; then
    docker run --rm -v "$OUT:/out" -v "$PORT:/port:ro" papermario-3ds-builder \
        /opt/devkitpro/tools/bin/3dsxtool "/out/$NAME.elf" "/out/$NAME.3dsx" \
        --romfs=/out/romfs "--smdh=/port/$SMDH"
    echo "3dsx: $OUT/$NAME.3dsx ($(stat -c %s "$OUT/$NAME.3dsx") bytes)"
fi
if [[ " $WANT " == *" cia "* ]]; then
    # makerom takes the romfs root from the rsf (RomFs: RootPath: romfs),
    # relative to the working directory
    cp "${RSF:-$PORT/cia/app.rsf}" "$OUT/app.rsf"
    # iQue can't join the EU/US program (a third copy of the game doesn't fit in
    # memory): its own title, installed beside the other one
    if [ "$PM3DS_VERSION" = ique ]; then
        sed -i -e 's/^\(  UniqueId *: *\)0x92B99/\10x92B9A/' \
               -e 's/^\(  ProductCode *: *\)"CTR-H-STHV"/\1"CTR-H-STHQ"/' "$OUT/app.rsf"
        grep -q '0x92B9A' "$OUT/app.rsf" || { echo "package.sh: iQue UniqueId not set in app.rsf"; exit 1; }
    fi
    ver() { sed -n "s/^#define PM3DS_VERSION_$1 *\([0-9]*\).*/\1/p" "$PORT/include/pm3ds_brand.h"; }
    (cd "$OUT" && "$MAKEROM" -f cia -o "$NAME.cia" -elf "$NAME.elf" -rsf app.rsf \
        -icon "$ICON" -banner "$BANNER" -target t -exefslogo \
        -major "$(ver MAJOR)" -minor "$(ver MINOR)" -micro "$(ver MICRO)")
    # makerom drops the minor version: write the whole title version into the ticket and TMD
    python3 "$PORT/tools/cia_title_version.py" "$OUT/$NAME.cia" "$(ver MAJOR)" "$(ver MINOR)" "$(ver MICRO)" || exit 1
    echo "cia: $OUT/$NAME.cia ($(stat -c %s "$OUT/$NAME.cia") bytes)"
fi
