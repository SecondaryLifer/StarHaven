// discover.js: port of tools/hackrecomp/discover.py (see its docstring). Finds
// the code of a ROM hack: which sections it loads and the functions in them.
// Where the Python walks a set (its order is CPython's), this walks it sorted,
// so the sections can come out in another order; the content is the same.
import { Instruction, REG } from "./mips.js";

const EVT_CALL = 0x43;
const NOT_GAME_CODE = new Set(["mtc0", "mfc0", "dmtc0", "dmfc0", "cache", "eret", "tlbr", "tlbwi", "tlbwr", "tlbp", "teq",
    "tne", "tge", "tgeu", "tlt", "tltu", "teqi", "tnei", "tgei", "tgeiu", "tlti", "tltiu", "break", "syscall",
    "sync", "lwc2", "swc2", "ldc2", "sdc2", "mtc2", "mfc2", "ctc2", "cfc2", "ll", "sc", "lld", "scd"]);
const MAX_FUNC = 0x10000;

const hex = (v, n) => v.toString(16).toUpperCase().padStart(n, "0");
const u32 = (b, o) => ((b[o] << 24) | (b[o + 1] << 16) | (b[o + 2] << 8) | b[o + 3]) >>> 0;
const numSort = (a, b) => a - b;
const sortedNums = (it) => [...it].sort(numSort);
const tupleCmp = (a, b) => { for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return a[i] - b[i]; return 0; };
const key2 = (a, b) => `${a},${b}`;                  // (rom, vram) -> a key
function bisectRight(arr, x) {
    let lo = 0, hi = arr.length;
    while (lo < hi) { const m = (lo + hi) >> 1; if (x < arr[m]) hi = m; else lo = m + 1; }
    return lo;
}

function overlayVrams(van) {
    return new Set(van.segments.filter((s) => s.functions.length && s.vram >= 0x80200000).map((s) => s.vram));
}

class Section {
    constructor(name, rom, vram, size, data) {
        this.name = name; this.rom = rom; this.vram = vram; this.size = size; this.data = data;
        this.funcs = new Map();     // vram -> size
        this.strong = new Set();
        this.weak = new Set();
        this.unique = true;
        this._ins = new Map();
    }
    contains(v) { return this.vram <= v && v < this.vram + this.size; }
    word(v) {
        const o = v - this.vram;
        // (the Python raises past its data; nothing it ran into)
        return o >= 0 && o <= this.size - 4 && o + 4 <= this.data.length ? u32(this.data, o) : null;
    }
    ins(v) {
        let i = this._ins.get(v);
        if (i === undefined) {
            const w = this.word(v);
            i = w === null ? null : new Instruction(w, v);
            this._ins.set(v, i);
        }
        return i;
    }
}

function isPrologue(sec, v) {
    const w = sec.word(v);
    return w !== null && (w >>> 16) === 0x27BD && (w & 0x8000) !== 0;
}

function inDelaySlot(sec, v) {
    if (isPrologue(sec, v)) return false;
    const i = sec.contains(v - 4) ? sec.ins(v - 4) : null;
    return i !== null && i.isValid() && (i.isBranch() || i.isJump());
}

function jumpTableTargets(sec, start, pc) {
    const jr = sec.ins(pc);
    const reg = jr.rs;
    let hi = null, lo = null, base = null;
    for (let back = 1; back < 12; back++) {
        const i = sec.ins(pc - 4 * back);
        if (i === null) break;
        const name = i.getOpcodeName();
        if (lo === null && name === "lw" && i.rt === reg) {
            lo = i.getProcessedImmediate(); base = i.rs;
            continue;
        }
        if (lo !== null && name === "lui" && i.rt === base) {
            hi = i.getProcessedImmediate();
            break;
        }
    }
    if (hi === null || lo === null) return [];
    let table = (hi * 65536 + lo) >>> 0;
    const out = [];
    while (sec.contains(table) && out.length < 512) {
        const t = sec.word(table);
        if (t === null || !(start <= t && t < start + MAX_FUNC) || (t & 3) || !sec.contains(t)) break;
        out.push(t);
        table += 4;
    }
    return out;
}

