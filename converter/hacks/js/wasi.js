// wasi.js: enough of WASI preview1 for clang, lld and N64Recomp built for
// wasm32-wasip1, on an in-memory file system. One Wasi per run (a WASI
// command ends with proc_exit).

const E = { SUCCESS: 0, BADF: 8, EXIST: 20, INVAL: 28, ISDIR: 31, NOENT: 44, NOSYS: 52, NOTDIR: 54, NOTEMPTY: 55, NOTSUP: 58, SPIPE: 70 };
const FT = { CHAR: 2, DIR: 3, FILE: 4 };
const O = { CREAT: 1, DIRECTORY: 2, EXCL: 4, TRUNC: 8 };
const FD_APPEND = 1;
const ALL_RIGHTS = 0x1FFFFFFFn;

export class Exit extends Error {
    constructor(code) { super(`exit ${code}`); this.code = code; }
}

/** an in-memory tree: directories are Maps, files {data, size} */
export class MemFS {
    constructor() { this.root = new Map(); }
    static split(path) { return path.split("/").filter((p) => p && p !== "."); }
    /** node at an absolute path, or null */
    lookup(path) {
        let n = this.root;
        const stack = [];
        for (const p of MemFS.split(path)) {
            if (p === "..") { n = stack.pop() || this.root; continue; }
            if (!(n instanceof Map)) return null;
            stack.push(n);
            n = n.get(p);
            if (n === undefined) return null;
        }
        return n;
    }
    /** the parent directory of path (created when mkdirs) and the last name */
    parent(path, mkdirs = false) {
        const parts = MemFS.split(path);
        const name = parts.pop();
        let n = this.root;
        const stack = [];
        for (const p of parts) {
            if (p === "..") { n = stack.pop() || this.root; continue; }
            stack.push(n);
            let c = n.get(p);
            if (c === undefined && mkdirs) { c = new Map(); n.set(p, c); }
            if (!(c instanceof Map)) return [null, name];
            n = c;
        }
        return [n, name];
    }
    writeFile(path, data) {
        const [dir, name] = this.parent(path, true);
        const bytes = typeof data === "string" ? new TextEncoder().encode(data) : data;
        dir.set(name, { data: bytes, size: bytes.length });
    }
    readFile(path) {
        const n = this.lookup(path);
        if (!n || n instanceof Map) return null;
        return n.data.subarray(0, n.size);
    }
    mkdirs(path) { this.parent(path + "/x", true); }
}

const inodes = new WeakMap();
let nextInode = 2;
function inode(node) {
    let n = inodes.get(node);
    if (n === undefined) { n = nextInode++; inodes.set(node, n); }
    return n;
}

const join = (base, rel) => (rel.startsWith("/") ? rel : `${base.replace(/\/$/, "")}/${rel}`);

export class Wasi {
    /** args: argv (argv[0] the program); fs: MemFS; env: {} */
    constructor({ args, fs, env = {}, stdout = () => {}, stderr = () => {} }) {
        this.args = args; this.fs = fs; this.env = Object.entries(env).map(([k, v]) => `${k}=${v}`);
        this.out = stdout; this.err = stderr;
        this.fds = new Map([[0, { tty: true }], [1, { tty: true, write: stdout }], [2, { tty: true, write: stderr }],
            [3, { node: fs.root, path: "/", preopen: "/" }]]);
        this.nextFd = 4;
        this.memory = null;
        const self = this;
        this.imports = { wasi_snapshot_preview1: new Proxy({}, {
            get(_, name) {
                const f = self[name];
                if (typeof f !== "function") return () => E.NOSYS;
                return (...a) => {
                    try { return f.apply(self, a); } catch (e) {
                        if (e instanceof Exit) throw e;
                        console.error(`wasi ${name}`, e);
                        return E.INVAL;
                    }
                };
            },
        }) };
    }

    /** run a module (WebAssembly.Module); returns the exit code */
    run(module) {
        const inst = new WebAssembly.Instance(module, this.imports);
        this.memory = inst.exports.memory;
        try {
            inst.exports._start();
        } catch (e) {
            if (e instanceof Exit) return e.code;
            throw e;
        }
        return 0;
    }

