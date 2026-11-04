/*
 * pm3ds_lang.h — the languages of the port's own text (bottom screen, settings).
 *
 * The game's text stays the ROM's. T("English text") gives the text in the
 * chosen language, in the bottom screen's encoding: one byte per glyph of the
 * game's font (0x21-0x7E ASCII, 0x80-0xAF its accented letters, as message
 * code + 0x20), and PM3DS_TXT_ESC + 3 bytes for any other character (Chinese:
 * the iQue ROM's font or the one built in, pm3ds_cjk_font.inc; Japanese: the 3DS
 * system font). Missing translations fall back to English.
 */
#ifndef PM3DS_LANG_H
#define PM3DS_LANG_H

#include <stddef.h>
#include <stdint.h>

enum {
    PM3DS_LANG_EN,
    PM3DS_LANG_DE,
    PM3DS_LANG_FR,
    PM3DS_LANG_ES,
    PM3DS_LANG_IT,
    PM3DS_LANG_JA,
    PM3DS_LANG_ZH,      /* Simplified Chinese (localization/ui_translations_zh_CN.csv) */
    PM3DS_LANG_COUNT
};

/* a character the game's font lacks: 0x01, then its code point in three
 * 7-bit groups, each | 0x80 (never a 0 byte inside a C string) */
#define PM3DS_TXT_ESC 0x01

const char *pm3ds_lang_name(int lang);   /* in its own language, UI-encoded */

int pm3ds_lang(void);                     /* the current language */
void pm3ds_lang_set(int lang);
int pm3ds_lang_from_system(void);         /* the console's language, if one of ours (else English) */
const char *T(const char *en);

/* UTF-8 -> the UI encoding; returns the length written */
size_t pm3ds_lang_encode(const char *utf8, char *out, size_t size);
/* one character of a UI string: its code point (game glyph code + 0x20 for the
 * game font, or the escaped code point), and the bytes it took */
static inline uint32_t pm3ds_txt_next(const char *s, int *len) {
    const unsigned char *p = (const unsigned char *)s;
    if (p[0] == PM3DS_TXT_ESC && p[1] && p[2] && p[3]) {
        *len = 4;
        return 0x80000000u | (uint32_t)(p[1] & 0x7F) << 14 | (uint32_t)(p[2] & 0x7F) << 7 | (p[3] & 0x7F);
    }
    *len = 1;
    return p[0];
}

#endif
