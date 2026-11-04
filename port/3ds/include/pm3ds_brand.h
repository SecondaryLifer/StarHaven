/*
 * pm3ds_brand.h — the port's name and its folder on the SD card.
 *
 * Kept free of Nintendo's names and trademarks; the game itself is only named
 * where the player is told which ROM to supply. Change the name here, in
 * cia/app.rsf, in the Makefile's APP_* lines and in tools/make_branding.py.
 */
#ifndef PM3DS_BRAND_H
#define PM3DS_BRAND_H

#define PM3DS_APP_NAME       "Star Haven"
#define PM3DS_APP_NAME_CAPS  "STAR HAVEN"

/* release version: shown on the bottom screen and in the log; the CIA's
 * title version comes from these numbers too (tools/package.sh) */
#define PM3DS_VERSION_MAJOR  1
#define PM3DS_VERSION_MINOR  3
#define PM3DS_VERSION_MICRO 12
#define PM3DS_VSTR_(x)       #x
#define PM3DS_VSTR(x)        PM3DS_VSTR_(x)
#define PM3DS_VERSION_STR    PM3DS_VSTR(PM3DS_VERSION_MAJOR) "." PM3DS_VSTR(PM3DS_VERSION_MINOR) \
                             "." PM3DS_VSTR(PM3DS_VERSION_MICRO)

/* where releases are published (platform/pm3ds_update.c checks it) */
#define PM3DS_GITHUB_REPO    "SecondaryLifer/StarHaven"

/* save, settings, log and the player's ROM */
#define PM3DS_SD_DIR         "sdmc:/3ds/StarHaven"
#define PM3DS_SD_DIR_SHOWN   "/3ds/StarHaven"
/* where the player's ROM is looked for, in this order (platform/pm3ds_rom.c,
 * universal/starter.c): any Paper Mario ROM there, whatever its file name */
#define PM3DS_ROM_DIRS       PM3DS_SD_DIR, "sdmc:/roms/n64", "sdmc:/roms", "sdmc:/n64", "sdmc:/3ds", "sdmc:/"
/* builds before the rename used this folder; it is moved on first boot */
#define PM3DS_SD_DIR_OLD     "sdmc:/3ds/PaperMario3DS"

#endif
