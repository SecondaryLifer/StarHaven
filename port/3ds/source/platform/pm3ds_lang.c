/*
 * pm3ds_lang.c — translations of the port's own text (pm3ds_lang.h).
 *
 * pm3ds_lang_table.inc lists every string as
 *   TR("English", "Deutsch", "Français", "Español", "Italiano", "日本語")
 * in UTF-8 (NULL: not translated, English is used). The chosen language's
 * column is converted to the UI encoding once, when the language is set.
 *
 * Simplified Chinese comes from its own file, pm3ds_lang_zh.inc (generated
 * from localization/ui_translations_zh_CN.csv by tools/gen_lang_zh.py), as
 * ZH("English", "中文"). It also covers English text the table doesn't list
 * (partner and area names, ...), so in Chinese T() looks there instead.
 */
#include <3ds.h>
#include <stdlib.h>
#include <string.h>

#include "pm3ds_lang.h"
#include "pm3ds_log.h"
#include "pm3ds_settings.h"

typedef struct {
    const char *text[PM3DS_LANG_COUNT];
} tr_entry_t;

#define TR(en, de, fr, es, it, ja) { { en, de, fr, es, it, ja } },
static const tr_entry_t s_table[] = {
#include "pm3ds_lang_table.inc"
};
#undef TR
#define NUM_TR (sizeof s_table / sizeof s_table[0])

typedef struct {
    const char *en, *zh;
} zh_entry_t;

#define ZH(en, zh) { en, zh },
static const zh_entry_t s_zh[] = {
#include "pm3ds_lang_zh.inc"
};
#undef ZH
#define NUM_ZH (sizeof s_zh / sizeof s_zh[0])

static const char *const s_names_utf8[PM3DS_LANG_COUNT] = {
    "English", "Deutsch", "Fran\u00e7ais", "Espa\u00f1ol", "Italiano", "\u65e5\u672c\u8a9e",
    "\u7b80\u4f53\u4e2d\u6587",
};

/* the game's font: Latin-1 letters to message codes 0x60-0x8F (+ 0x20 in UI text) */
static int latin_code(uint32_t cp) {
    static const uint16_t map[] = {
        0xC0, 0xC1, 0xC2, 0xC4, 0xC7, 0xC8, 0xC9, 0xCA, 0xCB, 0xCC, 0xCD, 0xCE, 0xCF, 0xD1, 0xD2, 0xD3,
        0xD4, 0xD6, 0xD9, 0xDA, 0xDB, 0xDC, 0xDF, 0xE0, 0xE1, 0xE2, 0xE4, 0xE7, 0xE8, 0xE9, 0xEA, 0xEB,
        0xEC, 0xED, 0xEE, 0xEF, 0xF1, 0xF2, 0xF3, 0xF4, 0xF6, 0xF9, 0xFA, 0xFB, 0xFC, 0xA1, 0xBF, 0xAA,
    };
    for (int i = 0; i < (int)(sizeof map / sizeof map[0]); i++)
        if (map[i] == cp) return 0x60 + i;
    return -1;
}

size_t pm3ds_lang_encode(const char *utf8, char *out, size_t size) {
    const unsigned char *p = (const unsigned char *)utf8;
    size_t n = 0;
    while (*p && n + 1 < size) {
        uint32_t cp;
        if (p[0] < 0x80) { cp = p[0]; p += 1; }
        else if ((p[0] & 0xE0) == 0xC0 && p[1]) { cp = (uint32_t)(p[0] & 0x1F) << 6 | (p[1] & 0x3F); p += 2; }
        else if ((p[0] & 0xF0) == 0xE0 && p[1] && p[2]) {
            cp = (uint32_t)(p[0] & 0x0F) << 12 | (uint32_t)(p[1] & 0x3F) << 6 | (p[2] & 0x3F); p += 3;
        } else { p++; continue; }
        if (cp >= 0x20 && cp < 0x7F) {
            out[n++] = (char)cp;
            continue;
        }
        int lc = latin_code(cp);
        if (lc >= 0) {
            out[n++] = (char)(lc + 0x20);
            continue;
        }
        if (cp == 0x3000) cp = ' ';   /* ideographic space */
        if (cp == ' ') { out[n++] = ' '; continue; }
        if (n + 5 > size) break;
        out[n++] = PM3DS_TXT_ESC;
        out[n++] = (char)(0x80 | (cp >> 14));
        out[n++] = (char)(0x80 | ((cp >> 7) & 0x7F));
        out[n++] = (char)(0x80 | (cp & 0x7F));
    }
    out[n] = 0;
    return n;
}

static int s_lang = -1;
static const char **s_conv;    /* NUM_TR (NUM_ZH in Chinese) strings in the UI encoding, for s_lang */
static int s_nconv;

/* English -> entry: an open-addressed hash of the English column */
#define HASH_SLOTS 1024
static int16_t s_hash[HASH_SLOTS], s_zh_hash[HASH_SLOTS];
static bool s_hashed;

static uint32_t hash_str(const char *s) {
    uint32_t h = 2166136261u;
    for (; *s; s++) h = (h ^ (unsigned char)*s) * 16777619u;
    return h;
}

