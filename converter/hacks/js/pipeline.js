// pipeline.js: patched ROM -> .3dsx for the hack runtime, with the tools
// handed in (WebAssembly workers in the browser, native ones in tests).
//
// Paths the tools see: /kit (the kit: headers, libraries, runtime, prebuilt
// vanilla code), /work (this hack's generated files), /out (objects, the ELF).
//
// tools = {
//   n64recomp(files): {path: text} of what N64Recomp wrote under funcs/
//                     (files: {path: Uint8Array|string} for /work)
//   compile(jobs):    one object per job ({args, src: path, text}), as Uint8Arrays,
//                     in job order; jobs run in parallel
//   link(args, objs): the ELF (objs: {path: Uint8Array} for /out)
// }
import { applyBps, applyIps, crc32, header, toZ64 } from "./romtools.js";
import { discover } from "./discover.js";
import { genSyms } from "./gensyms.js";
import { bodies, writeEntries } from "./entries.js";
import { sha1Hex } from "./sha1.js";
import { elfTo3dsx, smdhWithTitles } from "./threedsx.js";

export const VANILLA_US_CRC32 = 0xA7F5CD7E;          // Paper Mario (USA), .z64
const CHUNK_BYTES = 256 * 1024;                     // generated C per compile job

/** "Black Pit v1.4.1" -> "blackpit-v1-4-1" (file names on the SD card) */
export function slug(name) {
    return name.toLowerCase().replace(/[^a-z0-9]+/g, "-").replace(/^-|-$/g, "").slice(0, 40) || "hack";
}

/** the hack ROM from what the user gave: a patch (with the vanilla ROM) or a patched ROM */
export function hackRomFrom(vanilla, hack, hackName) {
    const v = vanilla && toZ64(vanilla);
    if (vanilla && !v) throw new Error("the Paper Mario ROM doesn't look like an N64 ROM");
    const lower = (hackName || "").toLowerCase();
    if (lower.endsWith(".bps") || String.fromCharCode(...hack.subarray(0, 4)) === "BPS1") {
        if (!v) throw new Error("a patch needs your Paper Mario (USA) ROM too");
        return { vanilla: v, rom: applyBps(v, hack) };
    }
    if (lower.endsWith(".ips") || String.fromCharCode(...hack.subarray(0, 5)) === "PATCH") {
        if (!v) throw new Error("a patch needs your Paper Mario (USA) ROM too");
        return { vanilla: v, rom: applyIps(v, hack) };
    }
    const rom = toZ64(hack);
    if (!rom) throw new Error(`${hackName || "the hack"} is neither a patch (.bps, .ips) nor an N64 ROM`);
    return { vanilla: v, rom };
}

/**
 * kit: {file(path) -> Uint8Array, vanillaJson, vanillaHashes: Set, cc1: cc1.json}
 * vanilla: the vanilla US ROM (.z64); rom: the hack's. name: shown on the 3DS.
 * log(text), progress(fraction 0..1, stage).
 */
