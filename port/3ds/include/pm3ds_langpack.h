/*
 * pm3ds_langpack.h — language packs: a fan translation's message text from the
 * SD card (GitHub #84), /3ds/StarHaven/lang/<id>/messages.bin, made with
 * tools/make_langpack.py. Settings > Game text picks one (port.cfg lang_pack=<id>).
 */
#ifndef PM3DS_LANGPACK_H
#define PM3DS_LANGPACK_H

#include <stdbool.h>
#include <stdint.h>

#define PM3DS_LANGPACK_ID 32

/* the packs on the SD card (read again by each call) */
int pm3ds_langpack_scan(void);
const char *pm3ds_langpack_id(int i);
const char *pm3ds_langpack_name(int i);   /* UI-encoded (pm3ds_lang.h) */

/* the pack to use ("": the ROM's text); loaded before the next message */
void pm3ds_langpack_request(const char *id);
/* the requested pack's name, UI-encoded, or NULL for none or one not found */
const char *pm3ds_langpack_current_name(void);

/* src/msg.c dma_load_msg: the pack's text of a message into dest (at most max
 * bytes, ending in [End]); false: the pack has none, the ROM's is used */
bool pm3ds_langpack_msg(uint32_t msgID, void *dest, uint32_t max);

#endif