/** [end (exclusive) or null if not code, calls] */
function extent(sec, start, limit) {
    let pc = start, maxt = start;
    const calls = new Set();
    const endBound = Math.min(limit, sec.vram + sec.size, start + MAX_FUNC);
    const done = (end) => [end, new Set([...calls].filter((c) => !(start < c && c < end)))];
    while (pc < endBound) {
        const i = sec.ins(pc);
        if (i === null || !i.isValid() || NOT_GAME_CODE.has(i.getOpcodeName())) return [null, calls];
        if (i.isBranch()) {
            const t = i.getBranchVramGeneric();
            if (t < start) { /* a loop back or a shared tail */ } else if (t < endBound) maxt = Math.max(maxt, t);
        }
        if (i.isJumpWithAddress()) {
            const t = i.getInstrIndexAsVram();
            calls.add(t);
            if (i.getOpcodeName() === "j" && pc < t && t < endBound) maxt = Math.max(maxt, t);
        }
        if (i.isJumptableJump()) {
            const cases = jumpTableTargets(sec, start, pc);
            for (const t of cases) maxt = Math.max(maxt, t);
            if (!cases.length && maxt <= pc + 4) return done(pc + 8);
        }
        if (i.isJrRa() || (i.getOpcodeName() === "j" && !(start <= i.getInstrIndexAsVram() && i.getInstrIndexAsVram() < endBound))) {
            if (maxt <= pc + 4) return done(pc + 8);
        }
        pc += 4;
    }
    return [pc > start ? pc : null, new Set([...calls].filter((c) => !(start < c && c < pc)))];
}

function evtCalls(b) {
    const out = new Set();
    for (let o = 0; o < b.length - 12; o += 4) {
        if (u32(b, o) !== EVT_CALL) continue;
        const argc = u32(b, o + 4), fn = u32(b, o + 8);
        if (argc >= 1 && argc < 16 && fn >= 0x80000000 && fn < 0x80800000 && (fn & 3) === 0) out.add(fn);
    }
    return out;
}

function analyze(sec, extraStrong) {
    if (sec.unique) for (const v of extraStrong) if (sec.contains(v)) sec.strong.add(v);
    for (const v of evtCalls(sec.data)) if (sec.contains(v)) sec.strong.add(v);
    for (let o = 0; o < sec.size - 3; o += 4) {
        if (o + 4 > sec.data.length) break;
        const p = u32(sec.data, o);
        if (sec.contains(p) && (p & 3) === 0 && isPrologue(sec, p)) sec.weak.add(p);
    }
    const allCalls = new Set();
    let pending = sortedNums(new Set([...sec.strong, ...sec.weak]));
    const done = new Set();
    while (pending.length) {
        const cand = sortedNums(new Set([...pending, ...sec.funcs.keys()]));
        const starts = [];
        for (const s of cand) {
            if (inDelaySlot(sec, s) && starts.length) {
                const [prevEnd] = extent(sec, starts[starts.length - 1], sec.vram + sec.size);
                if (prevEnd !== null && prevEnd > s) continue;
            }
            starts.push(s);
        }
        pending = [];
        starts.forEach((s, idx) => {
            if (done.has(s)) return;
            done.add(s);
            const nxt = idx + 1 < starts.length ? starts[idx + 1] : sec.vram + sec.size;
            const [end, calls] = extent(sec, s, nxt);
            if (end === null) return;
            sec.funcs.set(s, end - s);
            for (const c of calls) allCalls.add(c);
            for (const t of calls) if (sec.contains(t) && !done.has(t) && !sec.funcs.has(t)) pending.push(t);
            let nv = end;
            while (nv < sec.vram + sec.size && sec.word(nv) === 0) nv += 4;
            if (isPrologue(sec, nv) && !done.has(nv)) pending.push(nv);
        });
    }
    const clean = new Map();
    let lastEnd = 0;
    for (const s of sortedNums(sec.funcs.keys())) {
        if (s < lastEnd) continue;
        clean.set(s, sec.funcs.get(s));
        lastEnd = s + sec.funcs.get(s);
    }
    sec.funcs = clean;
    return allCalls;
}

