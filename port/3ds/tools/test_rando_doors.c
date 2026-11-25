/*
 * Host test of the door shuffle (n64_shim/rando_doors.c), over many seeds:
 *   cc -O2 -o /tmp/test_rando_doors port/3ds/tools/test_rando_doors.c && /tmp/test_rando_doors [seeds]
 * For every seed: each door end leads to another end of its own group and that
 * end leads back (coupled), every group stays connected (its own BFS, not the
 * shuffle's check), going through a door with the game's GotoMap and back
 * through the one you came out of returns you to the first door, and the same
 * seed deals the same doors. Prints failures and a few totals; exit 1 on any.
 */
#define PM3DS_DOORS_HOST_TEST
#include "../source/n64_shim/rando_doors.c"

#include <stdlib.h>

static int zone_of(int e) { return sDoors[e >> 1].zone; }

/* flood from the group's first room: a room reaches the room of every end paired with one of its ends */
static int zone_connected(int z) {
    static int seen[MAX_NODES], counted[MAX_NODES];
    int e, changed, started = 0, nodes = 0, reached = 0;
    memset(seen, 0, sizeof(seen));
    memset(counted, 0, sizeof(counted));
    for (e = 0; e < NUM_ENDS && !started; e++) {
        if (zone_of(e) == z) {
            seen[end_node(e)] = started = 1;
        }
    }
    do {
        changed = 0;
        for (e = 0; e < NUM_ENDS; e++) {
            if (zone_of(e) == z && seen[end_node(e)] && !seen[end_node(sDoorTo[e])]) {
                seen[end_node(sDoorTo[e])] = changed = 1;
            }
        }
    } while (changed);
    for (e = 0; e < NUM_ENDS; e++) {
        if (zone_of(e) == z && !counted[end_node(e)]) {
            counted[end_node(e)] = 1;
            nodes++;
            reached += seen[end_node(e)];
        }
    }
    return reached == nodes;
}

static const char *bare_place(const char *map, char *buf, int size) {
    snprintf(buf, size, "%s", map);
    return buf;
}

int main(int argc, char **argv) {
    long seeds = argc > 1 ? atol(argv[1]) : 100000;

    if (argc > 2 && strcmp(argv[1], "--seed") == 0) {   /* the spoiler's door list for one seed */
        pm3ds_doors_build((unsigned)strtoul(argv[2], NULL, 16), 1);
        pm3ds_doors_spoiler(stdout, bare_place);
        return 0;
    }
    long fails = 0, keptGroups = 0, selfRoom = 0, moved = 0, total = 0;
    int zones = 0, e;
    static uint16_t first[NUM_ENDS];

    for (e = 0; e < NUM_ENDS; e++) {
        if (zone_of(e) >= zones) zones = zone_of(e) + 1;
    }
    for (long s = 1; s <= seeds; s++) {
        uint32_t seed = (uint32_t)(s * 2654435761u) ^ 0x5EED5EEDu;
        int z;
        keptGroups += pm3ds_doors_build(seed, 1);
        memcpy(first, sDoorTo, sizeof(first));
        for (e = 0; e < NUM_ENDS; e++) {
            int t = sDoorTo[e];
            const char *m = end_map(e ^ 1);
            int en = end_entry(e ^ 1);
            if (t == e || sDoorTo[t] != e || zone_of(t) != zone_of(e)) {
                printf("seed %08X: end %d (%s %d) -> %d not coupled\n", seed, e, end_map(e), end_entry(e), t);
                fails++;
            }
            if (strcmp(end_map(e), end_map(t)) == 0) selfRoom++;
            moved += t != (e ^ 1);
            total++;
            /* through door e by its own GotoMap, then back through the door we came out of */
            pm3ds_doors_redirect(end_map(e), &m, &en);
            if (strcmp(m, end_map(t)) != 0 || en != end_entry(t)) {
                printf("seed %08X: %s door %d went to %s %d, not %s %d\n", seed, end_map(e), end_entry(e), m, en,
                       end_map(t), end_entry(t));
                fails++;
            } else {
                const char *bm = end_map(t ^ 1);
                int be = end_entry(t ^ 1);
                pm3ds_doors_redirect(m, &bm, &be);
                if (strcmp(bm, end_map(e)) != 0 || be != end_entry(e)) {
                    printf("seed %08X: back from %s %d went to %s %d, not %s %d\n", seed, m, en, bm, be, end_map(e),
                           end_entry(e));
                    fails++;
                }
            }
        }
        for (z = 0; z < zones; z++) {
            if (!zone_connected(z)) {
                printf("seed %08X: group %d is split\n", seed, z);
                fails++;
            }
        }
        pm3ds_doors_build(seed, 1);
        if (memcmp(first, sDoorTo, sizeof(first)) != 0) {
            printf("seed %08X: dealt differently the second time\n", seed);
            fails++;
        }
        if (fails > 20) break;
    }
    /* off: everything as it is, and no redirects */
    pm3ds_doors_build(1, 0);
    for (e = 0; e < NUM_ENDS; e++) {
        const char *m = end_map(e ^ 1);
        int en = end_entry(e ^ 1);
        if (sDoorTo[e] != (e ^ 1) || pm3ds_doors_redirect(end_map(e), &m, &en)) {
            printf("off: end %d moved\n", e);
            fails++;
        }
    }
    printf("%ld seeds, %d doors in %d groups: %ld failures, %ld groups kept as they were, "
           "%.1f%% of door ends lead elsewhere, %ld ends back into their own room\n",
           seeds, NUM_DOORS, zones, fails, keptGroups, 100.0 * moved / (total ? total : 1), selfRoom);
    return fails != 0;
}
