# Japanese (Mario Story, NMQJ) and iQue (Chinese) versions

Status: groundwork only. Nothing has been built for JP yet: that needs the
player's Japanese ROM as `ver/jp/baserom.z64` (SHA1 in `ver/jp/checksum.sha1`,
`b9cca3ff260b9ff427d981626b82f96de73586d3`, 40 MB).

The decomp builds JP byte-matching (`ver/jp/`, ~475 `VERSION_JP` blocks in
`src/`), so the game side is there; the work is the port's.

## Done (no ROM needed)

- `make PM3DS_VERSION=jp`: `-DVERSION_JP=1`, `build_jp/`, `PaperMario3DS_jp`.
- `include/pm3ds_rom_layout.h`: JP cart offsets from `ver/jp/splat.yaml`
  (icons, logos, title pages, imgfx, shading, audio, sprites, messages, mapfs),
  game ID `NMQJ`, `save_jp.fla`, `romdata_jp.bin`. `PM3DS_ROM_CRC32` is 0
  (unchecked) until a real dump has been verified.
- `include/ld_addrs.h` takes `ver/jp/build/include/ld_addrs.h`.
- `world_files_jp.mk` (gen_world_files.py): the US list plus
  `world/area_hos/hos_10/narrator.c`.
- `rando_locations_jp.inc` / `rando_shops_jp.inc`: the same spots and shops as US
  (checked); the logic column is US's (the generator gives 99 without `PMR_DIR`).
- Tools take `jp`: gen_include_assets.py (the JP fonts: kana, latin, kanji,
  buttons, menu_kana, menu_latin, palettes), romdata.py, package.sh
  (`StarHaven_JP`), gen_version_headers.sh, rando_locations.py.

## Steps once the ROM is there (as for US: 3ecd03bc5, ecd1ed368, 8aa9d2de1)

1. Split the ROM: `./configure jp`, tools/extract_assets.py for `assets/jp`
   and `ver/jp/build/include/ld_addrs.h`.
2. `tools/gen_version_headers.sh jp` (message_ids.h, item_data.inc.c, sprite headers
   into `gen/jp`), `PM3DS_VERSION=jp tools/gen_include_assets.py`,
   `PM3DS_VERSION=jp tools/gen_link_syms.py` (first link) for `m11_syms_jp.ld`.
3. `make PM3DS_VERSION=jp`, fix what doesn't compile (expect JP-only code paths
   never built for the 3DS: byte-order puns, port wraps that assume PAL/US).
4. Boot in Azahar: logos, title, file select, a New Game, a battle.
5. Fill in `PM3DS_ROM_CRC32` from the verified dump.

## The bottom screen's text (the real work)

The port's bottom screen decodes game messages (`touch_menu.c decode_msg_as`,
`journal.c`) into one byte per glyph of `MsgCharImgNormal` and draws them with
an atlas built from that font (`hud_info.c pm3ds_ui_get_font`,
`bottom_screen.c build_font`). JP has no `MsgCharImgNormal`:

- `MsgCharDataNormal` (msg_data.c, `#if VERSION_JP`) has variants: kana
  (0xC7 glyphs), latin (0x1B), kanji (0x79), plus buttons and the menu fonts,
  all 16x14 at 4 bpp. Text switches the variant with a READ_FUNC code
  (`MSG_READ_FUNC_VARIANT`), and each byte indexes the current variant.
- So the decoder must track the variant and emit (variant, index); the UI's
  strings need more than 256 glyph codes (an escape byte + index, or 16-bit
  strings), and the atlas has ~360 glyphs (fits 512x512 at 16 px cells).
- JP's latin font has only 27 glyphs, so the port's own English labels
  ("Options", "Items", ...) can't come from it: either keep a Latin font of
  our own for those, or translate the labels to Japanese. Decide with the ROM's
  fonts in front of us.

## Packaging

With the EU ROM only ~5 MB of heap is left in the EU+US universal build
(8aa9d2de1). A third code copy may not fit, so JP may ship as its own
`StarHaven_JP.cia` instead of joining the universal one. Measure first.

## iQue (the iQue Player's Chinese release)

Same groundwork as JP: `make PM3DS_VERSION=ique` (`build_ique/`,
`PaperMario3DS_ique`), the cart offsets in `pm3ds_rom_layout.h` (checked
against `ver/ique/splat.yaml`), CRC32 `18C02AD5`, `save_ique.fla`,
`world_files_ique.mk` (the US list minus one file), randomizer tables (as US),
and `ique` in the tools (the one 16x16 Chinese font, 0x9E0 glyphs).
Needs the iQue ROM as `ver/ique/baserom.z64` (SHA1
`5c724685085eba796537573dd6f84aaddedc8582`).

- The iQue dump's header has a blank game ID, so an iQue build also accepts a
  ROM whose ID is four zero bytes (pm3ds_rom.c); the CRC confirms it.
- `filemenu_msg.c`: the iQue glyph path wrote past `filemenu_glyphBuffer`;
  taken from PR #62.
- Values and fixes first worked out by Darth-Koopa in PR #62 (iQue Version
  Support) and their StarHaven_CN fork.
- Ported without a ROM (2026-11-04):
  - The bottom screen in Chinese (`localization/`): Settings > Language >
    简体中文, the default on iQue. The strings are their CSV; the characters
    are built in (Fusion Pixel), so this also works with the US and EU ROMs.
  - The game's names on the bottom screen: `touch_menu.c decode_msg_as` turns
    the 2-byte characters into code points (`ique_charmap.inc`, from
    `CHARSET_IQUE`), and `bottom_screen.c` draws them with the ROM's
    `charset/standard.dat`, decoding each character into a 256-slot cache when
    first seen. Lines break between Chinese characters (`wrap_text`).
  - Name buffers (`PM3DS_UI_NAME`, `PM3DS_TM_NAME`) are larger on iQue: a
    character takes 4 bytes in the UI encoding.
  - `gfx_translate.c`: palette loads may start on a halfword or in the cart.
  - `obfuscation_shims.c` is left out (iQue's splat has no obfuscated segments).
  - `gen_version_headers.sh` copies the version's `icon_offsets.h` when it
    isn't PAL's.
- Not needed here: their icon-address and `load_font_data` changes (this
  tree's `ld_addrs.h` already defines `icon_ROM_START` as the cart address, and
  `dma_copy` copies host pointers and invalidates the texture cache).
- Left for when the ROM is here, in this order:
  1. `tools/extract_assets.py`, `tools/gen_version_headers.sh ique`,
     `PM3DS_VERSION=ique tools/gen_include_assets.py`, a first link and
     `tools/gen_link_syms.py` for `m11_syms_ique.ld` (their fork has one to
     compare with).
  2. `make PM3DS_VERSION=ique`, then boot: logos, title, file select (the
     file menu's Chinese glyphs), a new game, a battle.
  3. The bottom screen with the ROM's font: item and enemy names, a tattle
     (its stat line looks for "Max HP:", which the Chinese text may not have).
  4. Their GitHub #72 reports, against the port rather than their fork: the
     ground's texture, the enemy that stays in the tree after the wrong-tree
     fight, the heart block.
  5. Packaging: its own `StarHaven_iQue.cia`, or a third copy in the
     universal one if it fits.