/** (start, end, ram) triples of a ROM's tables whose ram is an overlay address */
function tableTriples(rom, vrams) {
    const out = new Map();      // "s,e,d" -> [s, e, d]
    const n = rom.length;
    for (let o = 0; o < n - 12; o += 4) {
        const d = u32(rom, o + 8);
        if (!vrams.has(d)) continue;
        const s = u32(rom, o), e = u32(rom, o + 4);
        if (s >= 0x1000 && s < e && e <= n && e - s <= 0x100000 && (s & 3) === 0) out.set(`${s},${e},${d}`, [s, e, d]);
    }
    return out;
}

function mapInits(rom, vrams) {
    const out = new Map();      // key2(s, d) -> Set(init)
    const n = rom.length;
    for (let o = 4; o < n - 20; o += 4) {
        const d = u32(rom, o + 8);
        if (!vrams.has(d)) continue;
        const settings = u32(rom, o - 4), s = u32(rom, o), e = u32(rom, o + 4), init = u32(rom, o + 16);
        if (s >= 0x1000 && s < e && e <= n && e - s <= 0x100000 && (s & 3) === 0
            && d <= init && init < d + (e - s) && (init & 3) === 0 && d <= settings && settings < d + (e - s)) {
            const k = key2(s, d);
            if (!out.has(k)) out.set(k, new Set());
            out.get(k).add(init);
        }
    }
    return out;
}

function constDmas(sec, dmaFuncs) {
    const out = new Map();
    for (let pc = sec.vram; pc < sec.vram + sec.size - 4; pc += 4) {
        const i = sec.ins(pc);
        if (i === null || !i.isValid() || i.getOpcodeName() !== "jal" || !dmaFuncs.has(i.getInstrIndexAsVram())) continue;
        const regs = new Map();
        for (let v = pc - 40; v < pc + 8; v += 4) {
            const j = sec.ins(v);
            if (j === null) continue;
            const n = j.getOpcodeName();
            if (n === "lui") regs.set(j.rt, (j.getProcessedImmediate() * 65536) >>> 0);
            else if ((n === "ori" || n === "addiu") && regs.has(j.rs)) {
                const imm = j.getProcessedImmediate();
                regs.set(j.rt, n === "ori" ? (regs.get(j.rs) | imm) >>> 0 : (regs.get(j.rs) + imm) >>> 0);
            }
        }
        const a0 = regs.get(REG.a0), a1 = regs.get(REG.a1), a2 = regs.get(REG.a2);
        if (a0 !== undefined && a1 !== undefined && a2 !== undefined && a0 < a1 && a2 >= 0x80000000 && a2 < 0x80800000)
            out.set(`${a0},${a1},${a2}`, [a0, a1, a2]);
    }
    return out;
}

const sortedTriples = (m) => [...m.values()].sort(tupleCmp);

/**
 * van: elfinfo.py's json; vrom, hrom: Uint8Array (.z64). log(msg) reports
 * progress. Returns discover.py's json.
 */
