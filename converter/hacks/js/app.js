// app.js: the ROM hack panel of the converter page. Downloads the kit and
// the WebAssembly tools on first use, then runs pipeline.js in workers.
import { loadPack } from "./pack.js";
import { browserTools } from "./browsertools.js";
import { convert, hackRomFrom, slug } from "./pipeline.js";

const ASSETS = new URL("../", import.meta.url);      // core.pack, vanilla.pack, *.wasm.gz next to js/
const $ = (id) => document.getElementById(id);

let assets = null;                                   // {kit, modules}

async function fetchGz(name, onProgress) {
    const res = await fetch(new URL(name, ASSETS));
    if (!res.ok) throw new Error(`couldn't download ${name} (${res.status})`);
    const total = Number(res.headers.get("content-length")) || 0;
    let got = 0;
    const counted = res.body.pipeThrough(new TransformStream({ transform(c, ctl) { got += c.length; onProgress(got, total); ctl.enqueue(c); } }));
    return new Uint8Array(await new Response(counted.pipeThrough(new DecompressionStream("gzip"))).arrayBuffer());
}

async function loadAssets(say) {
    if (assets) return assets;
    const parts = [["core.pack", 4e6], ["vanilla.pack", 14e6], ["llvm.wasm.gz", 0], ["n64recomp.wasm.gz", 0]];
    const got = parts.map(() => 0), totals = parts.map(([, t]) => t);
    const show = () => {
        const g = got.reduce((a, b) => a + b, 0), t = totals.reduce((a, b) => a + b, 0);
        say(`Downloading the tools (once): ${(g / 1e6).toFixed(0)}${t ? ` of ${(t / 1e6).toFixed(0)}` : ""} MB`, t ? 0.9 * g / t : 0);
    };
    const prog = (i) => (g, t) => { got[i] = g; if (t) totals[i] = t; show(); };
    const [core, van, llvm, recomp] = await Promise.all([
        loadPack(new URL("core.pack", ASSETS), prog(0)), loadPack(new URL("vanilla.pack", ASSETS), prog(1)),
        fetchGz("llvm.wasm.gz", prog(2)), fetchGz("n64recomp.wasm.gz", prog(3))]);
    say("Preparing the compiler", 0.95);
    const kitMap = new Map([...core, ...van]);
    const modules = { llvm: await WebAssembly.compile(llvm), n64recomp: await WebAssembly.compile(recomp) };
    const dec = new TextDecoder();
    const kit = {
        map: kitMap,
        file: (p) => kitMap.get(p.replace(/^\/kit\//, "")),
        vanillaJson: JSON.parse(dec.decode(kitMap.get("vanilla.json"))),
        vanillaHashes: new Set(dec.decode(kitMap.get("vanilla.idx")).split("\n").filter(Boolean)),
        cc1: JSON.parse(dec.decode(kitMap.get("cc1.json"))),
    };
    assets = { kit, modules };
    return assets;
}

const readFile = (f) => f.arrayBuffer().then((b) => new Uint8Array(b));

function download(data, name, type = "application/octet-stream") {
    const url = URL.createObjectURL(new Blob([data], { type }));
    const a = Object.assign(document.createElement("a"), { href: url, download: name });
    document.body.append(a); a.click(); a.remove();
    setTimeout(() => URL.revokeObjectURL(url), 60000);
}

function threads() {
    const cores = navigator.hardwareConcurrency || 4;
    const mem = navigator.deviceMemory || 8;          // GB (Chrome only); a compile takes up to ~1 GB
    return Math.max(1, Math.min(cores, 8, mem));
}

export function setup() {
    const hackIn = $("hk-hack"), romIn = $("hk-rom"), nameIn = $("hk-name"), go = $("hk-go");
    const bar = $("hk-bar"), stage = $("hk-stage"), log = $("hk-log"), result = $("hk-result");
    hackIn.addEventListener("change", () => {
        const f = hackIn.files[0];
        if (f && !nameIn.dataset.touched) nameIn.value = f.name.replace(/\.(bps|ips|z64|n64|v64)$/i, "").replace(/[_]+/g, " ").trim();
    });
    nameIn.addEventListener("input", () => { nameIn.dataset.touched = "1"; });
    const say = (text, frac) => {
        stage.textContent = text;
        if (frac !== undefined) bar.value = frac;
    };
    const addLog = (line) => { log.textContent += line + "\n"; };

    go.addEventListener("click", async () => {
        const hackFile = hackIn.files[0], romFile = romIn.files[0];
        const name = nameIn.value.trim() || (hackFile && hackFile.name) || "ROM hack";
        result.hidden = true;
        log.textContent = "";
        stage.classList.remove("bad");
        if (!hackFile) { say("Pick the hack (its patch or the patched ROM) first."); stage.classList.add("bad"); return; }
        go.disabled = true;
        $("hk-progress").hidden = false;
        let tools = null;
        try {
            const vanilla = romFile ? await readFile(romFile) : null;
            const { vanilla: v, rom } = hackRomFrom(vanilla, await readFile(hackFile), hackFile.name);
            if (!v) throw new Error("pick your Paper Mario (USA) ROM too: the converter needs the original game to tell the hack's code from it");
            const { kit, modules } = await loadAssets(say);
            tools = browserTools({ modules, kit: kit.map, threads: threads() });
            addLog(`${threads()} threads`);
            const t0 = performance.now();
            const res = await convert({ kit, tools, vanilla: v, rom, name, log: addLog,
                progress: (f, s) => say(`${s}…`, f) });
            const secs = ((performance.now() - t0) / 1000).toFixed(0);
            say(`Done in ${secs} s.`, 1);
            const file = slug(name);
            $("hk-get-app").onclick = () => download(res.app, `${file}.3dsx`);
            $("hk-get-rom").onclick = () => download(res.rom, `${file}.z64`);
            $("hk-app-name").textContent = `${file}.3dsx`;
            $("hk-rom-name").textContent = `${file}.z64`;
            $("hk-save-name").textContent = `${res.id}.fla`;
            result.hidden = false;
        } catch (e) {
            stage.classList.add("bad");
            if (e.log && /llvm|n64recomp/.test(e.message))   // the hack itself: something the converter can't handle yet
                say("Didn't work: the converter can't handle this hack yet. Please tell us in #bug-reports on the "
                    + "Star Haven Discord which hack it is (the details below help).");
            else
                say(`Didn't work: ${e.message}`);
            if (e.log) addLog(e.log);
            console.error(e);
        } finally {
            if (tools) tools.pool.terminate();
            go.disabled = false;
        }
    });
}
