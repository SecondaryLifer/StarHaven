/*
 * rando_doors.c — the randomizer's door shuffle (PM3DS_RANDO_DOORS).
 *
 * The doors are two-way loading zones (tools/rando_doors.py -> rando_doors.inc),
 * each in a group (zone) of rooms the story opens all at once. A seed deals
 * each group's doors out again among the group: door ends are shuffled and
 * paired up, so going back through the door you came out of takes you back.
 * A dealing is kept only if every room of the group can still reach every
 * other (and, while there are tries left, if no door leads back into its own
 * room); a group that finds none keeps its doors. So each room opens at the
 * same point of the story as before, which is what the item randomizer's
 * key-item logic counts on.
 *
 * The game asks on every GotoMap (src/evt/map_api.c, goto_map): a door taken
 * is known by the map it is taken in and its original destination, which is
 * the entrance it pairs with; the destination becomes that door's new
 * partner. The map change itself stays the game's own, so "Stuck? Back to the
 * door" (src/state_world.c) reloads the room at the entrance really used.
 *
 * No decomp headers: tools/test_rando_doors.c builds this file on the host and
 * checks many seeds.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef PM3DS_DOORS_HOST_TEST
#include "pm3ds_log.h"
#define DOOR_LOG(...) pm3ds_log(PM3DS_LOG_INFO, __VA_ARGS__)
#else
#define DOOR_LOG(...) ((void)0)
#endif

typedef struct {
    uint8_t zone;
    uint8_t entryA, entryB;
    uint8_t nodeA, nodeB;   /* the room (or side of one) each end is in */
    const char *mapA, *mapB;
} DoorPair;

#define DOOR(zone, mapA, entryA, nodeA, mapB, entryB, nodeB) { zone, entryA, entryB, nodeA, nodeB, mapA, mapB },
static const DoorPair sDoors[] = {
#include "rando_doors.inc"
};
#undef DOOR
#define NUM_DOORS ((int)(sizeof(sDoors) / sizeof(sDoors[0])))
#define NUM_ENDS (2 * NUM_DOORS)   /* end 2i: pair i's A side, 2i + 1: its B side */
#define MAX_NODES 256
#define DOOR_TRIES 400

static uint16_t sDoorTo[NUM_ENDS];   /* going through end e comes out at end sDoorTo[e] (vanilla: e ^ 1) */
static int sDoorsOn;

static const char *end_map(int e) { return (e & 1) ? sDoors[e >> 1].mapB : sDoors[e >> 1].mapA; }
static int end_entry(int e) { return (e & 1) ? sDoors[e >> 1].entryB : sDoors[e >> 1].entryA; }
static int end_node(int e) { return (e & 1) ? sDoors[e >> 1].nodeB : sDoors[e >> 1].nodeA; }

static uint32_t door_rand(uint32_t *state) {   /* splitmix32, as rando.c */
    uint32_t z = (*state += 0x9E3779B9u);
    z = (z ^ (z >> 16)) * 0x85EBCA6Bu;
    z = (z ^ (z >> 13)) * 0xC2B2AE35u;
    return z ^ (z >> 16);
}

static uint8_t sRoot[MAX_NODES];

static int root(int n) {
    while (sRoot[n] != n) {
        sRoot[n] = sRoot[sRoot[n]];
        n = sRoot[n];
    }
    return n;
}

/* the doors ends[0..n) paired up two by two: does every room of the group reach every other? */
static int dealing_ok(const uint16_t *ends, int n, int strict) {
    int k, r = -1;
    for (k = 0; k < n; k++) {
        sRoot[end_node(ends[k])] = (uint8_t)end_node(ends[k]);
    }
    for (k = 0; k < n; k += 2) {
        int a = end_node(ends[k]), b = end_node(ends[k + 1]);
        if (strict && strcmp(end_map(ends[k]), end_map(ends[k + 1])) == 0) {
            return 0;   // back into its own room
        }
        sRoot[root(a)] = (uint8_t)root(b);
    }
    for (k = 0; k < n; k++) {
        int x = root(end_node(ends[k]));
        if (r < 0) {
            r = x;
        } else if (x != r) {
            return 0;
        }
    }
    return 1;
}

