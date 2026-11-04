# Bottom-screen translations

The port's own text (the bottom screen, Settings, the new-file screen) is
translated in two places:

- `source/platform/pm3ds_lang_table.inc`: English, Deutsch, Français, Español,
  Italiano and 日本語, one `TR(...)` line per string.
- `ui_translations_zh_CN.csv` here: Simplified Chinese (简体中文), by
  Darth-Koopa (GitHub PR #62, their StarHaven_CN fork). Columns:
  `category, source_file, english_source, proposed_simplified_chinese, review_notes`.
  A row matches the English text exactly, `%d`/`%s` codes included. It may
  also translate English the table doesn't list (partner and area names,
  ...). Rows noted "drafted by the maintainer" still want a native speaker's look.

After editing the CSV, run

    python3 port/3ds/tools/gen_lang_zh.py

It writes `source/platform/pm3ds_lang_zh.inc` and stops if a string uses a
character the built-in font lacks. Then run it with the fonts, which writes
`source/platform/pm3ds_cjk_font.inc` too:

    python3 port/3ds/tools/gen_lang_zh.py \
        --font10 fusion-pixel-10px-proportional-zh_hans.ttf \
        --font12 fusion-pixel-12px-proportional-zh_hans.ttf

The fonts are in the `-proportional-ttf` zips of
[Fusion Pixel](https://github.com/TakWolf/fusion-pixel-font/releases) (2026.10.25
used; SIL OFL 1.1, `port/3ds/fonts/`). That needs Pillow
(`pip install Pillow`). Both generated files are committed, so a build needs
neither.

The characters are built in because the game's font has no Chinese and only
Chinese 3DS consoles have a Simplified Chinese system font. On the iQue
version, text of normal size uses the ROM's own Chinese font instead
(`tools/gen_ique_charmap.py`), and so do the game's names (items, enemies,
tattles) that the bottom screen shows.