export function discover(van, vrom, hrom, log = () => {}) {
    const syms = van.symbols;
    const vrams = overlayVrams(van);
    const replace = [], kept = [];
    let sections = [];
    const byRom = new Map();
    for (const s of van.segments) {
        if (!s.functions.length || s.name.startsWith("dead_") || s.name === "header" || s.name === "boot") continue;
        byRom.set(s.rom, s);
        let same = 0;
        for (const f of s.functions) {
            const o = f[1] - s.vram + s.rom;
            if (o + 4 <= hrom.length && o + 4 <= vrom.length
                && hrom[o] === vrom[o] && hrom[o + 1] === vrom[o + 1] && hrom[o + 2] === vrom[o + 2] && hrom[o + 3] === vrom[o + 3]) same++;
        }
        if (same >= 0.7 * s.functions.length) kept.push(s);
        else {
            replace.push(s.name);
            const sec = new Section(s.name + "_hack", s.rom, s.vram, s.size, hrom.subarray(s.rom, s.rom + s.size));
            for (const f of s.functions) if (isPrologue(sec, f[1])) sec.weak.add(f[1]);
            sections.push(sec);
        }
    }
    log("tables");
    const triples = tableTriples(hrom, vrams);
    const confirmed = [];
    for (const sec of sections) {
        const ends = [...triples.values()].filter(([s0, , d]) => s0 === sec.rom && d === sec.vram).map(([, e]) => e);
        if (!ends.length) continue;
        sec.size = Math.max(...ends) - sec.rom;
        sec.data = hrom.subarray(sec.rom, sec.rom + sec.size);
        sec._ins.clear();
        confirmed.push(sec);
    }
    sections = confirmed;
    const keptCode = kept.map((s) => new Section(s.name, s.rom, s.vram, s.size, hrom.subarray(s.rom, s.rom + s.size)));
    const dma = new Set(["dma_copy", "nuPiReadRom"].filter((n) => n in syms).map((n) => syms[n]));
    for (const k of keptCode) for (const [kk, t] of constDmas(k, dma)) triples.set(kk, t);
    const known = new Set([...kept.map((s) => key2(s.rom, s.vram)), ...sections.map((s) => key2(s.rom, s.vram))]);
    for (const [s, e, d] of sortedTriples(triples)) {
        if (known.has(key2(s, d))) continue;
        const v = byRom.get(s);
        if (v && v.vram === d && !replace.includes(v.name)) continue;
        known.add(key2(s, d));
        sections.push(new Section(`hack_${hex(s, 7)}`, s, d, e - s, hrom.subarray(s, e)));
    }
    const scanned = new Set();
    for (;;) {
        const todo = [...keptCode, ...sections].filter((k) => !scanned.has(key2(k.rom, k.vram)));
        if (!todo.length) break;
        for (const k of todo) {
            scanned.add(key2(k.rom, k.vram));
            for (const [s, e, d] of sortedTriples(constDmas(k, dma))) {
                if (!known.has(key2(s, d))) {
                    known.add(key2(s, d));
                    sections.push(new Section(`hack_${hex(s, 7)}`, s, d, e - s, hrom.subarray(s, e)));
                }
            }
        }
    }

    const byLoad = new Map(sections.map((x) => [key2(x.rom, x.vram), x]));
    for (const k of [...keptCode, ...sections]) {
        for (let pc = k.vram; pc < k.vram + k.size - 4; pc += 4) {
            const i = k.ins(pc);
            if (pc - 40 < k.vram || i === null || !i.isValid() || i.getOpcodeName() !== "jal" || !dma.has(i.getInstrIndexAsVram())) continue;
            const tmp = new Section(k.name, k.rom + pc - 40 - k.vram, pc - 40, 52, k.data.subarray(pc - 40 - k.vram, pc + 12 - k.vram));
            for (const [s, , d] of sortedTriples(constDmas(tmp, dma))) {
                const tgt = byLoad.get(key2(s, d));
                for (let v = pc + 8; v < pc + 24; v += 4) {
                    const j = k.ins(v);
                    if (j !== null && j.isValid() && j.isJumpWithAddress() && tgt !== undefined && tgt.contains(j.getInstrIndexAsVram()))
                        tgt.strong.add(j.getInstrIndexAsVram());
                }
            }
        }
    }
    for (const [k, inits] of mapInits(hrom, vrams)) {
        const sec = byLoad.get(k);
        if (sec) for (const v of inits) sec.strong.add(v);
    }

    const spans = [...sections.map((s) => [s.vram, s.vram + s.size]), ...kept.map((s) => [s.vram, s.vram + s.size])];
    for (const sec of sections)
        sec.unique = spans.filter(([a, b]) => a < sec.vram + sec.size && sec.vram < b).length === 1;

    log("calls");
    const strong = evtCalls(hrom);
    for (const k of keptCode) {
        const seg = kept.find((s) => s.name === k.name);
        for (const f of seg.functions) for (const c of extent(k, f[1], f[1] + f[2])[1]) strong.add(c);
    }
    for (let round = 0; round < 3; round++) {
        log(`code, round ${round + 1}`);
        const found = new Set();
        for (const sec of sections) for (const c of analyze(sec, strong)) found.add(c);
        if ([...found].every((c) => strong.has(c))) break;
        for (const c of found) strong.add(c);
    }

    const allCalls = new Set(strong);
    for (const sec of sections)
        for (const [v, n] of sec.funcs) for (const c of extent(sec, v, v + n)[1]) allCalls.add(c);
    const add = {}, entries = [], resize = {}, drop = {};
    const keptSpans = [...kept.map((s) => [s.vram, s.vram + s.size]), ...sections.map((x) => [x.vram, x.vram + x.size])];
    const allSorted = sortedNums(allCalls);
    kept.forEach((s, ki) => {
        const k = keptCode[ki];
        const fl = s.functions.map((f) => [f[1], f[1] + f[2], f[0]]).sort((a, b) => a[0] - b[0] || a[1] - b[1] || (a[2] < b[2] ? -1 : a[2] > b[2] ? 1 : 0));
        const starts = fl.map((f) => f[0]);
        const changed = (f) => {
            const o = s.rom + f[0] - s.vram, n = f[1] - f[0];
            const a = vrom.subarray(o, o + n), b = hrom.subarray(o, o + n);
            if (a.length !== b.length) return true;
            for (let x = 0; x < a.length; x++) if (a[x] !== b[x]) return true;
            return false;
        };
        const mine = allSorted.filter((c) => k.contains(c));
        if (keptSpans.filter(([a, b]) => a < k.vram + k.size && k.vram < b).length > 1) {
            for (const t of mine) {
                const i = bisectRight(starts, t) - 1;
                if (i >= 0 && fl[i][0] < t && t < fl[i][1] && changed(fl[i])) entries.push([fl[i][2], s.name, t]);
            }
            return;
        }
        for (const t of mine) {
            const i = bisectRight(starts, t) - 1;
            if (i >= 0 && fl[i][0] <= t && t < fl[i][1]) {
                const realEnd = changed(fl[i]) ? extent(k, fl[i][0], fl[i][1])[0] : null;
                if (!realEnd || t < realEnd) {
                    if (t !== fl[i][0]) entries.push([fl[i][2], s.name, t]);
                    continue;
                }
                (resize[s.name] ||= {})[fl[i][2]] = realEnd - fl[i][0];
            }
            let j = i + 1;
            while (j < fl.length && changed(fl[j])) j++;
            const nxt = j < fl.length ? starts[j] : k.vram + k.size;
            const [end] = extent(k, t, nxt);
            if (end) {
                (add[s.name] ||= []).push([`f_${hex(t, 8)}`, t, end - t]);
                const gone = fl.slice(i + 1, j).filter((f) => f[0] < end).map((f) => f[2]);
                if (gone.length) (drop[s.name] ||= []).push(...gone);
            }
        }
    });
    for (const sec of sections) {
        const fl = [...sec.funcs].sort((a, b) => a[0] - b[0]);
        const starts = fl.map(([v]) => v);
        const own = evtCalls(sec.data);
        for (const [v, n] of fl) for (const c of extent(sec, v, v + n)[1]) own.add(c);
        for (const t of sortedNums(sec.unique ? allCalls : own)) {
            if (!sec.contains(t)) continue;
            const i = bisectRight(starts, t) - 1;
            if (i >= 0 && starts[i] < t && t < starts[i] + fl[i][1]) entries.push([`f_${hex(starts[i], 8)}_${hex(sec.rom, 7)}`, sec.name, t]);
        }
    }

    const out = { sections: [], replace, add, entries, resize, drop };
    let nf = 0;
    for (const sec of sections) {
        if (!sec.funcs.size) continue;
        const fl = [...sec.funcs].sort((a, b) => a[0] - b[0]).map(([v, n]) => [`f_${hex(v, 8)}_${hex(sec.rom, 7)}`, v, n]);
        nf += fl.length;
        out.sections.push({ name: sec.name, rom: sec.rom, vram: sec.vram, size: sec.size, functions: fl });
    }
    log(`${kept.length} vanilla segments kept, ${replace.length} rebuilt, ${out.sections.length} sections to recompile from the hack, ` +
        `${nf} functions; ${Object.values(add).reduce((a, v) => a + v.length, 0)} new functions in kept segments, ` +
        `${entries.length} entries into the middle of functions`);
    return out;
}