/* rando.c: deal the doors for this seed (on 0: as they are); returns the groups left as they were */
int pm3ds_doors_build(unsigned seed, int on) {
    static uint16_t ends[NUM_ENDS];
    uint32_t st = seed ^ 0x444F4F52u;   /* "DOOR": its own stream, the items don't change */
    int e, z, kept = 0, zones = 0;

    for (e = 0; e < NUM_ENDS; e++) {
        sDoorTo[e] = (uint16_t)(e ^ 1);
    }
    sDoorsOn = on;
    if (!on) {
        return 0;
    }
    for (e = 0; e < NUM_ENDS; e++) {
        if (sDoors[e >> 1].zone >= zones) {
            zones = sDoors[e >> 1].zone + 1;
        }
    }
    for (z = 0; z < zones; z++) {
        int n = 0, t, k;
        for (e = 0; e < NUM_ENDS; e++) {
            if (sDoors[e >> 1].zone == z) {
                ends[n++] = (uint16_t)e;
            }
        }
        for (t = 0; t < DOOR_TRIES; t++) {
            for (k = n - 1; k > 0; k--) {   // Fisher-Yates: then ends 2k and 2k + 1 are a door
                int j = (int)(door_rand(&st) % (uint32_t)(k + 1));
                uint16_t x = ends[k];
                ends[k] = ends[j];
                ends[j] = x;
            }
            if (dealing_ok(ends, n, t < DOOR_TRIES / 2)) {
                break;
            }
        }
        if (t == DOOR_TRIES) {
            kept++;
            DOOR_LOG("rando: door group %d kept as it is", z);
            continue;
        }
        for (k = 0; k < n; k += 2) {
            sDoorTo[ends[k]] = ends[k + 1];
            sDoorTo[ends[k + 1]] = ends[k];
        }
    }
    DOOR_LOG("rando: doors shuffled, %d groups, %d doors (%d groups kept)", zones, NUM_DOORS, kept);
    return kept;
}

/* src/evt/map_api.c, goto_map: GotoMap(*toMap, *toEntry) run in fromMap; returns 1 when it goes elsewhere now */
int pm3ds_doors_redirect(const char *fromMap, const char **toMap, int *toEntry) {
    int e;

    if (!sDoorsOn || fromMap == NULL || *toMap == NULL) {
        return 0;
    }
    for (e = 0; e < NUM_ENDS; e++) {
        int v = e ^ 1;   // the entrance door e led to
        if (end_entry(v) == *toEntry && strcmp(end_map(v), *toMap) == 0 && strcmp(end_map(e), fromMap) == 0) {
            int t = sDoorTo[e];
            if (t == v) {
                return 0;
            }
            DOOR_LOG("rando: door %s %d -> %s %d (was %s %d)", fromMap, end_entry(e), end_map(t), end_entry(t),
                     *toMap, *toEntry);
            *toMap = end_map(t);
            *toEntry = end_entry(t);
            return 1;
        }
    }
    return 0;
}

/* rando.c, the spoiler log: every door that leads elsewhere, by the room it is in */
void pm3ds_doors_spoiler(FILE *f, const char *(*place)(const char *map, char *buf, int size)) {
    char a[48], b[48];
    int e, n = 0;

    if (!sDoorsOn) {
        return;
    }
    fprintf(f, "\nDoors (door N is the room's entrance N; going back takes you back):\n");
    for (e = 0; e < NUM_ENDS; e++) {
        if (sDoorTo[e] != (e ^ 1)) {
            n++;
        }
    }
    if (n == 0) {
        fprintf(f, "  (all as they are)\n");
        return;
    }
    /* by room, then entrance */
    {
        static uint16_t order[NUM_ENDS];
        int i, j, m = 0;
        for (e = 0; e < NUM_ENDS; e++) {
            if (sDoorTo[e] != (e ^ 1)) {
                order[m++] = (uint16_t)e;
            }
        }
        for (i = 1; i < m; i++) {
            uint16_t x = order[i];
            for (j = i; j > 0; j--) {
                int c = strcmp(end_map(order[j - 1]), end_map(x));
                if (c < 0 || (c == 0 && end_entry(order[j - 1]) < end_entry(x))) {
                    break;
                }
                order[j] = order[j - 1];
            }
            order[j] = x;
        }
        for (i = 0; i < m; i++) {
            e = order[i];
            place(end_map(e), a, sizeof(a));
            place(end_map(sDoorTo[e]), b, sizeof(b));
            fprintf(f, "  %s door %d -> %s door %d   (was %s door %d)\n", a, end_entry(e), b, end_entry(sDoorTo[e]),
                    end_map(e ^ 1), end_entry(e ^ 1));
        }
    }
}
