// threedsx.js: ELF -> .3dsx, as devkitPro's 3dsxtool (3dstools, src/3dsxtool.cpp)
// does it, and the SMDH (title + icon) that goes in it.

const R_ARM_ABS32 = 2, R_ARM_REL32 = 3, R_ARM_TARGET1 = 38, R_ARM_TARGET2 = 41, R_ARM_PREL31 = 42, R_ARM_TLS_IE32 = 107;
const SHT_SYMTAB = 2, SHT_RELA = 4, SHT_REL = 9, SHF_ALLOC = 2, PT_LOAD = 1, STB_WEAK = 2;

/** elf: Uint8Array (a copy is patched); smdh: Uint8Array or null. -> Uint8Array */
export function elfTo3dsx(elfIn, smdh) {
    const img = new Uint8Array(elfIn);
    const dv = new DataView(img.buffer, img.byteOffset, img.byteLength);
    const u32 = (o) => dv.getUint32(o, true), u16 = (o) => dv.getUint16(o, true);
    const fail = (m) => { throw new Error(`3dsx: ${m}`); };
    if (u32(0) !== 0x464C457F) fail("not an ELF file");
    if (u16(16) !== 2) fail("the ELF must be an executable");

    const shoff = u32(32), shnum = u16(48), phoff = u32(28), phnum = u16(44);
    const sect = (i) => {
        const o = shoff + i * 40;
        return { type: u32(o + 4), flags: u32(o + 8), addr: u32(o + 12), offset: u32(o + 16), size: u32(o + 20),
            link: u32(o + 24), info: u32(o + 28), entsize: u32(o + 36) };
    };

    let baseAddr = 1, topAddr = 0, hasBase = false;
    let code = null, rodata = null, data = null, codeSize = 0, rodataSize = 0, dataSize = 0, bssSize = 0;
    for (let i = 0; i < phnum; i++) {
        const o = phoff + i * 32;
        const type = u32(o), fileOff = u32(o + 4), memPos = u32(o + 8), fileSize = u32(o + 16), memSize = u32(o + 20), flags = u32(o + 24);
        if (type !== PT_LOAD || !memSize) continue;
        if (!hasBase) { hasBase = true; baseAddr = memPos; } else if (memPos !== topAddr) fail("non-contiguous segments");
        if (memSize & 3) fail("a segment is not word-aligned");
        if (flags !== 6 && memSize !== fileSize) fail("only the data segment can have a BSS");
        if (fileSize & 3) fail("the loadable part of a segment is not word-aligned");
        if (flags === 5) { if (code || rodata || data) fail("code segment order"); code = fileOff; codeSize = memSize; }
        else if (flags === 4) { if (rodata || data) fail("rodata segment order"); rodata = fileOff; rodataSize = memSize; }
        else if (flags === 6) { if (data) fail("too many data segments"); data = fileOff; dataSize = memSize; bssSize = memSize - fileSize; }
        else fail("invalid segment");
        topAddr = (memPos + ((memSize + 0xFFF) & ~0xFFF)) >>> 0;
    }
    if (topAddr - baseAddr >= 0x10000000) fail("the executable must not be bigger than 256 MiB");
    if (u32(24) !== baseAddr) fail("the entry point must be the start of the code");
    const rodataStart = baseAddr + ((codeSize + 0xFFF) & ~0xFFF);
    const dataStart = rodataStart + ((rodataSize + 0xFFF) & ~0xFFF);
    const words = (topAddr - baseAddr) / 4;
    const absMap = new Uint8Array(words), relMap = new Uint8Array(words);
    const setReloc = (addr, map) => { const k = (addr - baseAddr) / 4; if (k >= 0 && k < map.length) map[k] = 1; };
    const hasReloc = (addr) => { const k = (addr - baseAddr) / 4; return absMap[k] || relMap[k]; };

    let symOff = -1, symCount = 0, symNames = 0;
    for (let i = 0; i < shnum; i++) {
        const s = sect(i);
        if (s.type === SHT_SYMTAB) { symOff = s.offset; symCount = s.size / 16; symNames = sect(s.link).offset; }
    }
    if (symOff < 0) fail("the ELF has no symbol table");

    for (let i = 0; i < shnum; i++) {
        const s = sect(i);
        if (s.type === SHT_RELA) fail("RELA relocations are not supported");
        if (s.type !== SHT_REL) continue;
        const target = sect(s.info);
        if (!(target.flags & SHF_ALLOC)) continue;
        const vsect = target.addr, vsectEnd = vsect + target.size, symTab = sect(s.link).offset;
        const n = s.size / s.entsize;
        for (let r = 0; r < n; r++) {
            const ro = s.offset + r * 8;
            const srcAddr = u32(ro), info = u32(ro + 4), type = info & 0xFF, symIdx = info >>> 8;
            const so = symTab + symIdx * 16;
            let symAddr = u32(so + 4);
            const symBind = img[so + 12] >> 4;
            if (srcAddr & 3) fail("unaligned relocation");
            if (srcAddr < vsect || srcAddr >= vsectEnd) continue;
            if (hasReloc(srcAddr)) continue;
            const at = target.offset + srcAddr - vsect;
            let src = u32(at);
            if (type === R_ARM_ABS32 || type === R_ARM_TARGET1) {
                if (symBind === STB_WEAK && symAddr === 0) continue;
                if (src < baseAddr) fail(`relocation to an invalid address (${src.toString(16)})`);
                src = (src - baseAddr) >>> 0;
                setReloc(srcAddr, absMap);
            } else if (type === R_ARM_REL32 || type === R_ARM_TARGET2 || type === R_ARM_PREL31 || type === R_ARM_TLS_IE32) {
                let off = src | 0;
                if (type === R_ARM_PREL31) {
                    if (off & 0x80000000) continue;
                    if (off & 0x40000000) off |= 0x80000000;
                }
                off = (off - ((symAddr | 0) - (srcAddr | 0))) | 0;
                symAddr = (symAddr + off) >>> 0;
                if (symAddr < baseAddr || symAddr > topAddr) fail(`relative relocation to an invalid address (${symAddr.toString(16)})`);
                const inCode = (a) => a < rodataStart, inRo = (a) => a >= rodataStart && a < dataStart, inData = (a) => a >= dataStart && a <= topAddr;
                if ((inCode(symAddr) && !inCode(srcAddr)) || (inRo(symAddr) && !inRo(srcAddr)) || (inData(symAddr) && !inData(srcAddr))) {
                    src = (symAddr - baseAddr) >>> 0;
                    if (type === R_ARM_PREL31) src = (src | (1 << 28)) >>> 0;
                    setReloc(srcAddr, relMap);
                }
            } else continue;
            dv.setUint32(at, src, true);
        }
    }
    // interworking thunks
    for (let i = 0; i < symCount; i++) {
        const so = symOff + i * 16;
        let p = symNames + u32(so), name = "";
        while (img[p]) name += String.fromCharCode(img[p++]);
        if (!name) continue;
        if (name[0] !== "_" && name[1] !== "_") continue;
        if (!name.endsWith("_from_arm")) continue;
        setReloc(u32(so + 4) + 8, absMap);
    }

    const relocs = [];
    const counts = [];
    const build = (map, pos, posEnd) => {
        const start = relocs.length;
        pos = (pos - baseAddr) / 4; posEnd = (posEnd - baseAddr) / 4;
        for (let i = pos; i < posEnd;) {
            let rs = 0, rp = 0;
            while (i < posEnd && !map[i]) { i++; rs++; }
            while (i < posEnd && map[i]) { i++; rp++; }
            if (i === posEnd && rs && !rp) break;
            for (; rs > 0xFFFF; rs -= 0xFFFF) relocs.push([0xFFFF, 0]);
            for (; rp > 0xFFFF; rp -= 0xFFFF) { relocs.push([rs, 0xFFFF]); rs = 0; }
            if (rs || rp) relocs.push([rs, rp]);
        }
        counts.push(relocs.length - start);
    };
    build(absMap, baseAddr, rodataStart); build(relMap, baseAddr, rodataStart);
    build(absMap, rodataStart, dataStart); build(relMap, rodataStart, dataStart);
    build(absMap, dataStart, topAddr); build(relMap, dataStart, topAddr);

    const ext = !!smdh;
    const headSize = 8 * 4 + (ext ? 12 : 0);
    const segBytes = codeSize + rodataSize + (dataSize - bssSize);
    let size = headSize + 3 * 8 + segBytes + relocs.length * 4;
    const smdhPos = size;
    if (ext) size = (size + smdh.length + 3) & ~3;
    const out = new Uint8Array(size);
    const ov = new DataView(out.buffer);
    let p = 0;
    const w32 = (v) => { ov.setUint32(p, v >>> 0, true); p += 4; }, w16 = (v) => { ov.setUint16(p, v, true); p += 2; };
    w32(0x58534433); w16(headSize); w16(8); w32(0); w32(0);
    w32(codeSize); w32(rodataSize); w32(dataSize); w32(bssSize);
    if (ext) { w32(smdhPos); w32(smdh.length); w32(0); }
    for (let k = 0; k < 6; k++) w32(counts[k]);
    if (code !== null) { out.set(img.subarray(code, code + codeSize), p); p += codeSize; }
    if (rodata !== null) { out.set(img.subarray(rodata, rodata + rodataSize), p); p += rodataSize; }
    if (data !== null) { out.set(img.subarray(data, data + dataSize - bssSize), p); p += dataSize - bssSize; }
    for (const [s, n] of relocs) { w16(s); w16(n); }
    if (ext) out.set(smdh, smdhPos);
    return out;
}

/**
 * An SMDH with new titles: template is an SMDH (its icons and settings are
 * kept); shortName (<= 63 characters), longName (<= 127), publisher (<= 63).
 */
export function smdhWithTitles(template, shortName, longName, publisher) {
    const out = new Uint8Array(template);
    const put = (off, len, s) => {
        out.fill(0, off, off + len);
        for (let i = 0; i < s.length && (i + 1) * 2 < len; i++) {
            const c = s.charCodeAt(i);
            out[off + i * 2] = c & 0xFF; out[off + i * 2 + 1] = c >> 8;
        }
    };
    for (let lang = 0; lang < 16; lang++) {
        const t = 8 + lang * 0x200;
        put(t, 0x80, shortName); put(t + 0x80, 0x100, longName); put(t + 0x180, 0x80, publisher);
    }
    return out;
}
