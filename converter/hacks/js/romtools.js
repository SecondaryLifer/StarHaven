// romtools.js: N64 ROM chores for the converter: byte order, CRCs, and the
// patch formats ROM hacks ship in (BPS as tools/bps.py, and IPS).

let CRC_TABLE = null;
export function crc32(b) {
    if (!CRC_TABLE) {
        CRC_TABLE = new Uint32Array(256);
        for (let n = 0; n < 256; n++) {
            let c = n;
            for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1;
            CRC_TABLE[n] = c >>> 0;
        }
    }
    let c = 0xFFFFFFFF;
    for (let i = 0; i < b.length; i++) c = CRC_TABLE[(c ^ b[i]) & 0xFF] ^ (c >>> 8);
    return (c ^ 0xFFFFFFFF) >>> 0;
}

/** a ROM in any of the three byte orders -> .z64 (big-endian), or null if it isn't an N64 ROM */
export function toZ64(b) {
    if (b.length < 0x40) return null;
    const m = (b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3]) >>> 0;
    const out = new Uint8Array(b.length);
    if (m === 0x80371240) out.set(b);                                       // .z64
    else if (m === 0x37804012) for (let i = 0; i + 1 < b.length; i += 2) { out[i] = b[i + 1]; out[i + 1] = b[i]; }   // .v64
    else if (m === 0x40123780) for (let i = 0; i + 3 < b.length; i += 4) {  // .n64
        out[i] = b[i + 3]; out[i + 1] = b[i + 2]; out[i + 2] = b[i + 1]; out[i + 3] = b[i];
    } else return null;
    return out;
}

/** the header's CRC1 (0x10) and internal name */
export function header(z64) {
    const crc1 = (z64[0x10] << 24 | z64[0x11] << 16 | z64[0x12] << 8 | z64[0x13]) >>> 0;
    const title = String.fromCharCode(...z64.subarray(0x20, 0x34)).replace(/[\0 ]+$/, "");
    const id = String.fromCharCode(...z64.subarray(0x3B, 0x3F));
    return { crc1, title, id };
}

function bpsNum(p, st) {
    let v = 0, s = 1;
    for (;;) {
        const b = p[st.i++];
        v += (b & 0x7F) * s;
        if (b & 0x80) return v;
        s *= 128;
        v += s;
    }
}

/** BPS: the target ROM. Throws with a readable reason when it can't be applied. */
export function applyBps(src, patch) {
    if (String.fromCharCode(...patch.subarray(0, 4)) !== "BPS1") throw new Error("not a BPS patch");
    const dv = new DataView(patch.buffer, patch.byteOffset, patch.byteLength);
    const wantSrc = dv.getUint32(patch.length - 12, true), wantDst = dv.getUint32(patch.length - 8, true),
        wantPatch = dv.getUint32(patch.length - 4, true);
    if (crc32(patch.subarray(0, patch.length - 4)) !== wantPatch) throw new Error("the patch file is damaged");
    const have = crc32(src);
    if (have !== wantSrc) throw new Error(`this patch is for another ROM (it needs CRC32 ${hex(wantSrc)}, yours is ${hex(have)})`);
    const st = { i: 4 };
    bpsNum(patch, st);
    const dstSize = bpsNum(patch, st);
    const metaSize = bpsNum(patch, st);
    st.i += metaSize;
    const out = new Uint8Array(dstSize);
    let o = 0, srcRel = 0, dstRel = 0;
    const end = patch.length - 12;
    while (st.i < end) {
        const d = bpsNum(patch, st);
        const cmd = d % 4, n = Math.floor(d / 4) + 1;
        if (cmd === 0) out.set(src.subarray(o, o + n), o);
        else if (cmd === 1) { out.set(patch.subarray(st.i, st.i + n), o); st.i += n; }
        else {
            const raw = bpsNum(patch, st);
            const off = (raw % 2 ? -1 : 1) * Math.floor(raw / 2);
            if (cmd === 2) { srcRel += off; out.set(src.subarray(srcRel, srcRel + n), o); srcRel += n; }
            else { dstRel += off; for (let k = 0; k < n; k++) out[o + k] = out[dstRel + k]; dstRel += n; }
        }
        o += n;
    }
    if (crc32(out) !== wantDst) throw new Error("the patched ROM came out wrong (CRC32 mismatch)");
    return out;
}

/** IPS (and IPS32 lacks here: N64 ROMs past 16 MB need BPS) */
export function applyIps(src, patch) {
    if (String.fromCharCode(...patch.subarray(0, 5)) !== "PATCH") throw new Error("not an IPS patch");
    let out = new Uint8Array(src);
    let i = 5;
    const grow = (n) => { if (n > out.length) { const b = new Uint8Array(n); b.set(out); out = b; } };
    while (i + 3 <= patch.length) {
        const off = patch[i] << 16 | patch[i + 1] << 8 | patch[i + 2];
        if (off === 0x454F46 && i + 3 >= patch.length - 3) {     // "EOF" (+ optional truncation size)
            if (patch.length - (i + 3) === 3) out = out.subarray(0, patch[i + 3] << 16 | patch[i + 4] << 8 | patch[i + 5]);
            return out;
        }
        const size = patch[i + 3] << 8 | patch[i + 4];
        i += 5;
        if (size) {
            grow(off + size);
            out.set(patch.subarray(i, i + size), off);
            i += size;
        } else {                                                  // RLE
            const n = patch[i] << 8 | patch[i + 1], v = patch[i + 2];
            i += 3;
            grow(off + n);
            out.fill(v, off, off + n);
        }
    }
    throw new Error("the IPS patch ends early");
}

const hex = (v) => v.toString(16).toUpperCase().padStart(8, "0");
