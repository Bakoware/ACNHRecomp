"""Instructions first needed by SDK 20 (Animal Crossing: New Horizons 3.0.2): saturating shifts and absolute value,
signed long add across lanes, bfc, umnegl, and two hints that do nothing here (csdb, MTE's stg). Mixed into
emit.Emitter like ExtOps; the saturation helpers (nx::sat, nx::sat128) are in runtime/include/nx/cpu.h.

The saturation flag (FPSR.QC) is not modelled, as elsewhere in the recompiler."""
from capstone import arm64_const as A

from .emit import Unhandled, u


class Sdk20Ops:
    """Placed before ExtOps in the Emitter bases: i_pmull extends ExtOps' version."""
    # Speculation barrier: no speculation to fence off in recompiled code.
    def i_csdb(self, f, i, ops):
        return ";"

    # MTE tag stores: the Switch has no memory tagging, so the SDK's tagged-heap paths never get here with tags
    # that matter. The data is untouched by stg itself; only a writeback form would change the base register.
    def i_stg(self, f, i, ops):
        if len(ops) > 2 or i.writeback:
            raise Unhandled("stg with writeback")
        return ";"

    def i_bfc(self, f, i, ops):  # bfc wd, #lsb, #width: clear the field
        d = self.regop(i, ops[0])
        lsb, width = ops[1].imm, ops[2].imm
        keep = ~(((1 << width) - 1) << lsb) & ((1 << d.bits) - 1)
        return d.w(f"{d.r()} & {keep:#x}ull")

    def i_umnegl(self, f, i, ops):  # umnegl xd, wn, wm = -(zext(wn) * zext(wm))
        d = self.regop(i, ops[0])
        n, m = (self.regop(i, o).r() for o in ops[1:3])
        return d.w(f"uint64_t(0) - uint64_t(uint32_t({n})) * uint64_t(uint32_t({m}))")

    def i_smnegl(self, f, i, ops):
        d = self.regop(i, ops[0])
        n, m = (self.regop(i, o).r() for o in ops[1:3])
        return d.w(f"uint64_t(0 - int64_t(int32_t({n})) * int64_t(int32_t({m})))")

    def i_saddlv(self, f, i, ops):
        d, a = self.regop(i, ops[0]), self.regop(i, ops[1])
        bits, n = self.vinfo(ops[1])
        return (f"{{ int64_t s_ = 0; for (int k = 0; k < {n}; k++) s_ += c.v[{a.n}].s{bits}[k]; "
                f"nx::V128 r = nx::vzero(); r.s{bits * 2}[0] = int{bits * 2}_t(s_); c.v[{d.n}] = r; }}")

    def i_sqabs(self, f, i, ops):
        bits = self.vinfo(ops[0])[0]
        return self.v_un(i, ops, f"nx::sat<int{bits}_t>(a < 0 ? -int64_t(a) : int64_t(a))", signed=True)

    def i_sqneg(self, f, i, ops):
        bits = self.vinfo(ops[0])[0]
        return self.v_un(i, ops, f"nx::sat<int{bits}_t>(-int64_t(a))", signed=True)

    @staticmethod
    def _sq_shift(bits, a, sh, rounding):
        """Signed saturating shift of lane value `a` by the signed amount `sh` (negative: right shift, truncating
        or rounding). Shift counts are clamped so the 128-bit intermediate never overflows; any non-zero value
        shifted by >= bits saturates anyway."""
        left = f"nx::sat128<int{bits}_t>((__int128)int64_t({a}) << ({sh} > {bits} ? {bits} : {sh}))"
        if rounding:
            # (a + 2^(n-1)) >> n, in 128 bits; n up to bits (beyond: result 0 after rounding, as a >> 64 + 1 bit)
            right = (f"int{bits}_t(-{sh} > {bits} ? 0 : (((__int128)int64_t({a}) + ((__int128)1 << (-{sh} - 1))) >> -{sh}))")
        else:
            right = f"int{bits}_t(int64_t({a}) >> (-{sh} > {bits - 1} ? {bits - 1} : -{sh}))"
        return f"(({sh}) >= 0 ? {left} : {right})"

    def sq_shl(self, i, ops, rounding):
        bits, n = self.vinfo(ops[0])
        if ops[2].type == A.ARM64_OP_IMM:
            if rounding:
                raise Unhandled("sqrshl immediate")
            return self.v_un(i, ops[:2], f"nx::sat128<int{bits}_t>((__int128)int64_t(a) << {ops[2].imm})", signed=True)
        sh = f"int(int8_t(S1.u{bits}[k]))"
        return self.v_bin(i, ops, self._sq_shift(bits, "a", sh, rounding), signed=True)

    def i_sqshl(self, f, i, ops):
        return self.sq_shl(i, ops, False)

    def i_sqrshl(self, f, i, ops):
        return self.sq_shl(i, ops, True)

    def i_uqshl(self, f, i, ops):
        bits, n = self.vinfo(ops[0])
        mx = f"uint{bits}_t(~0ull)"
        if ops[2].type == A.ARM64_OP_IMM:
            sh = ops[2].imm
            return self.v_un(i, ops[:2], f"(uint64_t(a) >> ({bits} - {sh})) ? {mx} : uint{bits}_t(uint64_t(a) << {sh})"
                             if sh else "a")
        s = f"int(int8_t(S1.u{bits}[k]))"
        return self.v_bin(i, ops, f"{s} >= 0 ? ({s} >= {bits} ? (a ? {mx} : 0) : ((unsigned __int128)a << {s}) > {mx} ? {mx} : "
                                  f"uint{bits}_t(uint64_t(a) << {s})) : (-{s} >= {bits} ? 0 : uint64_t(a) >> -{s})")

    # Shift and insert (main: pixel packing). sli keeps the destination's low `n` bits, sri its high `n` bits.
    def i_sli(self, f, i, ops):
        bits, n = self.vinfo(ops[0])
        sh = ops[2].imm
        keep = (1 << sh) - 1
        return self.v_shift_imm(i, ops, f"(({{a}} << {{n}}) | (uint64_t({{o}}) & {keep:#x}ull))", acc=True)

    def i_sri(self, f, i, ops):
        bits, n = self.vinfo(ops[0])
        sh = ops[2].imm
        if sh >= bits:
            return ";"  # a shift by the lane size inserts nothing
        keep = ~(((1 << bits) - 1) >> sh) & ((1 << bits) - 1)
        return self.v_shift_imm(i, ops, f"(({{a}} >> {{n}}) | (uint64_t({{o}}) & {keep:#x}ull))", acc=True)

    # Polynomial (carry-less) multiply. 1Q (crypto) is ExtOps'; 8B -> 8H lanes here.
    def i_pmull(self, f, i, ops):
        if ops[0].vas == A.ARM64_VAS_1Q:
            return super().i_pmull(f, i, ops)
        d, a, b = (self.regop(i, o) for o in ops[:3])
        base = 8 if i.mnemonic == "pmull2" else 0
        return (f"{{ nx::V128 A_ = c.v[{a.n}], B_ = c.v[{b.n}]; nx::V128 r = nx::vzero(); for (int k = 0; k < 8; k++) {{ "
                f"uint16_t x_ = A_.u8[{base} + k], p_ = 0; uint8_t y_ = B_.u8[{base} + k]; "
                f"for (int j = 0; j < 8; j++) if (y_ >> j & 1) p_ ^= uint16_t(x_ << j); r.u16[k] = p_; }} c.v[{d.n}] = r; }}")

    i_pmull2 = i_pmull

    # Table lookup that keeps the destination lane for an out-of-range index (tbl writes 0 there).
    def i_tbx(self, f, i, ops):
        bits, n = self.vinfo(ops[0])
        d = self.regop(i, ops[0])
        tables = [self.regop(i, o) for o in ops[1:-1]]
        idx = self.regop(i, ops[-1])
        cp = " ".join(f"std::memcpy(t_ + {16 * k}, &c.v[{r.n}], 16);" for k, r in enumerate(tables))
        clear = "" if n == 16 else " r.u64[1] = 0;"
        return (f"{{ uint8_t t_[64]; {cp} nx::V128 I_ = c.v[{idx.n}]; nx::V128 r = c.v[{d.n}]; "
                f"for (int k = 0; k < {n}; k++) if (I_.u8[k] < {16 * len(tables)}) r.u8[k] = t_[I_.u8[k]];{clear} c.v[{d.n}] = r; }}")