    get u8() { return new Uint8Array(this.memory.buffer); }
    get dv() { return new DataView(this.memory.buffer); }
    str(ptr, len) { return new TextDecoder().decode(this.u8.subarray(ptr, ptr + len)); }
    putStrings(list, ptrs, buf) {
        const dv = this.dv, u8 = this.u8;
        for (const s of list) {
            const b = new TextEncoder().encode(s + "\0");
            dv.setUint32(ptrs, buf, true); ptrs += 4;
            u8.set(b, buf); buf += b.length;
        }
        return E.SUCCESS;
    }
    sizes(list, countPtr, sizePtr) {
        const dv = this.dv;
        dv.setUint32(countPtr, list.length, true);
        dv.setUint32(sizePtr, list.reduce((a, s) => a + new TextEncoder().encode(s).length + 1, 0), true);
        return E.SUCCESS;
    }
    args_get(argv, buf) { return this.putStrings(this.args, argv, buf); }
    args_sizes_get(c, s) { return this.sizes(this.args, c, s); }
    environ_get(env, buf) { return this.putStrings(this.env, env, buf); }
    environ_sizes_get(c, s) { return this.sizes(this.env, c, s); }

    clock_res_get(_id, res) { this.dv.setBigUint64(res, 1000n, true); return E.SUCCESS; }
    clock_time_get(id, _prec, out) {
        const ns = id === 0 ? BigInt(Date.now()) * 1000000n : BigInt(Math.round(performance.now() * 1e6));
        this.dv.setBigUint64(out, ns, true);
        return E.SUCCESS;
    }
    random_get(buf, len) {
        for (let o = 0; o < len; o += 65536) crypto.getRandomValues(this.u8.subarray(buf + o, buf + Math.min(len, o + 65536)));
        return E.SUCCESS;
    }
    sched_yield() { return E.SUCCESS; }
    proc_exit(code) { throw new Exit(code); }
    poll_oneoff(inp, out, n, nevents) {
        const dv = this.dv;
        for (let i = 0; i < n; i++) {
            const s = inp + i * 48;
            dv.setBigUint64(out + i * 32, dv.getBigUint64(s, true), true);   // userdata
            dv.setUint16(out + i * 32 + 8, 0, true);                         // error
            dv.setUint8(out + i * 32 + 10, dv.getUint8(s + 8));              // type
        }
        dv.setUint32(nevents, n, true);
        return E.SUCCESS;
    }

