// sha1.js: SHA-1 of a string's UTF-8 bytes, as hex (matches Python's
// hashlib.sha1(s.encode()).hexdigest()). Synchronous, for many small texts.

const enc = new TextEncoder();

export function sha1Hex(str) {
    const msg = enc.encode(str);
    const ml = msg.length;
    const nBlocks = ((ml + 8) >> 6) + 1;
    const w = new Uint32Array(nBlocks * 16);
    for (let i = 0; i < ml; i++) w[i >> 2] |= msg[i] << (24 - (i & 3) * 8);
    w[ml >> 2] |= 0x80 << (24 - (ml & 3) * 8);
    w[nBlocks * 16 - 1] = (ml * 8) >>> 0;
    w[nBlocks * 16 - 2] = Math.floor(ml / 0x20000000);
    let h0 = 0x67452301, h1 = 0xEFCDAB89, h2 = 0x98BADCFE, h3 = 0x10325476, h4 = 0xC3D2E1F0;
    const x = new Uint32Array(80);
    for (let b = 0; b < nBlocks; b++) {
        for (let t = 0; t < 16; t++) x[t] = w[b * 16 + t];
        for (let t = 16; t < 80; t++) { const v = x[t - 3] ^ x[t - 8] ^ x[t - 14] ^ x[t - 16]; x[t] = (v << 1) | (v >>> 31); }
        let a = h0, bb = h1, c = h2, d = h3, e = h4;
        for (let t = 0; t < 80; t++) {
            let f, k;
            if (t < 20) { f = (bb & c) | (~bb & d); k = 0x5A827999; }
            else if (t < 40) { f = bb ^ c ^ d; k = 0x6ED9EBA1; }
            else if (t < 60) { f = (bb & c) | (bb & d) | (c & d); k = 0x8F1BBCDC; }
            else { f = bb ^ c ^ d; k = 0xCA62C1D6; }
            const tmp = (((a << 5) | (a >>> 27)) + f + e + k + x[t]) >>> 0;
            e = d; d = c; c = (bb << 30) | (bb >>> 2); bb = a; a = tmp;
        }
        h0 = (h0 + a) >>> 0; h1 = (h1 + bb) >>> 0; h2 = (h2 + c) >>> 0; h3 = (h3 + d) >>> 0; h4 = (h4 + e) >>> 0;
    }
    return [h0, h1, h2, h3, h4].map((v) => v.toString(16).padStart(8, "0")).join("");
}
