#!/usr/bin/env python3
"""List the door shuffle's loading zones from the decomp's map scripts.

A door is a two-way loading zone: an exit script bound to a trigger
(EVT_EXIT_WALK / _NOK / _FIXED, EVT_EXIT_SINGLE_DOOR / SPLIT_SINGLE_DOOR /
DOUBLE_DOOR, or a hand-written one with UseExitHeading or Exec(ExitXxxDoor)),
leaving map M by its entrance e (exitIdx) for GotoMap("N", f), whose map N
leaves by f for GotoMap("M", e). The pair (M e, N f) is one door. One-way
exits, pipes, cutscene GotoMaps and exits whose source map also reaches the
same target from another script are never doors.

Only the areas in AREAS are shuffled (doors there lead to rooms of the same
area), and inside those only the doors the story never gates:
  KEEP     doors that stay as they are, with why (story-scripted rooms, boss
           rooms): they are not shuffled at all
  PIN      doors kept as they are inside a shuffled group: the rooms on both
           sides count as one room for the shuffle
  REGIONS  rooms split by something the story or an ability opens (a bridge,
           a gate, a scene that must play first): each side is its own room
A group (zone) is a set of rooms joined by shuffled doors; the C side
(n64_shim/rando_doors.c) deals each group's doors out again among
themselves and keeps a dealing only if the group stays connected, so every
room is reached at the same point of the story as in the original game, and
the item randomizer's logic still holds.

Writes port/3ds/source/n64_shim/rando_doors.inc, one
  DOOR(zone, "mapA", entryA, nodeA, "mapB", entryB, nodeB)
per door (node: the room, or room side, the entrance is in), for every
version (the maps are the same in all three).

usage: rando_doors.py [--list]
"""
import os
import re
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
PORT = os.path.join(ROOT, "port", "3ds")
WORLD = os.path.join(ROOT, "src", "world")

AREAS = ("kmr", "mac", "nok", "sbk")

KEEP = {
    # Goomba Village in the prologue: Goompa (a partner only then) talks at entrances he
    # expects you to come in by (kmr_05), the hammer block in kmr_03 splits it, and the
    # village gate scenes need the original way round
    ("kmr_00", 0): "prologue village", ("kmr_02", 1): "prologue village", ("kmr_02", 2): "prologue village",
    ("kmr_03", 0): "prologue village", ("kmr_03", 1): "prologue village", ("kmr_04", 0): "prologue village",
    ("kmr_05", 0): "prologue village", ("kmr_05", 1): "prologue village",
    # Goomba King's castle (boss)
    ("kmr_11", 0): "boss room", ("kmr_11", 1): "boss room",
}

PIN = {
    # the Goomba Bros. wait by kmr_07's east exit and ambush you coming from the west
    (("kmr_07", 1), ("kmr_12", 0)): "Goomba Bros. ambush",
}

REGIONS = {
    # kmr_02's road side: the village gate opens from STORY_CH0_TWINK_GAVE_LUCKY_STAR
    ("kmr_02", 0): "road",
    # Pleasant Path's bridges: nok_12 STORY_CH1_MADE_FIRST_BRIDGE, nok_14 _SECOND_BRIDGE (Kooper)
    ("nok_12", 0): "west", ("nok_12", 1): "east",
    ("nok_14", 0): "west", ("nok_14", 1): "east",
    # Koopa Village: the shell-thief scene plays as you first come in by nok_01's west entrance;
    # the rest of the village comes after it
    ("nok_01", 0): "west", ("nok_01", 1): "east",
}

ENTRY = re.compile(r"(\w+_ENTRY_\w+)\s*=\s*(\d+)")
SCRIPT = re.compile(r"EvtScript\s+N\((\w+)\)\s*=\s*(\{.*?\n\};|EVT_EXIT_\w+\([^;]*\);)", re.S)
MACRO = re.compile(r"EVT_EXIT_(WALK|WALK_NOK|WALK_FIXED|SINGLE_DOOR|SPLIT_SINGLE_DOOR|DOUBLE_DOOR)\(([^;]*)\);")
GOTO = re.compile(r"Call\((?:GotoMap|GotoMapSpecial)\s*,\s*(?:Ref\()?\"(\w+)\"\)?\s*,\s*(\w+)")
BIND = re.compile(r"BindTrigger\(Ref\(N\((\w+)\)\)")
HAND_EXIT = re.compile(r"Call\(UseExitHeading,\s*\w+,\s*(\w+)\)|Set\(LVar0,\s*(\w+_ENTRY_\w+)\)\s*\n\s*Set\(LVar1")


def map_dirs():
    dirs = set()
    for v in ("", "_us", "_ique"):
        mk = os.path.join(PORT, "world_files%s.mk" % v)
        found = set(m.group(1) for m in re.finditer(r"(?:^|\s)world/(area_\w+/\w+)/", open(mk).read()))
        if dirs and found != dirs:
            sys.exit("rando_doors.py: the versions compile different maps; give each its own table")
        dirs = found
    return sorted(dirs)


