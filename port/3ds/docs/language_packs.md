# Language packs

A language pack puts a translation of the game's messages on the SD card. Star Haven reads the
messages from the pack in place of the ROM's (GitHub #84). Players copy the pack's folder to
`/3ds/StarHaven/lang/` and pick it in Settings > **Game text**. A pack holds only the translated
text, never anything from the ROM.

## Making one

You need the decompilation's message files, the `.msg` sources of `assets/us/msg` (`00_Intro.msg`
to `2E_Credits.msg`), translated. Each file is one section. The messages keep the order, ids and
control codes of the originals, as in the decompilation, where `tools/build/msg/combine.py`
numbers them in file order.

1. Make a folder with the translated `.msg` files and a `pack.ini`:

   ```ini
   name = Català
   plural = es
   singular = a
   ```

   - **`name`** appears in Settings, at most 31 bytes of UTF-8.
   - **`plural`** (optional) is what a few messages add after a number greater than one,
     where English adds "s" (the shops' "coin[Var 2]", Chuck Quizmo, the Jump Attack minigame).
   - **`singular`** (optional) is added after one; English adds nothing.

   You can leave files out: any message the pack lacks shows the ROM's text.

2. Build it, from the repository root (Python 3 with `msgpack`, as for the decompilation):

   ```
   python3 port/3ds/tools/make_langpack.py my_translation/ out/ca
   ```

   This compiles every file with the decompilation's own message compiler and writes
   `out/ca/messages.bin`. It refuses a message longer than the game's 1024-byte buffers (256 for the
   credits). In place of `.msg` files the folder may hold the `.bin` that `combine.py` writes.

3. Copy `out/ca` to the SD card as `/3ds/StarHaven/lang/ca/`. The folder's name is the pack's id
   in `port.cfg` (`lang_pack=ca`) and may not contain spaces. If the pack doesn't load,
   `/3ds/StarHaven/log.txt` says why.

## What a pack changes

| | |
|---|---|
| Messages: dialogue, signs, tattles, item and badge names and descriptions, the credits | yes |
| The plural ending | yes (`plural`, `singular`) |
| The bottom screen's names (items, enemies, tattles) | yes: they are read from the messages |
| The file select's and pause menu's own words (`src/filemenu/filemenu_msg.c`, `src/pause/pause_gfx.c`) | not yet |
| Words drawn into pictures (title, chapter cards, battle messages) | not yet |
| The bottom screen's own text (Settings, tabs) | no: Settings > Language has its own languages |

## Versions

Packs use the **US** version's message ids, those of `assets/us/msg`. On the EU ROM, two sections
have messages the US one doesn't: the minigames (section 08) and the credits (2E). Star
Haven maps those back to the US numbering, so the same pack works on both ROMs. The EU ROM's
extra messages show its English text. Players with the EU ROM should set the game language to
English, so the file select and pause menu match. The Japanese and iQue versions have their own
messages and take no packs.

## The font

The game's font has the accented letters of the EU languages (à è é í ï ò ó ú ü ç ...). It has
no middle dot (·), so Catalan's l·l needs another spelling (l.l, for example).

## The file

`messages.bin`, all numbers little-endian:

| Offset | |
|---|---|
| 0 | `"SHLP"` |
| 4 | u16 version (1), u16 base (0: US ids) |
| 8 | u32 message count, u32 text size |
| 16 | the name, 32 bytes of UTF-8 ending in 0 |
| 48 | plural, then singular: 8 bytes each of game font codes ending in `[End]` (0xFD); a first byte of 0 means not set |
| 64 | per message, ids ascending: u32 id (section << 16 \| index), u32 offset into the text, u32 length |
| 64 + 12 × count | the text, each message ending in `[End]` |

The loader is `source/n64_shim/langpack.c`. `src/msg.c`'s `dma_load_msg` loads every message and
asks it first.