    fd(fd) { return this.fds.get(fd); }
    fd_close(fd) { if (!this.fds.has(fd)) return E.BADF; this.fds.delete(fd); return E.SUCCESS; }
    fd_advise() { return E.SUCCESS; }
    fd_allocate(fd, off, len) {
        const f = this.fd(fd);
        if (!f || !f.node || f.node instanceof Map) return E.BADF;
        this.grow(f.node, Number(off + len));
        return E.SUCCESS;
    }
    fd_datasync() { return E.SUCCESS; }
    fd_sync() { return E.SUCCESS; }
    fd_fdstat_get(fd, buf) {
        const f = this.fd(fd);
        if (!f) return E.BADF;
        const dv = this.dv;
        dv.setUint8(buf, f.tty ? FT.CHAR : f.node instanceof Map ? FT.DIR : FT.FILE);
        dv.setUint16(buf + 2, f.append ? FD_APPEND : 0, true);
        dv.setBigUint64(buf + 8, ALL_RIGHTS, true);
        dv.setBigUint64(buf + 16, ALL_RIGHTS, true);
        return E.SUCCESS;
    }
    fd_fdstat_set_flags(fd, flags) { const f = this.fd(fd); if (!f) return E.BADF; f.append = !!(flags & FD_APPEND); return E.SUCCESS; }
    fd_fdstat_set_rights() { return E.SUCCESS; }
    stat(buf, node, tty = false) {
        const dv = this.dv;
        for (let i = 0; i < 64; i += 8) dv.setBigUint64(buf + i, 0n, true);
        // a different inode for every file and folder: clang tells include folders apart by it
        dv.setBigUint64(buf + 8, BigInt(node ? inode(node) : 1), true);
        dv.setUint8(buf + 16, tty ? FT.CHAR : node instanceof Map ? FT.DIR : FT.FILE);
        dv.setBigUint64(buf + 24, 1n, true);
        dv.setBigUint64(buf + 32, BigInt(node && !(node instanceof Map) ? node.size : 0), true);
        return E.SUCCESS;
    }
    fd_filestat_get(fd, buf) { const f = this.fd(fd); if (!f) return E.BADF; return this.stat(buf, f.node, f.tty); }
    fd_filestat_set_size(fd, size) {
        const f = this.fd(fd);
        if (!f || !f.node || f.node instanceof Map) return E.BADF;
        const n = Number(size);
        this.grow(f.node, n);
        if (n < f.node.size) f.node.data.fill(0, n, f.node.size);
        f.node.size = n;
        return E.SUCCESS;
    }
    fd_filestat_set_times() { return E.SUCCESS; }
    fd_prestat_get(fd, buf) {
        const f = this.fd(fd);
        if (!f || !f.preopen) return E.BADF;
        this.dv.setUint8(buf, 0);
        this.dv.setUint32(buf + 4, new TextEncoder().encode(f.preopen).length, true);
        return E.SUCCESS;
    }
    fd_prestat_dir_name(fd, ptr, len) {
        const f = this.fd(fd);
        if (!f || !f.preopen) return E.BADF;
        this.u8.set(new TextEncoder().encode(f.preopen).subarray(0, len), ptr);
        return E.SUCCESS;
    }
    iovs(iovs, n) {
        const dv = this.dv, out = [];
        for (let i = 0; i < n; i++) out.push([dv.getUint32(iovs + i * 8, true), dv.getUint32(iovs + i * 8 + 4, true)]);
        return out;
    }
    grow(node, size) {
        if (size <= node.data.length) return;
        const d = new Uint8Array(Math.max(size, node.data.length * 2, 4096));
        d.set(node.data.subarray(0, node.size));
        node.data = d;
    }
    readAt(f, iovs, n, pos) {
        let total = 0;
        const u8 = this.u8;
        for (const [p, len] of this.iovs(iovs, n)) {
            const k = Math.max(0, Math.min(len, f.node.size - pos));
            u8.set(f.node.data.subarray(pos, pos + k), p);
            pos += k; total += k;
            if (k < len) break;
        }
        return total;
    }
    writeAt(f, iovs, n, pos) {
        let total = 0;
        const u8 = this.u8;
        for (const [p, len] of this.iovs(iovs, n)) {
            this.grow(f.node, pos + len);
            f.node.data.set(u8.subarray(p, p + len), pos);
            pos += len; total += len;
            if (pos > f.node.size) f.node.size = pos;
        }
        return total;
    }
    fd_read(fd, iovs, n, nread) {
        const f = this.fd(fd);
        if (!f) return E.BADF;
        if (f.tty) { this.dv.setUint32(nread, 0, true); return E.SUCCESS; }
        if (f.node instanceof Map) return E.ISDIR;
        const k = this.readAt(f, iovs, n, f.pos);
        f.pos += k;
        this.dv.setUint32(nread, k, true);
        return E.SUCCESS;
    }
    fd_pread(fd, iovs, n, off, nread) {
        const f = this.fd(fd);
        if (!f || f.tty) return E.BADF;
        this.dv.setUint32(nread, this.readAt(f, iovs, n, Number(off)), true);
        return E.SUCCESS;
    }
    fd_write(fd, iovs, n, nwritten) {
        const f = this.fd(fd);
        if (!f) return E.BADF;
        if (f.tty) {
            let total = 0;
            for (const [p, len] of this.iovs(iovs, n)) {
                if (f.write) f.write(new TextDecoder().decode(this.u8.subarray(p, p + len)));
                total += len;
            }
            this.dv.setUint32(nwritten, total, true);
            return E.SUCCESS;
        }
        if (f.append) f.pos = f.node.size;
        const k = this.writeAt(f, iovs, n, f.pos);
        f.pos += k;
        this.dv.setUint32(nwritten, k, true);
        return E.SUCCESS;
    }
    fd_pwrite(fd, iovs, n, off, nwritten) {
        const f = this.fd(fd);
        if (!f || f.tty) return E.BADF;
        this.dv.setUint32(nwritten, this.writeAt(f, iovs, n, Number(off)), true);
        return E.SUCCESS;
    }
    fd_seek(fd, off, whence, out) {
        const f = this.fd(fd);
        if (!f) return E.BADF;
        if (f.tty) return E.SPIPE;
        const base = whence === 0 ? 0 : whence === 1 ? f.pos : f.node.size;
        const p = base + Number(off);
        if (p < 0) return E.INVAL;
        f.pos = p;
        this.dv.setBigUint64(out, BigInt(p), true);
        return E.SUCCESS;
    }
    fd_tell(fd, out) { const f = this.fd(fd); if (!f) return E.BADF; this.dv.setBigUint64(out, BigInt(f.pos || 0), true); return E.SUCCESS; }
    fd_readdir(fd, buf, len, cookie, used) {
        const f = this.fd(fd);
        if (!f || !(f.node instanceof Map)) return E.NOTDIR;
        const names = [".", "..", ...f.node.keys()];
        const u8 = this.u8, dv = this.dv;
        let o = 0;
        for (let i = Number(cookie); i < names.length && o < len; i++) {
            const name = new TextEncoder().encode(names[i]);
            const node = i < 2 ? f.node : f.node.get(names[i]);
            const ent = new Uint8Array(24 + name.length);
            const ev = new DataView(ent.buffer);
            ev.setBigUint64(0, BigInt(i + 1), true);
            ev.setBigUint64(8, 1n, true);
            ev.setUint32(16, name.length, true);
            ev.setUint8(20, node instanceof Map ? FT.DIR : FT.FILE);
            ent.set(name, 24);
            const k = Math.min(ent.length, len - o);
            u8.set(ent.subarray(0, k), buf + o);
            o += k;
        }
        dv.setUint32(used, o, true);
        return E.SUCCESS;
    }
    fd_renumber(from, to) {
        const f = this.fd(from);
        if (!f) return E.BADF;
        this.fds.set(to, f); this.fds.delete(from);
        return E.SUCCESS;
    }