def scan():
    entries = {m.group(1): int(m.group(2)) for m in ENTRY.finditer(open(os.path.join(WORLD, "entrances.h")).read())}

    def val(s):
        s = s.strip()
        return entries[s] if s in entries else int(s, 0)

    exits, others = [], set()   # (map, exit, target map, target entry); (map, target map, target entry) elsewhere
    for d in map_dirs():
        mapname = d.split("/")[1]
        text = ""
        for dp, _, files in os.walk(os.path.join(WORLD, d)):
            for f in sorted(files):
                if f.endswith(".c") or f.endswith(".h"):
                    text += open(os.path.join(dp, f), errors="replace").read() + "\n"
        bound = set(BIND.findall(text))
        for m in SCRIPT.finditer(text):
            name, body = m.group(1), m.group(2)
            mm = MACRO.match(body)
            if mm:
                args = [a.strip() for a in mm.group(2).split(",")]
                if mm.group(1).startswith("WALK"):
                    args = args[1:]
                hit = (val(args[0]), args[1].strip('"'), val(args[2]))
                if name in bound:
                    exits.append((mapname,) + hit)
                else:
                    others.add((mapname, hit[1], hit[2]))
                continue
            gotos = GOTO.findall(body)
            ex = HAND_EXIT.search(body)
            door = "UseExitHeading" in body or re.search(r"Exec\(Exit\w*Door\)", body)
            if name in bound and ex and door and len(gotos) == 1:
                exits.append((mapname, val(ex.group(1) or ex.group(2)), gotos[0][0], val(gotos[0][1])))
            else:
                for tmap, te in gotos:
                    try:
                        others.add((mapname, tmap, val(te)))
                    except (KeyError, ValueError):
                        pass
    return exits, others


def main():
    exits, others = scan()
    fwd = {}
    for m, e, tm, te in exits:
        fwd.setdefault((m, e), set()).add((tm, te))
    pairs, why = [], {}
    for (m, e), ts in sorted(fwd.items()):
        if len(ts) != 1:
            continue   # an entrance that leaves for two places
        (t,) = ts
        if fwd.get(t) != {(m, e)} or (m, e) >= t:
            continue   # one-way, or listed from the other side
        a, b = (m, e), t
        if a[0][:3] != b[0][:3] or a[0][:3] not in AREAS:
            continue
        if a[0] == b[0]:
            why[(a, b)] = "same room"
        elif a in KEEP or b in KEEP:
            why[(a, b)] = KEEP.get(a) or KEEP.get(b)
        elif (a[0], b[0], b[1]) in others or (b[0], a[0], a[1]) in others:
            why[(a, b)] = "a cutscene goes the same way"
        elif (a, b) in PIN or (b, a) in PIN:
            why[(a, b)] = "pinned: " + (PIN.get((a, b)) or PIN.get((b, a)))
        pairs.append((a, b))

    def room(ep):
        return (ep[0], REGIONS.get(ep, ""))

    parent = {}

    def find(x):
        parent.setdefault(x, x)
        while parent[x] != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x

    for a, b in pairs:   # pinned doors join their rooms into one
        if why.get((a, b), "").startswith("pinned"):
            parent[find(room(a))] = find(room(b))
    shuffled = [(a, b) for a, b in pairs if (a, b) not in why]
    zone_of = {}
    zparent = {}

    def zfind(x):
        zparent.setdefault(x, x)
        while zparent[x] != x:
            zparent[x] = zparent[zparent[x]]
            x = zparent[x]
        return x

    for a, b in shuffled:
        zparent[zfind(find(room(a)))] = zfind(find(room(b)))
    groups = {}
    for a, b in shuffled:
        groups.setdefault(zfind(find(room(a))), []).append((a, b))
    zones = [g for g in sorted(groups.values()) if len(g) >= 2]
    nodes = {}
    out = os.path.join(PORT, "source", "n64_shim", "rando_doors.inc")
    with open(out, "w") as fh:
        fh.write("/* GENERATED by tools/rando_doors.py -- do not edit */\n")
        for z, g in enumerate(zones):
            fh.write("/* zone %d: %s */\n" % (z, " ".join(sorted(set(
                "%s%s" % (r[0], "(" + r[1] + ")" if r[1] else "") for r in (find(room(x)) for p in g for x in p))))))
            for a, b in g:
                na = nodes.setdefault(find(room(a)), len(nodes))
                nb = nodes.setdefault(find(room(b)), len(nodes))
                fh.write('DOOR(%d, "%s", %d, %d, "%s", %d, %d)\n' % (z, a[0], a[1], na, b[0], b[1], nb))
    n = sum(len(g) for g in zones)
    print("%d doors in %d groups over %d rooms -> %s" % (n, len(zones), len(nodes), os.path.relpath(out, ROOT)))
    kept = [(a, b, why.get((a, b), "alone in its group")) for a, b in pairs
            if not any((a, b) in g for g in zones)]
    print("%d doors of %s kept as they are" % (len(kept), "/".join(AREAS)))
    if "--list" in sys.argv:
        for a, b, w in kept:
            print("  %s %d <-> %s %d: %s" % (a[0], a[1], b[0], b[1], w))


if __name__ == "__main__":
    main()
