# Star Haven

A native Nintendo 3DS port of **Paper Mario** (Nintendo 64), built from the community
decompilation. The game's own code runs on the 3DS; nothing is emulated.

**Star Haven contains no game data.** You need your own copy of the game: Star Haven reads
your ROM from the SD card when it starts.

## What you need

- A **New Nintendo 3DS, New 3DS XL, New 2DS XL** (or New 3DS LL) for 60 fps. The original
  3DS and 2DS run it at about 30 fps, with a few extras off to get there (no reverb, no 3D,
  shadows only under Mario and your partner), and only the CIA: the `.3dsx` doesn't get
  enough memory there.
- Custom firmware (Luma3DS) and a CIA installer such as FBI, or the Homebrew Launcher for
  the `.3dsx`.
- A dump of **Paper Mario** as `.z64`, `.n64` or `.v64`:
  - Europe (En, Fr, De, Es), the 48 MB or the 64 MB dump, or
  - USA, or
  - the iQue Player's Chinese release (纸片马力欧), 40 MB (see below).

  The Japanese version is not supported.
- For sound: the DSP firmware dump at `/3ds/dspfirm.cdc`. Most custom-firmware setups have
  it; if yours doesn't, run DSP1 once.

## Installing

1. Install `StarHaven.cia` with FBI (or put `StarHaven.3dsx` in `/3ds/` for the Homebrew
   Launcher).
2. Copy your ROM into `/3ds/StarHaven/` on the SD card. The file name doesn't matter, and
   `.z64`, `.n64` and `.v64` dumps all work. Star Haven also finds it in `/roms/n64/`,
   `/roms/`, `/n64/`, `/3ds/` or the top of the SD card.
3. Start Star Haven. Loading the ROM takes a few seconds.

If the ROM is missing, unreadable or a version Star Haven doesn't know, both screens say
what's wrong and how to fix it.