    path(fd, ptr, len) {
        const f = this.fd(fd);
        if (!f || !(f.node instanceof Map)) return null;
        return join(f.path, this.str(ptr, len));
    }
    path_open(fd, _dirflags, ptr, len, oflags, _rb, _ri, fdflags, out) {
        const p = this.path(fd, ptr, len);
        if (p === null) return E.BADF;
        let node = this.fs.lookup(p);
        if (node && (oflags & O.EXCL) && (oflags & O.CREAT)) return E.EXIST;
        if (!node) {
            if (!(oflags & O.CREAT)) return E.NOENT;
            const [dir, name] = this.fs.parent(p);
            if (!dir) return E.NOENT;
            node = { data: new Uint8Array(0), size: 0 };
            dir.set(name, node);
        }
        if ((oflags & O.DIRECTORY) && !(node instanceof Map)) return E.NOTDIR;
        if ((oflags & O.TRUNC) && !(node instanceof Map)) { node.size = 0; }
        const nfd = this.nextFd++;
        this.fds.set(nfd, { node, path: p, pos: 0, append: !!(fdflags & FD_APPEND) });
        this.dv.setUint32(out, nfd, true);
        return E.SUCCESS;
    }
    path_filestat_get(fd, _flags, ptr, len, buf) {
        const p = this.path(fd, ptr, len);
        if (p === null) return E.BADF;
        const n = this.fs.lookup(p);
        if (!n) return E.NOENT;
        return this.stat(buf, n);
    }
    path_filestat_set_times() { return E.SUCCESS; }
    path_create_directory(fd, ptr, len) {
        const p = this.path(fd, ptr, len);
        if (p === null) return E.BADF;
        if (this.fs.lookup(p)) return E.EXIST;
        const [dir, name] = this.fs.parent(p);
        if (!dir) return E.NOENT;
        dir.set(name, new Map());
        return E.SUCCESS;
    }
    path_remove_directory(fd, ptr, len) {
        const p = this.path(fd, ptr, len);
        const [dir, name] = this.fs.parent(p);
        const n = dir && dir.get(name);
        if (!n) return E.NOENT;
        if (!(n instanceof Map)) return E.NOTDIR;
        if (n.size) return E.NOTEMPTY;
        dir.delete(name);
        return E.SUCCESS;
    }
    path_unlink_file(fd, ptr, len) {
        const p = this.path(fd, ptr, len);
        const [dir, name] = this.fs.parent(p);
        const n = dir && dir.get(name);
        if (!n) return E.NOENT;
        if (n instanceof Map) return E.ISDIR;
        dir.delete(name);
        return E.SUCCESS;
    }
    path_rename(fd, ptr, len, fd2, ptr2, len2) {
        const a = this.path(fd, ptr, len), b = this.path(fd2, ptr2, len2);
        const [da, na] = this.fs.parent(a), [db, nb] = this.fs.parent(b);
        const n = da && da.get(na);
        if (!n || !db) return E.NOENT;
        da.delete(na);
        db.set(nb, n);
        return E.SUCCESS;
    }
    path_readlink() { return E.INVAL; }
    path_symlink() { return E.NOTSUP; }
    path_link() { return E.NOTSUP; }
}
