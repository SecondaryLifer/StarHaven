// worker.js: runs one tool (clang -cc1, ld.lld, N64Recomp) at a time on an
// in-memory file system. Messages:
//   {init, modules: {name: WebAssembly.Module}, files: {path: bytes|text}}  the shared files (/kit headers, /work headers)
//   {job, id, tool, args, files, outputs: [paths], outDir}  -> {id, ok, outputs: {path: bytes}, log}
import { MemFS, Wasi } from "./wasi.js";

let modules = {};
let shared = {};

self.onmessage = (ev) => {
    const m = ev.data;
    if (m.init) {
        modules = { ...modules, ...m.modules };
        shared = { ...shared, ...m.files };
        self.postMessage({ init: true });
        return;
    }
    const { id, tool, args, files, outputs = [], outDir } = m;
    let log = "";
    try {
        const fs = new MemFS();
        for (const [p, d] of Object.entries(shared)) fs.writeFile(p, d);
        for (const [p, d] of Object.entries(files || {})) fs.writeFile(p, d);
        for (const d of ["/out", "/tmp"]) fs.mkdirs(d);
        const wasi = new Wasi({ args, fs, env: { PWD: "/work" }, stdout: (s) => { log += s; }, stderr: (s) => { log += s; } });
        const code = wasi.run(modules[tool]);
        const out = {};
        for (const p of outputs) {
            const d = fs.readFile(p);
            if (d) out[p] = d.slice();
        }
        if (outDir) {                       // everything a tool wrote under a folder (N64Recomp's funcs/)
            const dir = fs.lookup(outDir);
            if (dir instanceof Map) for (const [name, node] of dir) if (!(node instanceof Map)) out[`${outDir}/${name}`] = node.data.slice(0, node.size);
        }
        const ok = code === 0;
        self.postMessage({ id, ok, code, log, outputs: out }, Object.values(out).map((b) => b.buffer));
    } catch (e) {
        self.postMessage({ id, ok: false, log: log + "\n" + (e && e.stack || e) });
    }
};
