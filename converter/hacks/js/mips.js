// mips.js: the part of rabbitizer (MIT, (c) Decompollaborate) that discover.js
// needs, for the N64's CPU with rabbitizer's default settings (pseudo
// instructions on, `j` an unconditional branch, gnuMode). Decoding follows
// RabbitizerInstruction_ProcessUniqueId.c, validity getValidBits(); the
// instruction tables come from rabbitizer's own (mips_tables.js).
import { TABLES, DESCR } from "./mips_tables.js";

const field = (w, pos, len) => (w >>> pos) & ((1 << len) - 1);
const mask = (pos, len) => (((1 << len) - 1) << pos) >>> 0;

const OPERAND_BITS = {
    rs: mask(21, 5), rt: mask(16, 5), rd: mask(11, 5), sa: mask(6, 5),
    cop0d: mask(11, 5), fs: mask(11, 5), ft: mask(16, 5), fd: mask(6, 5), cop1cs: mask(11, 5),
    cop2t: mask(16, 5), cop2cd: mask(11, 5), op: mask(16, 5), hint: mask(16, 5),
    code: mask(6, 20), code_lower: mask(6, 10), copraw: mask(0, 25),
    label: mask(0, 26), immediate: mask(0, 16), branch_target_label: mask(0, 16),
    immediate_base: (mask(0, 16) | mask(21, 5)) >>> 0, maybe_rd_rs: (mask(11, 5) | mask(21, 5)) >>> 0,
};

// valid bits of each instruction's operands, once
const OPS_MASK = {};
for (const [id, d] of Object.entries(DESCR)) {
    let m = 0;
    for (const o of d.ops) {
        if (!(o in OPERAND_BITS)) throw new Error(`mips.js: operand ${o} (${id})`);
        m |= OPERAND_BITS[o];
    }
    OPS_MASK[id] = m >>> 0;
}

function lookup(table, key) {
    return TABLES[table][key] || null;
}

/** decode a word: {id, mandatory} as rabbitizer's processUniqueId leaves them */
function decode(w) {
    const opcode = field(w, 26, 6);
    let mand = mask(26, 6);
    let id = null;
    switch (opcode) {
        default: {
            id = lookup("normal", opcode);
            if (id === "beq" && field(w, 16, 5) === 0) id = field(w, 21, 5) === 0 ? "b" : "beqz";
            else if (id === "bne" && field(w, 16, 5) === 0) id = "bnez";
            break;
        }
        case 0x00: {
            mand |= mask(0, 6);
            id = lookup("special", field(w, 0, 6));
            if (w === 0) id = "nop";
            else if (id === "or" && field(w, 16, 5) === 0) id = "move";
            else if (id === "nor" && field(w, 16, 5) === 0) id = "not";
            else if (id === "sub" && field(w, 21, 5) === 0) id = "neg" in DESCR ? "neg" : id;
            else if (id === "subu" && field(w, 21, 5) === 0) id = "negu";
            break;
        }
        case 0x01: {
            mand |= mask(16, 5);
            id = lookup("regimm", field(w, 16, 5));
            if (id === "bgezal" && field(w, 21, 5) === 0) id = "bal";
            break;
        }
        case 0x10: {
            const fmt = field(w, 21, 5);
            mand |= mask(21, 5);
            id = lookup("cop0", fmt);
            if (fmt === 0x08) {
                mand |= mask(16, 5);
                id = lookup("cop0_bc0", field(w, 16, 5));
            } else if (fmt === 0x10) {
                mand |= mask(0, 6);
                id = lookup("cop0_tlb", field(w, 0, 6));
            }
            break;
        }
        case 0x11: {
            const fmt = field(w, 21, 5);
            mand |= mask(21, 5);
            id = lookup("cop1", fmt);
            const sub = { 0x08: "cop1_bc1", 0x10: "cop1_fpu_s", 0x11: "cop1_fpu_d", 0x14: "cop1_fpu_w", 0x15: "cop1_fpu_l" }[fmt];
            if (sub === "cop1_bc1") {
                mand |= mask(16, 5);
                id = lookup(sub, field(w, 16, 5));
            } else if (sub) {
                mand |= mask(0, 6);
                id = lookup(sub, field(w, 0, 6));
            }
            break;
        }
        case 0x12: {
            mand |= mask(21, 5);
            id = lookup("cop2", field(w, 21, 5));
            break;
        }
    }
    return { id, mand: mand >>> 0 };
}

const S16 = (x) => (x & 0x8000 ? x - 0x10000 : x);

/** one instruction at vram: the rabbitizer.Instruction methods discover.js uses */
export class Instruction {
    constructor(word, vram) {
        this.word = word >>> 0;
        this.vram = vram >>> 0;
        const { id, mand } = decode(this.word);
        this.id = id;
        this.d = id ? DESCR[id] : null;
        this.valid = !!id && ((~(mand | OPS_MASK[id]) & this.word) >>> 0) === 0;
        this.rs = field(this.word, 21, 5);
        this.rt = field(this.word, 16, 5);
    }
    isValid() { return this.valid; }
    getOpcodeName() { return this.d ? this.d.name || this.id : "INVALID"; }
    isBranch() { return !!(this.d && this.d.isBranch); }
    isJump() { return !!(this.d && this.d.isJump); }
    isJumpWithAddress() { return !!(this.d && this.d.isJumpWithAddress); }
    isJumptableJump() { return this.id === "jr" && this.rs !== 31; }
    isJrRa() { return this.id === "jr" && this.rs === 31; }
    getProcessedImmediate() {
        const imm = this.word & 0xFFFF;
        return this.d && this.d.isUnsigned ? imm : S16(imm);
    }
    getInstrIndexAsVram() {
        let v = ((this.word & 0x3FFFFFF) << 2) >>> 0;
        v = this.vram === 0 ? (v | 0x80000000) >>> 0 : (v | ((this.vram + 4) & 0xF0000000)) >>> 0;
        return v;
    }
    getBranchVramGeneric() {
        if (this.d && this.d.ops.includes("label")) return this.getInstrIndexAsVram();
        return (S16(this.word & 0xFFFF) * 4 + 4 + this.vram) >>> 0;
    }
}

export const REG = { a0: 4, a1: 5, a2: 6, ra: 31 };