export async function convert({ kit, tools, vanilla, rom, name, log = () => {}, progress = () => {} }) {
    const t0 = performance.now();
    const step = (m) => log(`${((performance.now() - t0) / 1000).toFixed(1)} s  ${m}`);
    if (crc32(vanilla) !== VANILLA_US_CRC32)
        throw new Error("the Paper Mario ROM isn't the USA release (the hacks are made for it)");
    const hdr = header(rom);
    const id = slug(name);

    progress(0.02, "Looking for the hack's code");
    const hackJson = discover(kit.vanillaJson, vanilla, rom, (m) => step(`discover: ${m}`));

    const work = genSyms(kit.vanillaJson, rom, "/work/rom.z64", hackJson);
    step(`gen_syms: ${work.summary}`);
    delete work.summary;

    progress(0.08, "Recompiling the MIPS code to C");
    const recompIn = { ...work, "rom.z64": rom };
    const funcFiles = await tools.n64recomp(recompIn);
    const nFiles = Object.keys(funcFiles).length;
    step(`N64Recomp: ${nFiles} files`);
    const funcs = bodies(Object.fromEntries(Object.entries(funcFiles).filter(([p]) => /funcs_\d+\.c$/.test(p))));
    const ent = writeEntries(JSON.parse(work["entries.json"]), funcs);
    step(`entries: ${ent.summary}`);

    // what vanilla doesn't have
    const fresh = [];
    let reused = 0;
    for (const lines of Object.values(funcs)) {
        const text = lines.join("\n");
        if (kit.vanillaHashes.has(sha1Hex(text))) reused++;
        else fresh.push(text);
    }
    step(`${fresh.length} functions to compile, ${reused} from the vanilla game`);

    const files = {};
    for (const [p, t] of Object.entries(work)) files[`/work/${p}`] = t;
    for (const [p, t] of Object.entries(funcFiles)) files[`/work/funcs/${p.replace(/^funcs\//, "")}`] = t;
    files["/work/funcs/funcs_entries.c"] = ent["funcs/funcs_entries.c"];
    files["/work/entry_table.inc"] = ent["entry_table.inc"];

    // the jobs: chunks of new functions, the per-hack generated files, rt_tables.c
    const head = '#include "recomp.h"\n#include "funcs.h"\n\n';
    const jobs = [];
    let cur = [], size = 0;
    const flush = () => {
        if (!cur.length) return;
        const p = `/work/chunks/c${jobs.length}.c`;
        files[p] = head + cur.join("\n") + "\n";
        jobs.push({ src: p, args: [...kit.cc1.recomp, "-include", "/work/hle_decls.h"] });
        cur = []; size = 0;
    };
    fresh.sort((a, b) => b.length - a.length);       // big ones first: they set the pace
    for (const t of fresh) {
        cur.push(t); size += t.length;
        if (size >= CHUNK_BYTES) flush();
    }
    flush();
    for (const p of ["/work/funcs/funcs_hle.c", "/work/funcs/funcs_entries.c"])
        jobs.push({ src: p, args: [...kit.cc1.recomp, "-include", "/work/hle_decls.h"] });
    const q = (s) => `"${s.replace(/[\\"]/g, "\\$&")}"`;
    jobs.push({ src: "/kit/rt/src/rt_tables.c", args: [...kit.cc1.tables, `-DPM3DS_HACK_ID=${q(id)}`,
        `-DPM3DS_HACK_NAME=${q(name)}`, `-DPM3DS_HACK_CRC=0x${hdr.crc1.toString(16).toUpperCase()}u`] });
    for (const j of jobs) j.out = j.src.replace(/^\/(work|kit)\/.*\//, "/out/").replace(/\.c$/, ".o");
    jobs[jobs.length - 1].out = "/out/rt_tables.o";

    progress(0.15, "Compiling");
    const objs = await tools.compile(jobs, files, (done) => progress(0.15 + 0.75 * done / jobs.length, "Compiling"));
    step(`compiled ${jobs.length} files`);

    progress(0.92, "Linking");
    const objMap = {};
    jobs.forEach((j, i) => { objMap[j.out] = objs[i]; });
    const args = [...kit.cc1.lld, ...kit.cc1.runtime, ...jobs.map((j) => j.out), "/kit/lib/libvanilla.a", ...kit.cc1.lld_libs, "-o", "/out/hack.elf"];
    const elf = await tools.link(args, objMap);
    step(`linked: ${(elf.length / 1e6).toFixed(1)} MB`);

    progress(0.97, "Packing the .3dsx");
    const short = name.length > 60 ? name.slice(0, 60) : name;
    const smdh = smdhWithTitles(kit.file("/kit/template.smdh"), short, `${short} (Star Haven)`, "Star Haven");
    const app = elfTo3dsx(elf, smdh);
    step(`.3dsx: ${(app.length / 1e6).toFixed(1)} MB`);
    progress(1, "Done");
    return { app, rom, id, name, crc1: hdr.crc1, fresh: fresh.length, reused };
}