static void hash_add(int16_t *hash, const char *en, int i) {
    uint32_t h = hash_str(en) & (HASH_SLOTS - 1);
    while (hash[h] >= 0) h = (h + 1) & (HASH_SLOTS - 1);
    hash[h] = (int16_t)i;
}

static void build_hash(void) {
    memset(s_hash, 0xFF, sizeof s_hash);
    memset(s_zh_hash, 0xFF, sizeof s_zh_hash);
    for (int i = 0; i < (int)NUM_TR; i++) hash_add(s_hash, s_table[i].text[0], i);
    for (int i = 0; i < (int)NUM_ZH; i++) hash_add(s_zh_hash, s_zh[i].en, i);
    s_hashed = true;
}

/* the entry whose English text is en, or -1 */
static int lookup(const int16_t *hash, const char *en, bool zh) {
    uint32_t h = hash_str(en) & (HASH_SLOTS - 1);
    for (int probe = 0; probe < HASH_SLOTS && hash[h] >= 0; probe++, h = (h + 1) & (HASH_SLOTS - 1)) {
        int i = hash[h];
        if (!strcmp(zh ? s_zh[i].en : s_table[i].text[0], en)) return i;
    }
    return -1;
}

const char *pm3ds_lang_name(int lang) {
    static char names[PM3DS_LANG_COUNT][32];
    if (lang < 0 || lang >= PM3DS_LANG_COUNT) lang = PM3DS_LANG_EN;
    if (!names[lang][0]) pm3ds_lang_encode(s_names_utf8[lang], names[lang], sizeof names[lang]);
    return names[lang];
}

int pm3ds_lang(void) { return s_lang < 0 ? PM3DS_LANG_EN : s_lang; }

void pm3ds_lang_set(int lang) {
    if (lang < 0 || lang >= PM3DS_LANG_COUNT) lang = PM3DS_LANG_EN;
    if (lang == s_lang && s_conv) return;
    if (!s_hashed) build_hash();
    if (s_conv) {
        for (int i = 0; i < s_nconv; i++) free((void *)s_conv[i]);
        free(s_conv);
    }
    s_lang = lang;
    s_nconv = lang == PM3DS_LANG_ZH ? (int)NUM_ZH : (int)NUM_TR;
    s_conv = (const char **)calloc((size_t)s_nconv, sizeof *s_conv);
    if (!s_conv) return;
    for (int i = 0; i < s_nconv; i++) {
        const char *src = lang == PM3DS_LANG_ZH ? s_zh[i].zh
                        : s_table[i].text[lang] ? s_table[i].text[lang] : s_table[i].text[0];
        size_t cap = strlen(src) * 2 + 1;   /* a 3-byte UTF-8 character takes 4 bytes */
        char *dst = (char *)malloc(cap);
        if (!dst) continue;
        pm3ds_lang_encode(src, dst, cap);
        s_conv[i] = dst;
    }
    pm3ds_log(PM3DS_LOG_INFO, "lang: %d (%d strings)", lang, s_nconv);
}

int pm3ds_lang_from_system(void) {
#if VERSION_IQUE
    return PM3DS_LANG_ZH;   /* the game itself is in Chinese */
#endif
    u8 l = CFG_LANGUAGE_EN;
    if (R_SUCCEEDED(cfguInit())) {
        CFGU_GetSystemLanguage(&l);
        cfguExit();
    }
    switch (l) {
        case CFG_LANGUAGE_DE: return PM3DS_LANG_DE;
        case CFG_LANGUAGE_FR: return PM3DS_LANG_FR;
        case CFG_LANGUAGE_ES: return PM3DS_LANG_ES;
        case CFG_LANGUAGE_IT: return PM3DS_LANG_IT;
        case CFG_LANGUAGE_JP: return PM3DS_LANG_JA;
        case CFG_LANGUAGE_ZH: return PM3DS_LANG_ZH;
        default:              return PM3DS_LANG_EN;
    }
}

/* en's text in lang, UTF-8 (en itself if there is none), for text in the
 * game's language rather than the chosen one (a tattle's stat line) */
const char *pm3ds_lang_utf8(int lang, const char *en) {
    if (!en) return "";
    if (!s_hashed) build_hash();
    if (lang == PM3DS_LANG_ZH) {
        int i = lookup(s_zh_hash, en, true);
        return i >= 0 ? s_zh[i].zh : en;
    }
    if (lang <= PM3DS_LANG_EN || lang >= PM3DS_LANG_COUNT) return en;
    int i = lookup(s_hash, en, false);
    return i >= 0 && s_table[i].text[lang] ? s_table[i].text[lang] : en;
}

const char *T(const char *en) {
    if (!en) return "";
    if (!s_conv) pm3ds_lang_set(pm3ds_cfg.language);
    if (!s_conv || !s_hashed) return en;
    bool zh = s_lang == PM3DS_LANG_ZH;
    int i = lookup(zh ? s_zh_hash : s_hash, en, zh);
    return i >= 0 && s_conv[i] ? s_conv[i] : en;
}
