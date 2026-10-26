// browsertools.js: pipeline.js's tools on a pool of workers (worker.js) running
// the WebAssembly builds of clang/lld (one multi-call binary) and N64Recomp.

const enc = new TextEncoder();
const bytes = (d) => (typeof d === "string" ? enc.encode(d) : d);

class Pool {
    constructor(n, modules, files) {
        this.workers = [];
        this.idle = [];
        this.waiting = [];
        this.pending = new Map();
        this.nextId = 1;
        this.ready = Promise.all(Array.from({ length: n }, () => {
            const w = new Worker(new URL("./worker.js", import.meta.url), { type: "module" });
            w.onmessage = (ev) => this.message(w, ev.data);
            w.onerror = (e) => console.error("worker", e);
            this.workers.push(w);
            return new Promise((res) => { w.initDone = res; w.postMessage({ init: true, modules, files }); });
        })).then(() => { this.idle = [...this.workers]; });
    }
    message(w, m) {
        if (m.init) { w.initDone(); return; }
        const p = this.pending.get(m.id);
        this.pending.delete(m.id);
        this.release(w);
        if (m.ok) p.res(m); else p.rej(Object.assign(new Error(`${p.tool} failed`), { log: m.log }));
    }
    release(w) {
        const next = this.waiting.shift();
        if (next) next(w); else this.idle.push(w);
    }
    acquire() {
        const w = this.idle.pop();
        return w ? Promise.resolve(w) : new Promise((res) => this.waiting.push(res));
    }
    /** more shared files on every worker (a hack's headers) */
    async share(files) {
        await this.ready;
        await Promise.all(this.workers.map((w) => new Promise((res) => { w.initDone = res; w.postMessage({ init: true, modules: {}, files }); })));
    }
    async run(job) {
        await this.ready;
        const w = await this.acquire();
        const id = this.nextId++;
        return new Promise((res, rej) => {
            this.pending.set(id, { res, rej, tool: job.tool });
            w.postMessage({ ...job, id });
        });
    }
    terminate() { for (const w of this.workers) w.terminate(); }
}

/**
 * modules: {llvm, n64recomp} (WebAssembly.Module); kit: Map(path -> bytes)
 * of the core and vanilla packs, paths relative to /kit.
 */
export function browserTools({ modules, kit, threads }) {
    const headers = {};
    for (const [p, d] of kit) if (/^(res|sys|rt)\//.test(p)) headers[`/kit/${p}`] = d;
    const pool = new Pool(threads, modules, headers);
    let linkFiles = null;
    return {
        pool,
        async n64recomp(files) {
            const inFiles = {};
            for (const [p, d] of Object.entries(files)) inFiles[`/work/${p}`] = bytes(d);
            const r = await pool.run({ tool: "n64recomp", args: ["N64Recomp", "/work/recomp.toml"], files: inFiles, outDir: "/work/funcs" });
            const out = {};
            const dec = new TextDecoder();
            for (const [p, d] of Object.entries(r.outputs)) {
                const name = p.slice("/work/funcs/".length);
                if (!(`funcs/${name}` in files)) out[name] = dec.decode(d);
            }
            return out;
        },
        async compile(jobs, files, onDone) {
            // headers to every worker once; each job sends its own source
            const shared = {};
            for (const [p, d] of Object.entries(files)) if (!p.endsWith(".c") && !p.endsWith(".z64")) shared[p] = bytes(d);
            await pool.share(shared);
            let done = 0;
            return Promise.all(jobs.map(async (j) => {
                const src = j.src.startsWith("/kit/") ? {} : { [j.src]: bytes(files[j.src]) };
                const r = await pool.run({ tool: "llvm", args: ["clang", ...j.args, "-o", j.out, j.src], files: src, outputs: [j.out] });
                onDone(++done);
                return r.outputs[j.out];
            }));
        },
        async link(args, objs) {
            if (!linkFiles) {
                linkFiles = {};
                for (const [p, d] of kit) if (/^(lib|runtime)\//.test(p)) linkFiles[`/kit/${p}`] = d;
            }
            const r = await pool.run({ tool: "llvm", args: ["ld.lld", ...args], files: { ...linkFiles, ...objs }, outputs: ["/out/hack.elf"] });
            return r.outputs["/out/hack.elf"];
        },
    };
}