**The iQue (Chinese) version** runs in the same app: put the iQue ROM in `/3ds/StarHaven/` as
above. The game, the file menu and the bottom screen are in Chinese (the bottom screen draws the
game's own Chinese font); Settings > Language switches the bottom screen to another language.
Its saves are `save_ique.fla`. Language packs and N64 save import are for the EU and US versions
only. With more than one ROM on the SD card, Star Haven asks which one to play.
1.3.12 had a separate `StarHaven_iQue` app: you can delete it (FBI → Titles), your saves stay.

**Updating from 1.0.0 or 1.0.1:** 1.0.2 has a new title ID (the old one was also used by
other homebrew, and installing either one removed the other), so it installs next to the old
version. Delete the old Star Haven in FBI (Titles) or System Settings → Data Management.
Your saves are on the SD card and are kept.

## Controls

| 3DS | Game |
|---|---|
| Circle Pad | Move |
| A / B | A / B |
| L | Z |
| R | R |
| Select | L |
| Start | Start |
| X | C-Down (partner ability) |
| Y | C-Left |
| ZL (hold) | Turbo A: presses A over and over, for mashing |
| D-Pad, C-Stick | C buttons |
| ZR (hold) | Fast-forward |

Every button can be changed under **Options → Controls** (Turbo A can go on any button; earlier
versions' saved controls keep ZL as C-Left until you change it there). The 3D slider works (not on the
original 3DS and 2DS).

## Bottom screen

The tabs run down the left side:

- **Stats**: HP, FP, level, Star Points, coins, star pieces, Star Power and play time, and what
  the area you are in still hides: the star pieces, badges and hidden blocks and panels found
  there, and how many item spots the room you are standing in still holds.
- **Map**: the world map, where you are and the area's counts. Drag it to look around.
- **Party**: tap a partner to switch to them.
- **Tattle**: every enemy Goombario has tattled so far. Tap one to read the whole tattle, with
  its HP, Attack and Defense on top.
- **Found**: for each area, how many star pieces, badges, hidden blocks and panels, and item
  spots you have collected; the area you are in is highlighted (gifts from characters are not
  counted; a randomizer file shows only the totals).
- **Recipes**: every dish Tayce T. can cook, and how many you have had her cook. Dishes you
  haven't made yet show as *???* until you tap *Show all*. Tap a dish to see what it restores,
  its description and every way to cook it (two ingredients need the Cookbook).
- **Save** on the Stats tab, tapped twice, saves the game wherever the pause menu could open.
  Loading puts you back on the spot on a map with a save block, and at the entrance you came
  in by anywhere else.
- **Items** opens the item menu.
- In battle the side shows your HP, FP, Star Power and coins, next to the battle menus to tap
  or an A/B pad for action commands. Mario's commands are big tiles with the game's icons: Jump and Hammer along the
  bottom, Strategies, Items and Star Spirits above them. Every list shows each entry's icon.
  To choose a target, tap the enemy in the picture of the stage (each one has a number) or its
  numbered tile, which shows its HP once Goombario has tattled it.
- **Title and file screens**: tap the card on the title to start, and on the EU ROM **Game language**
  for the game's own language. The bottom screen then shows your four files with Delete, Copy and
  Back, the Yes/No prompts and a keyboard for the file name. The buttons still work too.
- **Fast >>** turns fast-forward on and off.
- **Options** (with its own tabs: Settings, Controls, Cheats on a cheat file, Debug):
  - **Language**: the bottom screen's own text in English, Deutsch, Français, Español, Italiano,
    日本語 or 简体中文 (Simplified Chinese).
  - **Game text**: *Original* (your ROM's) or a language pack on the SD card, a fan translation
    of the game's text (see Language packs below).
  - **Screen**: *Original* (4:3), *Fill* or *Wide*.
  - **3D depth**: how deep the 3D slider goes, from *Low* to *Deepest*.
  - **Frame rate**: 60 fps (in-between frames are added) or the original 30 fps. With the 3D
    slider up, 60 fps holds where there is time to draw both eyes twice and drops to 30
    elsewhere; battles stay at 30.
  - **Text speed**: Normal, Fast or Instant.
  - **Fast-forward**: speed (2x to 4x), and whether the button is held or toggles.
  - **Steerable spin** and **Steerable jump**: the Circle Pad turns a spin dash or a jump while it
    happens. Off by default, so Mario moves as in the original.
  - **Music** and **Sound effects**: switch either off (sound effects includes the areas'
    background sounds).
  - **Jukebox**: pick a song that plays instead of the area's music. Battles keep their own.
  - **Check for updates**: asks GitHub now whether a newer version is out, and shows the answer
    on the row.
  - **Check at launch**: the same question when Star Haven starts. A newer version shows a
    notice on the bottom screen (*Skip* stops the launch check mentioning that version). It
    downloads and installs nothing. Off here, from the next start.
  - **Controls** tab: remap any button.

Settings are remembered.

## Randomizer

When you pick an empty file, Star Haven asks how you want to play:

- **Classic**: the original game.
- **Randomizer**: items and badges are found in new places. The story keeps its normal order.
  You can also shuffle:
  - coins and star pieces,
  - key items, which always land somewhere you can reach before you need them,
  - shop stock,
  - gifts from characters,
  - key items into hidden panels and hidden blocks (hard to find without the spoiler log).

  Leave the **Seed** on *Random*, or type one in to replay or share a game: the same seed
  with the same settings gives the same game.

Each randomized game writes a spoiler log to `/3ds/StarHaven/spoiler_<seed>.txt` listing
where everything went. The seed is shown on the Stats tab.

## Cheats

The new-file screen also has a **Cheats** switch. Turn it on and that file gets a **Cheats** tab
under Options, where you switch each one on or off: infinite HP (Mario can't be knocked out),
infinite FP, infinite Star Power, 999 coins, unlimited BP (equip any badges), no level-up limits,
free quality-of-life badges, levels past 27 (up to 99, and a won fight gives at least 1 star point)
and action commands from the moment you switch it on. The switches
are saved with the file. This is only chosen when the file is made: a file started with the
switch off never shows cheats and can't turn them on later.

## Level 99

The new-file screen also has a **Level 99** switch (L, or tap it): that file levels up as far as
99 like in The Thousand-Year Door, with up to 95 HP, 95 FP and 99 BP, and a won fight is always
worth at least 1 star point. No cheats involved, and the rest of the game is as usual. It works
with Classic, Randomizer and Hard mode, and is only chosen when the file is made.

## Bringing an N64 save

Saves from an N64 emulator (Project64, RetroArch/mupen64plus, ares), a flashcart or a
Wii U Virtual Console extraction carry over:

1. Put the save in `/3ds/StarHaven/` as `import.fla` (a RetroArch `.srm` as `import.srm`).
2. Start Star Haven. It converts the save and says on the bottom screen how many save files
   it brought in. It goes into the version you play (`save.fla` for the EU ROM,
   `save_us.fla` for the US ROM), and replaces every save file there; the old save is kept
   as `save.fla.bak` (or `save_us.fla.bak`).
3. The import file is renamed `import.fla.done`, so it is only imported once. If it holds
   no Paper Mario save, it is renamed `import.fla.bad` and your saves are not changed.

The [save converter](https://secondarylifer.github.io/StarHaven/converter/) does the same in
a browser, and also turns a Star Haven save back into an N64 one.

Saves from **Paperboat** (the PC port) carry over with the same page, both ways: drop its
`file0.json` to `file3.json` there to get a `save_us.fla`, or your `save_us.fla` to get them
back. Paperboat is the US version, so this goes with the US ROM.

## Language packs

A language pack is a fan translation of the game's text. Put its folder in `/3ds/StarHaven/lang/`
(e.g. `/3ds/StarHaven/lang/ca/messages.bin`) and pick it in Settings > **Game text**. It works with
the US and the EU ROM; on the EU ROM, set the game language to English. For now packs change the
messages (dialogue, items, tattles, menus written as messages); the file select's and pause menu's
own words, and words drawn into pictures (title, chapter cards), stay your ROM's. Messages a pack
lacks are your ROM's too.

Translators: [`docs/language_packs.md`](https://github.com/SecondaryLifer/StarHaven/blob/main/port/3ds/docs/language_packs.md)
explains how to make one from `.msg` files with `tools/make_langpack.py`.

## Files on the SD card (`/3ds/StarHaven/`)

| File | |
|---|---|
| your ROM | any name, e.g. `Paper Mario (USA).z64` |
| `save.fla` | your save files (`save_us.fla` with the US version, `save_ique.fla` with the iQue one) |
| `save.fla.bak` | your saves from before an N64 save import |
| `import.fla`, `import.srm` | an N64 save to import at the next start (see above) |
| `port.cfg` | settings |
| `spoiler_<seed>.txt` | randomizer spoiler logs |
| `log.txt` | warnings, useful in bug reports |
| `lang/<name>/messages.bin` | language packs (Settings > Game text) |

## Help and bug reports

Join the Star Haven 3DS Discord: https://discord.gg/zHq9tH49Z
Ask for help there, hear about new versions first, or report a bug. Bug reports on GitHub
(https://github.com/SecondaryLifer/StarHaven/issues) are welcome too. A photo of the crash
screen and your `log.txt` help a lot.

## Known issues

- A few N64 graphics effects are approximations.

## Credits

- The 3D HOME Menu banner is by [Velvii](https://github.com/Velvii). Its title uses the Hey
  Gorgeous font, free for personal use.
- The Simplified Chinese bottom screen is translated by [Darth-Koopa](https://github.com/Darth-Koopa),
  who also worked out the iQue version's support. Its characters are from
  [Fusion Pixel](https://github.com/TakWolf/fusion-pixel-font) by TakWolf (SIL Open Font License 1.1).
- Star Haven itself is by SecondaryLifer.

## Legal

Star Haven is a fan project. It is not affiliated with or endorsed by Nintendo. *Paper Mario*
is a trademark of Nintendo. Star Haven ships no Nintendo game data: images, models, sounds and
text are read from your own ROM at startup. Don't ask for ROMs.

Built on the work of the [Paper Mario decompilation project](https://github.com/pmret/papermario),
[devkitPro](https://devkitpro.org) (libctru, citro3d) and, for the randomizer's world logic, the
[Paper Mario Randomizer](https://github.com/icebound777/PMR-SeedGenerator) (MIT).
