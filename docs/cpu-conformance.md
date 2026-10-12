# CPU conformance audit

Base: `ea3244c9cb`. Track: `team/t19-cpu-conformance`.

## What is measured

Every ordered `INST` row in the six decoder tables is enumerated below. Aliases and reserved encodings count as rows, not distinct instructions. `translated` means a C++ visitor body exists and is not an unconditional stub; it does **not** prove architectural correctness. Guard flags include expected checks for invalid operands and architectural exceptions. This static audit does not inspect backend lowering or recursively prove helper semantics.

## Before / after

| Table | Rows | Translated before | Translated after | Architectural undefined | Remaining stubs |
|---|---:|---:|---:|---:|---:|
| a64 | 643 | 634 | 634 | 9 | 0 |
| arm | 261 | 237 | 254 | 1 | 6 |
| asimd | 147 | 137 | 137 | 10 | 0 |
| vfp | 62 | 61 | 61 | 1 | 0 |
| thumb16 | 79 | 78 | 78 | 1 | 0 |
| thumb32 | 239 | 239 | 239 | 0 | 0 |

17 decoder rows (8 instruction families, 16 operand forms) now translate A32 LDRBT/LDRHT/LDRSBT/LDRSHT/LDRT/STRBT/STRHT/STRT instead of raising undefined. They perform a post-indexed UNPRIV access, preserve condition execution, extend loads correctly, and write back the base after the transfer. PC and base/destination overlap remain unpredictable. The prior general fuzz exclusion is removed.

The six remaining unconditional unreachable handlers are privileged RFE/SRS/CPS and banked-register LDM/STM forms. They are outside the user-mode execution target; this audit does not claim their privileged semantics are implemented. Architectural UDF and reserved SIMD encodings intentionally raise exceptions. There are zero direct InterpretThisInstruction/NOT_IMPLEMENTED/UNIMPLEMENTED markers in decoder handlers.

## Differential evidence

The targeted Catch2 `[conformance]` test runs 4,096 deterministic randomized cases against the existing Unicorn A32 reference harness (16 forms × 256 states). It compares registers, CPSR, extended registers, FPSCR and written memory through RunTestInstance. Cases cover add/subtract, all executable condition codes, random NZCV/carry, all register shift kinds including zero shifts/RRX, sign extension and post-indexed writeback. Existing full A64/A32/Thumb randomized suites also run in CI with Unicorn enabled. A passing source audit cannot replace these executable tests.

## Scope limits and priorities

No real-game instruction histogram was available in this worktree. Priority therefore follows ISA categories: valid user-mode memory instructions first. LSE (CAS/CASP, SWP, LDADD/LDCLR/LDEOR/LDSET and min/max families) has **no decoder rows** here and is not covered by the row denominator; adding concurrency-correct backend atomics requires a separate measured change. Missing ISA extensions cannot be inferred from existing tables. The tables include AdvSIMD/FP, AES/SHA/CRC and later crypto/flag extensions, but this is not a claim of complete ARMv8 coverage. A full ISA inventory, backend differential sweep, FP exception/NaN audit and memory-ordering litmus tests remain necessary for the stated all-instruction goal.

Rosetta can execute x86-64 on this Mac, but local heavy compilation was avoided after measuring swap at 16.7/17.4 GB and only 8.9 GB disk free. The dedicated workflow builds and executes the real dynarmic suite on Windows and Ubuntu x64, outside the graphics renderer work.

## Console follow-up

Run legal owner-provided 32-bit software exercising the unprivileged transfer forms on Xbox UWP; verify callback memory accesses and W^X JIT execution, then collect an anonymized instruction histogram (opcode counts only, no game/firmware bytes or keys). Also compare FP/crypto/CRC outputs with real Switch hardware and validate exclusive-monitor ordering under contention. Windows desktop Unicorn comparisons do not establish Xbox AppContainer behavior.

Instruction behavior reference: [Arm Architecture Reference Manual](https://documentation-service.arm.com/static/67e40a9c4191d739c8e9bd2c). Reference interpreter: [Unicorn 2.1.4](https://github.com/unicorn-engine/unicorn/releases/tag/2.1.4).

## Complete ordered decoder inventory

`guards` lists direct exceptional/validation calls, including legal architectural traps. An empty guard column does not establish that every operand encoding is legal.

### a64

| Row | Handler | Name | Encoding | Status | Guards | Source |
|---:|---|---|---|---|---|---|
| 1 | `FMOV_3` | FMOV (vector, immediate) | `0Q00111100000abc111111defghddddd` | translated |  | `simd_modified_immediate.cpp` |
| 2 | `FMOV_2` | FMOV (vector, immediate) | `0Qo0111100000abc111101defghddddd` | translated | UnallocatedEncoding | `simd_modified_immediate.cpp` |
| 3 | `MOVI` | MOVI, MVNI, ORR, BIC (vector, immediate) | `0Qo0111100000abcmmmm01defghddddd` | translated | UNREACHABLE, UnallocatedEncoding | `simd_modified_immediate.cpp` |
| 4 | `UnallocatedEncoding` | Unallocated SIMD modified immediate | `0--0111100000-------11----------` | architectural undefined | RaiseException | `impl.cpp` |
| 5 | `NOP` | NOP | `11010101000000110010000000011111` | translated |  | `system.cpp` |
| 6 | `YIELD` | YIELD | `11010101000000110010000000111111` | translated | RaiseException | `system.cpp` |
| 7 | `WFE` | WFE | `11010101000000110010000001011111` | translated | RaiseException | `system.cpp` |
| 8 | `WFI` | WFI | `11010101000000110010000001111111` | translated | RaiseException | `system.cpp` |
| 9 | `SEV` | SEV | `11010101000000110010000010011111` | translated | RaiseException | `system.cpp` |
| 10 | `SEVL` | SEVL | `11010101000000110010000010111111` | translated | RaiseException | `system.cpp` |
| 11 | `CFINV` | CFINV | `11010101000000000100000000011111` | translated |  | `system_flag_manipulation.cpp` |
| 12 | `XAFlag` | XAFlag | `11010101000000000100000000111111` | translated |  | `system_flag_format.cpp` |
| 13 | `AXFlag` | AXFlag | `11010101000000000100000001011111` | translated |  | `system_flag_format.cpp` |
| 14 | `IC_IALLU` | IC IALLU | `11010101000010000111010100011111` | translated |  | `sys_ic.cpp` |
| 15 | `IC_IALLUIS` | IC IALLUIS | `11010101000010000111000100011111` | translated |  | `sys_ic.cpp` |
| 16 | `CLREX` | CLREX | `11010101000000110011MMMM01011111` | translated |  | `system.cpp` |
| 17 | `DSB` | DSB | `11010101000000110011MMMM10011111` | translated |  | `system.cpp` |
| 18 | `DMB` | DMB | `11010101000000110011MMMM10111111` | translated |  | `system.cpp` |
| 19 | `ISB` | ISB | `11010101000000110011MMMM11011111` | translated |  | `system.cpp` |
| 20 | `DC_IVAC` | DC IVAC | `110101010000100001110110001ttttt` | translated |  | `sys_dc.cpp` |
| 21 | `DC_ISW` | DC ISW | `110101010000100001110110010ttttt` | translated |  | `sys_dc.cpp` |
| 22 | `DC_CSW` | DC CSW | `110101010000100001111010010ttttt` | translated |  | `sys_dc.cpp` |
| 23 | `DC_CISW` | DC CISW | `110101010000100001111110010ttttt` | translated |  | `sys_dc.cpp` |
| 24 | `DC_ZVA` | DC ZVA | `110101010000101101110100001ttttt` | translated |  | `sys_dc.cpp` |
| 25 | `DC_CVAC` | DC CVAC | `110101010000101101111010001ttttt` | translated |  | `sys_dc.cpp` |
| 26 | `DC_CVAU` | DC CVAU | `110101010000101101111011001ttttt` | translated |  | `sys_dc.cpp` |
| 27 | `DC_CVAP` | DC CVAP | `110101010000101101111100001ttttt` | translated |  | `sys_dc.cpp` |
| 28 | `DC_CIVAC` | DC CIVAC | `110101010000101101111110001ttttt` | translated |  | `sys_dc.cpp` |
| 29 | `IC_IVAU` | IC IVAU | `110101010000101101110101001ttttt` | translated |  | `sys_ic.cpp` |
| 30 | `BLR` | BLR | `1101011000111111000000nnnnn00000` | translated |  | `a64_branch.cpp` |
| 31 | `BR` | BR | `1101011000011111000000nnnnn00000` | translated |  | `a64_branch.cpp` |
| 32 | `RET` | RET | `1101011001011111000000nnnnn00000` | translated |  | `a64_branch.cpp` |
| 33 | `HINT` | HINT | `11010101000000110010MMMMooo11111` | translated |  | `system.cpp` |
| 34 | `SXTB_1` | SXTB (32-bit) | `0001001100000000000111nnnnnddddd` | translated |  | `data_processing_bitfield.cpp` |
| 35 | `SXTB_2` | SXTB (64-bit) | `1001001101000000000111nnnnnddddd` | translated |  | `data_processing_bitfield.cpp` |
| 36 | `SXTH_1` | SXTH (32-bit) | `0001001100000000001111nnnnnddddd` | translated |  | `data_processing_bitfield.cpp` |
| 37 | `SXTH_2` | SXTH (64-bit) | `1001001101000000001111nnnnnddddd` | translated |  | `data_processing_bitfield.cpp` |
| 38 | `SXTW` | SXTW | `1001001101000000011111nnnnnddddd` | translated |  | `data_processing_bitfield.cpp` |
| 39 | `REV32_int` | REV32 | `1101101011000000000010nnnnnddddd` | translated |  | `data_processing_register.cpp` |
| 40 | `AESE` | AESE | `0100111000101000010010nnnnnddddd` | translated |  | `simd_aes.cpp` |
| 41 | `AESD` | AESD | `0100111000101000010110nnnnnddddd` | translated |  | `simd_aes.cpp` |
| 42 | `AESMC` | AESMC | `0100111000101000011010nnnnnddddd` | translated |  | `simd_aes.cpp` |
| 43 | `AESIMC` | AESIMC | `0100111000101000011110nnnnnddddd` | translated |  | `simd_aes.cpp` |
| 44 | `SHA1H` | SHA1H | `0101111000101000000010nnnnnddddd` | translated |  | `simd_sha.cpp` |
| 45 | `SHA1SU1` | SHA1SU1 | `0101111000101000000110nnnnnddddd` | translated |  | `simd_sha.cpp` |
| 46 | `SHA256SU0` | SHA256SU0 | `0101111000101000001010nnnnnddddd` | translated |  | `simd_sha.cpp` |
| 47 | `FCMEQ_zero_1` | FCMEQ (zero) | `0101111011111000110110nnnnnddddd` | translated |  | `simd_scalar_two_register_misc.cpp` |
| 48 | `FRECPE_1` | FRECPE | `0101111011111001110110nnnnnddddd` | translated |  | `simd_scalar_two_register_misc.cpp` |
| 49 | `FRECPX_1` | FRECPX | `0101111011111001111110nnnnnddddd` | translated |  | `simd_scalar_two_register_misc.cpp` |
| 50 | `FRSQRTE_1` | FRSQRTE | `0111111011111001110110nnnnnddddd` | translated |  | `simd_scalar_two_register_misc.cpp` |
| 51 | `SHA512SU0` | SHA512SU0 | `1100111011000000100000nnnnnddddd` | translated |  | `simd_sha512.cpp` |
| 52 | `SM4E` | SM4E | `1100111011000000100001nnnnnddddd` | translated |  | `simd_sha512.cpp` |
| 53 | `RBIT_int` | RBIT | `z101101011000000000000nnnnnddddd` | translated |  | `data_processing_register.cpp` |
| 54 | `REV16_int` | REV16 | `z101101011000000000001nnnnnddddd` | translated |  | `data_processing_register.cpp` |
| 55 | `CLZ_int` | CLZ | `z101101011000000000100nnnnnddddd` | translated |  | `data_processing_register.cpp` |
| 56 | `CLS_int` | CLS | `z101101011000000000101nnnnnddddd` | translated |  | `data_processing_register.cpp` |
| 57 | `FCVTNS_2` | FCVTNS (vector) | `010111100z100001101010nnnnnddddd` | translated |  | `simd_scalar_two_register_misc.cpp` |
| 58 | `FCVTMS_2` | FCVTMS (vector) | `010111100z100001101110nnnnnddddd` | translated |  | `simd_scalar_two_register_misc.cpp` |
| 59 | `FCVTAS_2` | FCVTAS (vector) | `010111100z100001110010nnnnnddddd` | translated |  | `simd_scalar_two_register_misc.cpp` |
| 60 | `SCVTF_int_2` | SCVTF (vector, integer) | `010111100z100001110110nnnnnddddd` | translated |  | `simd_scalar_two_register_misc.cpp` |
| 61 | `FCMGT_zero_2` | FCMGT (zero) | `010111101z100000110010nnnnnddddd` | translated |  | `simd_scalar_two_register_misc.cpp` |
| 62 | `FCMEQ_zero_2` | FCMEQ (zero) | `010111101z100000110110nnnnnddddd` | translated |  | `simd_scalar_two_register_misc.cpp` |
| 63 | `FCMLT_2` | FCMLT (zero) | `010111101z100000111010nnnnnddddd` | translated |  | `simd_scalar_two_register_misc.cpp` |
| 64 | `FCVTPS_2` | FCVTPS (vector) | `010111101z100001101010nnnnnddddd` | translated |  | `simd_scalar_two_register_misc.cpp` |
| 65 | `FCVTZS_int_2` | FCVTZS (vector, integer) | `010111101z100001101110nnnnnddddd` | translated |  | `simd_scalar_two_register_misc.cpp` |
| 66 | `FRECPE_2` | FRECPE | `010111101z100001110110nnnnnddddd` | translated |  | `simd_scalar_two_register_misc.cpp` |
| 67 | `FRECPX_2` | FRECPX | `010111101z100001111110nnnnnddddd` | translated |  | `simd_scalar_two_register_misc.cpp` |
| 68 | `FCVTNU_2` | FCVTNU (vector) | `011111100z100001101010nnnnnddddd` | translated |  | `simd_scalar_two_register_misc.cpp` |
| 69 | `FCVTMU_2` | FCVTMU (vector) | `011111100z100001101110nnnnnddddd` | translated |  | `simd_scalar_two_register_misc.cpp` |
| 70 | `FCVTAU_2` | FCVTAU (vector) | `011111100z100001110010nnnnnddddd` | translated |  | `simd_scalar_two_register_misc.cpp` |
| 71 | `UCVTF_int_2` | UCVTF (vector, integer) | `011111100z100001110110nnnnnddddd` | translated |  | `simd_scalar_two_register_misc.cpp` |
| 72 | `FCMGE_zero_2` | FCMGE (zero) | `011111101z100000110010nnnnnddddd` | translated |  | `simd_scalar_two_register_misc.cpp` |
| 73 | `FCMLE_2` | FCMLE (zero) | `011111101z100000110110nnnnnddddd` | translated |  | `simd_scalar_two_register_misc.cpp` |
| 74 | `FCVTPU_2` | FCVTPU (vector) | `011111101z100001101010nnnnnddddd` | translated |  | `simd_scalar_two_register_misc.cpp` |
| 75 | `FCVTZU_int_2` | FCVTZU (vector, integer) | `011111101z100001101110nnnnnddddd` | translated |  | `simd_scalar_two_register_misc.cpp` |
| 76 | `FRSQRTE_2` | FRSQRTE | `011111101z100001110110nnnnnddddd` | translated |  | `simd_scalar_two_register_misc.cpp` |
| 77 | `FCVTXN_1` | FCVTXN, FCVTXN2 | `011111100z100001011010nnnnnddddd` | translated | ReservedValue | `simd_scalar_two_register_misc.cpp` |
| 78 | `FMAXNMP_pair_2` | FMAXNMP (scalar) | `011111100z110000110010nnnnnddddd` | translated |  | `simd_scalar_pairwise.cpp` |
| 79 | `FADDP_pair_2` | FADDP (scalar) | `011111100z110000110110nnnnnddddd` | translated |  | `simd_scalar_pairwise.cpp` |
| 80 | `FMAXP_pair_2` | FMAXP (scalar) | `011111100z110000111110nnnnnddddd` | translated |  | `simd_scalar_pairwise.cpp` |
| 81 | `FMINNMP_pair_2` | FMINNMP (scalar) | `011111101z110000110010nnnnnddddd` | translated |  | `simd_scalar_pairwise.cpp` |
| 82 | `FMINP_pair_2` | FMINP (scalar) | `011111101z110000111110nnnnnddddd` | translated |  | `simd_scalar_pairwise.cpp` |
| 83 | `FRINTN_1` | FRINTN (vector) | `0Q00111001111001100010nnnnnddddd` | translated |  | `simd_two_register_misc.cpp` |
| 84 | `FRINTM_1` | FRINTM (vector) | `0Q00111001111001100110nnnnnddddd` | translated |  | `simd_two_register_misc.cpp` |
| 85 | `FCMEQ_zero_3` | FCMEQ (zero) | `0Q00111011111000110110nnnnnddddd` | translated |  | `simd_two_register_misc.cpp` |
| 86 | `FABS_1` | FABS (vector) | `0Q00111011111000111110nnnnnddddd` | translated |  | `simd_two_register_misc.cpp` |
| 87 | `FRINTP_1` | FRINTP (vector) | `0Q00111011111001100010nnnnnddddd` | translated |  | `simd_two_register_misc.cpp` |
| 88 | `FRINTZ_1` | FRINTZ (vector) | `0Q00111011111001100110nnnnnddddd` | translated |  | `simd_two_register_misc.cpp` |
| 89 | `FRECPE_3` | FRECPE | `0Q00111011111001110110nnnnnddddd` | translated |  | `simd_two_register_misc.cpp` |
| 90 | `FRINTA_1` | FRINTA (vector) | `0Q10111001111001100010nnnnnddddd` | translated |  | `simd_two_register_misc.cpp` |
| 91 | `FRINTX_1` | FRINTX (vector) | `0Q10111001111001100110nnnnnddddd` | translated |  | `simd_two_register_misc.cpp` |
| 92 | `NOT` | NOT | `0Q10111000100000010110nnnnnddddd` | translated |  | `simd_two_register_misc.cpp` |
| 93 | `RBIT_asimd` | RBIT (vector) | `0Q10111001100000010110nnnnnddddd` | translated |  | `simd_two_register_misc.cpp` |
| 94 | `FNEG_1` | FNEG (vector) | `0Q10111011111000111110nnnnnddddd` | translated |  | `simd_two_register_misc.cpp` |
| 95 | `FRINTI_1` | FRINTI (vector) | `0Q10111011111001100110nnnnnddddd` | translated |  | `simd_two_register_misc.cpp` |
| 96 | `FRSQRTE_3` | FRSQRTE | `0Q10111011111001110110nnnnnddddd` | translated |  | `simd_two_register_misc.cpp` |
| 97 | `LDXR` | LDXRB, LDXRH, LDXR | `zz00100001011111011111nnnnnttttt` | translated |  | `load_store_exclusive.cpp` |
| 98 | `LDAXR` | LDAXRB, LDAXRH, LDAXR | `zz00100001011111111111nnnnnttttt` | translated |  | `load_store_exclusive.cpp` |
| 99 | `STLLR` | STLLRB, STLLRH, STLLR | `zz00100010011111011111nnnnnttttt` | translated |  | `load_store_exclusive.cpp` |
| 100 | `STLR` | STLRB, STLRH, STLR | `zz00100010011111111111nnnnnttttt` | translated |  | `load_store_exclusive.cpp` |
| 101 | `LDLAR` | LDLARB, LDLARH, LDLAR | `zz00100011011111011111nnnnnttttt` | translated |  | `load_store_exclusive.cpp` |
| 102 | `LDAR` | LDARB, LDARH, LDAR | `zz00100011011111111111nnnnnttttt` | translated |  | `load_store_exclusive.cpp` |
| 103 | `REV` | REV | `z10110101100000000001onnnnnddddd` | translated | UnallocatedEncoding | `data_processing_register.cpp` |
| 104 | `SUQADD_1` | SUQADD | `01011110zz100000001110nnnnnddddd` | translated |  | `simd_scalar_two_register_misc.cpp` |
| 105 | `SQABS_1` | SQABS | `01011110zz100000011110nnnnnddddd` | translated |  | `simd_scalar_two_register_misc.cpp` |
| 106 | `CMGT_zero_1` | CMGT (zero) | `01011110zz100000100010nnnnnddddd` | translated |  | `simd_scalar_three_same.cpp` |
| 107 | `CMEQ_zero_1` | CMEQ (zero) | `01011110zz100000100110nnnnnddddd` | translated |  | `simd_scalar_three_same.cpp` |
| 108 | `CMLT_1` | CMLT (zero) | `01011110zz100000101010nnnnnddddd` | translated |  | `simd_scalar_three_same.cpp` |
| 109 | `ABS_1` | ABS | `01011110zz100000101110nnnnnddddd` | translated | ReservedValue | `simd_scalar_two_register_misc.cpp` |
| 110 | `SQXTN_1` | SQXTN, SQXTN2 | `01011110zz100001010010nnnnnddddd` | translated |  | `simd_scalar_two_register_misc.cpp` |
| 111 | `USQADD_1` | USQADD | `01111110zz100000001110nnnnnddddd` | translated |  | `simd_scalar_two_register_misc.cpp` |
| 112 | `SQNEG_1` | SQNEG | `01111110zz100000011110nnnnnddddd` | translated |  | `simd_scalar_two_register_misc.cpp` |
| 113 | `CMGE_zero_1` | CMGE (zero) | `01111110zz100000100010nnnnnddddd` | translated |  | `simd_scalar_three_same.cpp` |
| 114 | `CMLE_1` | CMLE (zero) | `01111110zz100000100110nnnnnddddd` | translated |  | `simd_scalar_three_same.cpp` |
| 115 | `NEG_1` | NEG (vector) | `01111110zz100000101110nnnnnddddd` | translated | ReservedValue | `simd_scalar_two_register_misc.cpp` |
| 116 | `SQXTUN_1` | SQXTUN, SQXTUN2 | `01111110zz100001001010nnnnnddddd` | translated |  | `simd_scalar_two_register_misc.cpp` |
| 117 | `UQXTN_1` | UQXTN, UQXTN2 | `01111110zz100001010010nnnnnddddd` | translated |  | `simd_scalar_two_register_misc.cpp` |
| 118 | `ADDP_pair` | ADDP (scalar) | `01011110zz110001101110nnnnnddddd` | translated | ReservedValue | `simd_scalar_pairwise.cpp` |
| 119 | `FCVTN` | FCVTN, FCVTN2 | `0Q0011100z100001011010nnnnnddddd` | translated |  | `simd_two_register_misc.cpp` |
| 120 | `FCVTL` | FCVTL, FCVTL2 | `0Q0011100z100001011110nnnnnddddd` | translated |  | `simd_two_register_misc.cpp` |
| 121 | `FRINTN_2` | FRINTN (vector) | `0Q0011100z100001100010nnnnnddddd` | translated |  | `simd_two_register_misc.cpp` |
| 122 | `FRINTM_2` | FRINTM (vector) | `0Q0011100z100001100110nnnnnddddd` | translated |  | `simd_two_register_misc.cpp` |
| 123 | `FCVTNS_4` | FCVTNS (vector) | `0Q0011100z100001101010nnnnnddddd` | translated |  | `simd_two_register_misc.cpp` |
| 124 | `FCVTMS_4` | FCVTMS (vector) | `0Q0011100z100001101110nnnnnddddd` | translated |  | `simd_two_register_misc.cpp` |
| 125 | `FCVTAS_4` | FCVTAS (vector) | `0Q0011100z100001110010nnnnnddddd` | translated |  | `simd_two_register_misc.cpp` |
| 126 | `SCVTF_int_4` | SCVTF (vector, integer) | `0Q0011100z100001110110nnnnnddddd` | translated |  | `simd_two_register_misc.cpp` |
| 127 | `FCMGT_zero_4` | FCMGT (zero) | `0Q0011101z100000110010nnnnnddddd` | translated |  | `simd_two_register_misc.cpp` |
| 128 | `FCMEQ_zero_4` | FCMEQ (zero) | `0Q0011101z100000110110nnnnnddddd` | translated |  | `simd_two_register_misc.cpp` |
| 129 | `FCMLT_4` | FCMLT (zero) | `0Q0011101z100000111010nnnnnddddd` | translated |  | `simd_two_register_misc.cpp` |
| 130 | `FABS_2` | FABS (vector) | `0Q0011101z100000111110nnnnnddddd` | translated | ReservedValue | `simd_two_register_misc.cpp` |
| 131 | `FRINTP_2` | FRINTP (vector) | `0Q0011101z100001100010nnnnnddddd` | translated |  | `simd_two_register_misc.cpp` |
| 132 | `FRINTZ_2` | FRINTZ (vector) | `0Q0011101z100001100110nnnnnddddd` | translated |  | `simd_two_register_misc.cpp` |
| 133 | `FCVTPS_4` | FCVTPS (vector) | `0Q0011101z100001101010nnnnnddddd` | translated |  | `simd_two_register_misc.cpp` |
| 134 | `FCVTZS_int_4` | FCVTZS (vector, integer) | `0Q0011101z100001101110nnnnnddddd` | translated |  | `simd_two_register_misc.cpp` |
| 135 | `URECPE` | URECPE | `0Q0011101z100001110010nnnnnddddd` | translated | ReservedValue | `simd_two_register_misc.cpp` |
| 136 | `FRECPE_4` | FRECPE | `0Q0011101z100001110110nnnnnddddd` | translated | ReservedValue | `simd_two_register_misc.cpp` |
| 137 | `FCVTXN_2` | FCVTXN, FCVTXN2 | `0Q1011100z100001011010nnnnnddddd` | translated | UnallocatedEncoding | `simd_two_register_misc.cpp` |
| 138 | `FRINTA_2` | FRINTA (vector) | `0Q1011100z100001100010nnnnnddddd` | translated |  | `simd_two_register_misc.cpp` |
| 139 | `FRINTX_2` | FRINTX (vector) | `0Q1011100z100001100110nnnnnddddd` | translated |  | `simd_two_register_misc.cpp` |
| 140 | `FCVTNU_4` | FCVTNU (vector) | `0Q1011100z100001101010nnnnnddddd` | translated |  | `simd_two_register_misc.cpp` |
| 141 | `FCVTMU_4` | FCVTMU (vector) | `0Q1011100z100001101110nnnnnddddd` | translated |  | `simd_two_register_misc.cpp` |
| 142 | `FCVTAU_4` | FCVTAU (vector) | `0Q1011100z100001110010nnnnnddddd` | translated |  | `simd_two_register_misc.cpp` |
| 143 | `UCVTF_int_4` | UCVTF (vector, integer) | `0Q1011100z100001110110nnnnnddddd` | translated |  | `simd_two_register_misc.cpp` |
| 144 | `FNEG_2` | FNEG (vector) | `0Q1011101z100000111110nnnnnddddd` | translated | ReservedValue | `simd_two_register_misc.cpp` |
| 145 | `FRINTI_2` | FRINTI (vector) | `0Q1011101z100001100110nnnnnddddd` | translated |  | `simd_two_register_misc.cpp` |
| 146 | `FCMGE_zero_4` | FCMGE (zero) | `0Q1011101z100000110010nnnnnddddd` | translated |  | `simd_two_register_misc.cpp` |
| 147 | `FCMLE_4` | FCMLE (zero) | `0Q1011101z100000110110nnnnnddddd` | translated |  | `simd_two_register_misc.cpp` |
| 148 | `FCVTPU_4` | FCVTPU (vector) | `0Q1011101z100001101010nnnnnddddd` | translated |  | `simd_two_register_misc.cpp` |
| 149 | `FCVTZU_int_4` | FCVTZU (vector, integer) | `0Q1011101z100001101110nnnnnddddd` | translated |  | `simd_two_register_misc.cpp` |
| 150 | `URSQRTE` | URSQRTE | `0Q1011101z100001110010nnnnnddddd` | translated | ReservedValue | `simd_two_register_misc.cpp` |
| 151 | `FRSQRTE_4` | FRSQRTE | `0Q1011101z100001110110nnnnnddddd` | translated | ReservedValue | `simd_two_register_misc.cpp` |
| 152 | `FSQRT_2` | FSQRT (vector) | `0Q1011101z100001111110nnnnnddddd` | translated | ReservedValue | `simd_two_register_misc.cpp` |
| 153 | `FMAXNMV_2` | FMAXNMV | `0Q1011100z110000110010nnnnnddddd` | translated |  | `simd_across_lanes.cpp` |
| 154 | `FMAXV_2` | FMAXV | `0Q1011100z110000111110nnnnnddddd` | translated |  | `simd_across_lanes.cpp` |
| 155 | `FMINNMV_2` | FMINNMV | `0Q1011101z110000110010nnnnnddddd` | translated |  | `simd_across_lanes.cpp` |
| 156 | `FMINV_2` | FMINV | `0Q1011101z110000111110nnnnnddddd` | translated |  | `simd_across_lanes.cpp` |
| 157 | `FMOV_float` | FMOV (register) | `00011110yy100000010000nnnnnddddd` | translated | UnallocatedEncoding | `floating_point_data_processing_one_register.cpp` |
| 158 | `FABS_float` | FABS (scalar) | `00011110yy100000110000nnnnnddddd` | translated | UnallocatedEncoding | `floating_point_data_processing_one_register.cpp` |
| 159 | `FNEG_float` | FNEG (scalar) | `00011110yy100001010000nnnnnddddd` | translated | UnallocatedEncoding | `floating_point_data_processing_one_register.cpp` |
| 160 | `FSQRT_float` | FSQRT (scalar) | `00011110yy100001110000nnnnnddddd` | translated | UnallocatedEncoding | `floating_point_data_processing_one_register.cpp` |
| 161 | `FRINTN_float` | FRINTN (scalar) | `00011110yy100100010000nnnnnddddd` | translated |  | `floating_point_data_processing_one_register.cpp` |
| 162 | `FRINTP_float` | FRINTP (scalar) | `00011110yy100100110000nnnnnddddd` | translated |  | `floating_point_data_processing_one_register.cpp` |
| 163 | `FRINTM_float` | FRINTM (scalar) | `00011110yy100101010000nnnnnddddd` | translated |  | `floating_point_data_processing_one_register.cpp` |
| 164 | `FRINTZ_float` | FRINTZ (scalar) | `00011110yy100101110000nnnnnddddd` | translated |  | `floating_point_data_processing_one_register.cpp` |
| 165 | `FRINTA_float` | FRINTA (scalar) | `00011110yy100110010000nnnnnddddd` | translated |  | `floating_point_data_processing_one_register.cpp` |
| 166 | `FRINTX_float` | FRINTX (scalar) | `00011110yy100111010000nnnnnddddd` | translated |  | `floating_point_data_processing_one_register.cpp` |
| 167 | `FRINTI_float` | FRINTI (scalar) | `00011110yy100111110000nnnnnddddd` | translated |  | `floating_point_data_processing_one_register.cpp` |
| 168 | `LD1R_1` | LD1R | `0Q001101010000001100zznnnnnttttt` | translated |  | `load_store_single_structure.cpp` |
| 169 | `LD3R_1` | LD3R | `0Q001101010000001110zznnnnnttttt` | translated |  | `load_store_single_structure.cpp` |
| 170 | `LD2R_1` | LD2R | `0Q001101011000001100zznnnnnttttt` | translated |  | `load_store_single_structure.cpp` |
| 171 | `LD4R_1` | LD4R | `0Q001101011000001110zznnnnnttttt` | translated |  | `load_store_single_structure.cpp` |
| 172 | `REV64_asimd` | REV64 | `0Q001110zz100000000010nnnnnddddd` | translated | UnallocatedEncoding | `simd_two_register_misc.cpp` |
| 173 | `REV16_asimd` | REV16 (vector) | `0Q001110zz100000000110nnnnnddddd` | translated | UnallocatedEncoding | `simd_two_register_misc.cpp` |
| 174 | `SADDLP` | SADDLP | `0Q001110zz100000001010nnnnnddddd` | translated |  | `simd_two_register_misc.cpp` |
| 175 | `SUQADD_2` | SUQADD | `0Q001110zz100000001110nnnnnddddd` | translated | ReservedValue | `simd_two_register_misc.cpp` |
| 176 | `CLS_asimd` | CLS (vector) | `0Q001110zz100000010010nnnnnddddd` | translated | ReservedValue | `simd_two_register_misc.cpp` |
| 177 | `CNT` | CNT | `0Q001110zz100000010110nnnnnddddd` | translated | ReservedValue | `simd_two_register_misc.cpp` |
| 178 | `SADALP` | SADALP | `0Q001110zz100000011010nnnnnddddd` | translated |  | `simd_two_register_misc.cpp` |
| 179 | `SQABS_2` | SQABS | `0Q001110zz100000011110nnnnnddddd` | translated | ReservedValue | `simd_two_register_misc.cpp` |
| 180 | `CMGT_zero_2` | CMGT (zero) | `0Q001110zz100000100010nnnnnddddd` | translated |  | `simd_two_register_misc.cpp` |
| 181 | `CMEQ_zero_2` | CMEQ (zero) | `0Q001110zz100000100110nnnnnddddd` | translated |  | `simd_two_register_misc.cpp` |
| 182 | `CMLT_2` | CMLT (zero) | `0Q001110zz100000101010nnnnnddddd` | translated |  | `simd_two_register_misc.cpp` |
| 183 | `ABS_2` | ABS | `0Q001110zz100000101110nnnnnddddd` | translated | ReservedValue | `simd_two_register_misc.cpp` |
| 184 | `XTN` | XTN, XTN2 | `0Q001110zz100001001010nnnnnddddd` | translated | ReservedValue | `simd_two_register_misc.cpp` |
| 185 | `SQXTN_2` | SQXTN, SQXTN2 | `0Q001110zz100001010010nnnnnddddd` | translated |  | `simd_two_register_misc.cpp` |
| 186 | `REV32_asimd` | REV32 (vector) | `0Q101110zz100000000010nnnnnddddd` | translated | UnallocatedEncoding | `simd_two_register_misc.cpp` |
| 187 | `UADDLP` | UADDLP | `0Q101110zz100000001010nnnnnddddd` | translated |  | `simd_two_register_misc.cpp` |
| 188 | `USQADD_2` | USQADD | `0Q101110zz100000001110nnnnnddddd` | translated | ReservedValue | `simd_two_register_misc.cpp` |
| 189 | `CLZ_asimd` | CLZ (vector) | `0Q101110zz100000010010nnnnnddddd` | translated | ReservedValue | `simd_two_register_misc.cpp` |
| 190 | `UADALP` | UADALP | `0Q101110zz100000011010nnnnnddddd` | translated |  | `simd_two_register_misc.cpp` |
| 191 | `SQNEG_2` | SQNEG | `0Q101110zz100000011110nnnnnddddd` | translated | ReservedValue | `simd_two_register_misc.cpp` |
| 192 | `CMGE_zero_2` | CMGE (zero) | `0Q101110zz100000100010nnnnnddddd` | translated |  | `simd_two_register_misc.cpp` |
| 193 | `CMLE_2` | CMLE (zero) | `0Q101110zz100000100110nnnnnddddd` | translated |  | `simd_two_register_misc.cpp` |
| 194 | `NEG_2` | NEG (vector) | `0Q101110zz100000101110nnnnnddddd` | translated | ReservedValue | `simd_two_register_misc.cpp` |
| 195 | `SQXTUN_2` | SQXTUN, SQXTUN2 | `0Q101110zz100001001010nnnnnddddd` | translated |  | `simd_two_register_misc.cpp` |
| 196 | `SHLL` | SHLL, SHLL2 | `0Q101110zz100001001110nnnnnddddd` | translated | ReservedValue | `simd_two_register_misc.cpp` |
| 197 | `UQXTN_2` | UQXTN, UQXTN2 | `0Q101110zz100001010010nnnnnddddd` | translated |  | `simd_two_register_misc.cpp` |
| 198 | `SADDLV` | SADDLV | `0Q001110zz110000001110nnnnnddddd` | translated |  | `simd_across_lanes.cpp` |
| 199 | `SMAXV` | SMAXV | `0Q001110zz110000101010nnnnnddddd` | translated |  | `simd_across_lanes.cpp` |
| 200 | `SMINV` | SMINV | `0Q001110zz110001101010nnnnnddddd` | translated |  | `simd_across_lanes.cpp` |
| 201 | `ADDV` | ADDV | `0Q001110zz110001101110nnnnnddddd` | translated | ReservedValue | `simd_across_lanes.cpp` |
| 202 | `UADDLV` | UADDLV | `0Q101110zz110000001110nnnnnddddd` | translated |  | `simd_across_lanes.cpp` |
| 203 | `UMAXV` | UMAXV | `0Q101110zz110000101010nnnnnddddd` | translated |  | `simd_across_lanes.cpp` |
| 204 | `UMINV` | UMINV | `0Q101110zz110001101010nnnnnddddd` | translated |  | `simd_across_lanes.cpp` |
| 205 | `FCVTNS_float` | FCVTNS (scalar) | `z0011110yy100000000000nnnnnddddd` | translated |  | `floating_point_conversion_integer.cpp` |
| 206 | `FCVTNU_float` | FCVTNU (scalar) | `z0011110yy100001000000nnnnnddddd` | translated |  | `floating_point_conversion_integer.cpp` |
| 207 | `SCVTF_float_int` | SCVTF (scalar, integer) | `z0011110yy100010000000nnnnnddddd` | translated | UNREACHABLE, UnallocatedEncoding | `floating_point_conversion_integer.cpp` |
| 208 | `UCVTF_float_int` | UCVTF (scalar, integer) | `z0011110yy100011000000nnnnnddddd` | translated | UNREACHABLE, UnallocatedEncoding | `floating_point_conversion_integer.cpp` |
| 209 | `FCVTAS_float` | FCVTAS (scalar) | `z0011110yy100100000000nnnnnddddd` | translated |  | `floating_point_conversion_integer.cpp` |
| 210 | `FCVTAU_float` | FCVTAU (scalar) | `z0011110yy100101000000nnnnnddddd` | translated |  | `floating_point_conversion_integer.cpp` |
| 211 | `FCVTPS_float` | FCVTPS (scalar) | `z0011110yy101000000000nnnnnddddd` | translated |  | `floating_point_conversion_integer.cpp` |
| 212 | `FCVTPU_float` | FCVTPU (scalar) | `z0011110yy101001000000nnnnnddddd` | translated |  | `floating_point_conversion_integer.cpp` |
| 213 | `FCVTMS_float` | FCVTMS (scalar) | `z0011110yy110000000000nnnnnddddd` | translated |  | `floating_point_conversion_integer.cpp` |
| 214 | `FCVTMU_float` | FCVTMU (scalar) | `z0011110yy110001000000nnnnnddddd` | translated |  | `floating_point_conversion_integer.cpp` |
| 215 | `FCVTZS_float_int` | FCVTZS (scalar, integer) | `z0011110yy111000000000nnnnnddddd` | translated |  | `floating_point_conversion_integer.cpp` |
| 216 | `FCVTZU_float_int` | FCVTZU (scalar, integer) | `z0011110yy111001000000nnnnnddddd` | translated |  | `floating_point_conversion_integer.cpp` |
| 217 | `FCMP_float` | FCMP | `00011110yy1mmmmm001000nnnnn0o000` | translated |  | `floating_point_compare.cpp` |
| 218 | `FCMPE_float` | FCMPE | `00011110yy1mmmmm001000nnnnn1o000` | translated |  | `floating_point_compare.cpp` |
| 219 | `FCVT_float` | FCVT | `00011110yy10001oo10000nnnnnddddd` | translated | UnallocatedEncoding | `floating_point_data_processing_one_register.cpp` |
| 220 | `ASR_1` | ASR (immediate, 32-bit) | `00010011000rrrrr011111nnnnnddddd` | translated |  | `data_processing_bitfield.cpp` |
| 221 | `RMIF` | RMIF | `10111010000iiiiii00001nnnnn0IIII` | translated |  | `system_flag_manipulation.cpp` |
| 222 | `SMULH` | SMULH | `10011011010mmmmm011111nnnnnddddd` | translated |  | `data_processing_multiply.cpp` |
| 223 | `UMULH` | UMULH | `10011011110mmmmm011111nnnnnddddd` | translated |  | `data_processing_multiply.cpp` |
| 224 | `SHA1C` | SHA1C | `01011110000mmmmm000000nnnnnddddd` | translated |  | `simd_sha.cpp` |
| 225 | `SHA1P` | SHA1P | `01011110000mmmmm000100nnnnnddddd` | translated |  | `simd_sha.cpp` |
| 226 | `SHA1M` | SHA1M | `01011110000mmmmm001000nnnnnddddd` | translated |  | `simd_sha.cpp` |
| 227 | `SHA1SU0` | SHA1SU0 | `01011110000mmmmm001100nnnnnddddd` | translated |  | `simd_sha.cpp` |
| 228 | `SHA256H` | SHA256H | `01011110000mmmmm010000nnnnnddddd` | translated |  | `simd_sha.cpp` |
| 229 | `SHA256H2` | SHA256H2 | `01011110000mmmmm010100nnnnnddddd` | translated |  | `simd_sha.cpp` |
| 230 | `SHA256SU1` | SHA256SU1 | `01011110000mmmmm011000nnnnnddddd` | translated |  | `simd_sha.cpp` |
| 231 | `DUP_elt_1` | DUP (element) | `01011110000iiiii000001nnnnnddddd` | translated | ReservedValue | `simd_copy.cpp` |
| 232 | `FCMEQ_reg_1` | FCMEQ (register) | `01011110010mmmmm001001nnnnnddddd` | translated |  | `simd_scalar_three_same.cpp` |
| 233 | `FRECPS_1` | FRECPS | `01011110010mmmmm001111nnnnnddddd` | translated |  | `simd_scalar_three_same.cpp` |
| 234 | `FRSQRTS_1` | FRSQRTS | `01011110110mmmmm001111nnnnnddddd` | translated |  | `simd_scalar_three_same.cpp` |
| 235 | `INS_gen` | INS (general) | `01001110000iiiii000111nnnnnddddd` | translated | ReservedValue | `simd_copy.cpp` |
| 236 | `SHA512H` | SHA512H | `11001110011mmmmm100000nnnnnddddd` | translated |  | `simd_sha512.cpp` |
| 237 | `SHA512H2` | SHA512H2 | `11001110011mmmmm100001nnnnnddddd` | translated |  | `simd_sha512.cpp` |
| 238 | `SHA512SU1` | SHA512SU1 | `11001110011mmmmm100010nnnnnddddd` | translated |  | `simd_sha512.cpp` |
| 239 | `RAX1` | RAX1 | `11001110011mmmmm100011nnnnnddddd` | translated |  | `simd_sha512.cpp` |
| 240 | `SM3PARTW1` | SM3PARTW1 | `11001110011mmmmm110000nnnnnddddd` | translated |  | `simd_sha512.cpp` |
| 241 | `SM3PARTW2` | SM3PARTW2 | `11001110011mmmmm110001nnnnnddddd` | translated |  | `simd_sha512.cpp` |
| 242 | `SM4EKEY` | SM4EKEY | `11001110011mmmmm110010nnnnnddddd` | translated |  | `simd_sha512.cpp` |
| 243 | `FMOV_float_gen` | FMOV (general) | `z0011110yy10r11o000000nnnnnddddd` | translated | UNREACHABLE, UnallocatedEncoding | `floating_point_conversion_integer.cpp` |
| 244 | `FMOV_float_imm` | FMOV (scalar, immediate) | `00011110yy1iiiiiiii10000000ddddd` | translated | UnallocatedEncoding | `floating_point_data_processing_one_register.cpp` |
| 245 | `ASR_2` | ASR (immediate, 64-bit) | `1001001101rrrrrr111111nnnnnddddd` | translated |  | `data_processing_bitfield.cpp` |
| 246 | `SVC` | SVC | `11010100000iiiiiiiiiiiiiiii00001` | translated |  | `a64_exception_generating.cpp` |
| 247 | `BRK` | BRK | `11010100001iiiiiiiiiiiiiiii00000` | translated | RaiseException | `a64_exception_generating.cpp` |
| 248 | `ST1_sngl_1` | ST1 (single structure) | `0Q00110100000000oo0Szznnnnnttttt` | translated |  | `load_store_single_structure.cpp` |
| 249 | `ST3_sngl_1` | ST3 (single structure) | `0Q00110100000000oo1Szznnnnnttttt` | translated |  | `load_store_single_structure.cpp` |
| 250 | `ST2_sngl_1` | ST2 (single structure) | `0Q00110100100000oo0Szznnnnnttttt` | translated |  | `load_store_single_structure.cpp` |
| 251 | `ST4_sngl_1` | ST4 (single structure) | `0Q00110100100000oo1Szznnnnnttttt` | translated |  | `load_store_single_structure.cpp` |
| 252 | `LD1_sngl_1` | LD1 (single structure) | `0Q00110101000000oo0Szznnnnnttttt` | translated |  | `load_store_single_structure.cpp` |
| 253 | `LD3_sngl_1` | LD3 (single structure) | `0Q00110101000000oo1Szznnnnnttttt` | translated |  | `load_store_single_structure.cpp` |
| 254 | `LD2_sngl_1` | LD2 (single structure) | `0Q00110101100000oo0Szznnnnnttttt` | translated |  | `load_store_single_structure.cpp` |
| 255 | `LD4_sngl_1` | LD4 (single structure) | `0Q00110101100000oo1Szznnnnnttttt` | translated |  | `load_store_single_structure.cpp` |
| 256 | `LDXP` | LDXP | `1z001000011111110uuuuunnnnnttttt` | translated |  | `load_store_exclusive.cpp` |
| 257 | `LDAXP` | LDAXP | `1z001000011111111uuuuunnnnnttttt` | translated |  | `load_store_exclusive.cpp` |
| 258 | `UDIV` | UDIV | `z0011010110mmmmm000010nnnnnddddd` | translated |  | `data_processing_register.cpp` |
| 259 | `SDIV` | SDIV | `z0011010110mmmmm000011nnnnnddddd` | translated |  | `data_processing_register.cpp` |
| 260 | `LSLV` | LSLV | `z0011010110mmmmm001000nnnnnddddd` | translated |  | `data_processing_shift.cpp` |
| 261 | `LSRV` | LSRV | `z0011010110mmmmm001001nnnnnddddd` | translated |  | `data_processing_shift.cpp` |
| 262 | `ASRV` | ASRV | `z0011010110mmmmm001010nnnnnddddd` | translated |  | `data_processing_shift.cpp` |
| 263 | `RORV` | RORV | `z0011010110mmmmm001011nnnnnddddd` | translated |  | `data_processing_shift.cpp` |
| 264 | `ADC` | ADC | `z0011010000mmmmm000000nnnnnddddd` | translated |  | `data_processing_addsub.cpp` |
| 265 | `ADCS` | ADCS | `z0111010000mmmmm000000nnnnnddddd` | translated |  | `data_processing_addsub.cpp` |
| 266 | `SBC` | SBC | `z1011010000mmmmm000000nnnnnddddd` | translated |  | `data_processing_addsub.cpp` |
| 267 | `SBCS` | SBCS | `z1111010000mmmmm000000nnnnnddddd` | translated |  | `data_processing_addsub.cpp` |
| 268 | `FMULX_vec_2` | FMULX | `010111100z1mmmmm110111nnnnnddddd` | translated |  | `simd_scalar_three_same.cpp` |
| 269 | `FCMEQ_reg_2` | FCMEQ (register) | `010111100z1mmmmm111001nnnnnddddd` | translated |  | `simd_scalar_three_same.cpp` |
| 270 | `FRECPS_2` | FRECPS | `010111100z1mmmmm111111nnnnnddddd` | translated |  | `simd_scalar_three_same.cpp` |
| 271 | `FRSQRTS_2` | FRSQRTS | `010111101z1mmmmm111111nnnnnddddd` | translated |  | `simd_scalar_three_same.cpp` |
| 272 | `FCMGE_reg_2` | FCMGE (register) | `011111100z1mmmmm111001nnnnnddddd` | translated |  | `simd_scalar_three_same.cpp` |
| 273 | `FACGE_2` | FACGE | `011111100z1mmmmm111011nnnnnddddd` | translated |  | `simd_scalar_three_same.cpp` |
| 274 | `FABD_2` | FABD | `011111101z1mmmmm110101nnnnnddddd` | translated |  | `simd_scalar_three_same.cpp` |
| 275 | `FCMGT_reg_2` | FCMGT (register) | `011111101z1mmmmm111001nnnnnddddd` | translated |  | `simd_scalar_three_same.cpp` |
| 276 | `FACGT_2` | FACGT | `011111101z1mmmmm111011nnnnnddddd` | translated |  | `simd_scalar_three_same.cpp` |
| 277 | `DUP_elt_2` | DUP (element) | `0Q001110000iiiii000001nnnnnddddd` | translated | ReservedValue | `simd_copy.cpp` |
| 278 | `DUP_gen` | DUP (general) | `0Q001110000iiiii000011nnnnnddddd` | translated | ReservedValue | `simd_copy.cpp` |
| 279 | `SMOV` | SMOV | `0Q001110000iiiii001011nnnnnddddd` | translated | ReservedValue, UnallocatedEncoding | `simd_copy.cpp` |
| 280 | `UMOV` | UMOV | `0Q001110000iiiii001111nnnnnddddd` | translated | ReservedValue, UnallocatedEncoding | `simd_copy.cpp` |
| 281 | `FCMEQ_reg_3` | FCMEQ (register) | `0Q001110010mmmmm001001nnnnnddddd` | translated |  | `simd_three_same.cpp` |
| 282 | `FRECPS_3` | FRECPS | `0Q001110010mmmmm001111nnnnnddddd` | translated |  | `simd_three_same.cpp` |
| 283 | `FRSQRTS_3` | FRSQRTS | `0Q001110110mmmmm001111nnnnnddddd` | translated |  | `simd_three_same.cpp` |
| 284 | `FMLA_vec_1` | FMLA (vector) | `0Q001110010mmmmm000011nnnnnddddd` | translated |  | `simd_three_same.cpp` |
| 285 | `FMLS_vec_1` | FMLS (vector) | `0Q001110110mmmmm000011nnnnnddddd` | translated |  | `simd_three_same.cpp` |
| 286 | `AND_asimd` | AND (vector) | `0Q001110001mmmmm000111nnnnnddddd` | translated |  | `simd_three_same.cpp` |
| 287 | `BIC_asimd_reg` | BIC (vector, register) | `0Q001110011mmmmm000111nnnnnddddd` | translated |  | `simd_three_same.cpp` |
| 288 | `ORR_asimd_reg` | ORR (vector, register) | `0Q001110101mmmmm000111nnnnnddddd` | translated |  | `simd_three_same.cpp` |
| 289 | `ORN_asimd` | ORN (vector) | `0Q001110111mmmmm000111nnnnnddddd` | translated |  | `simd_three_same.cpp` |
| 290 | `EOR_asimd` | EOR (vector) | `0Q101110001mmmmm000111nnnnnddddd` | translated |  | `simd_three_same.cpp` |
| 291 | `BSL` | BSL | `0Q101110011mmmmm000111nnnnnddddd` | translated |  | `simd_three_same.cpp` |
| 292 | `BIT` | BIT | `0Q101110101mmmmm000111nnnnnddddd` | translated |  | `simd_three_same.cpp` |
| 293 | `BIF` | BIF | `0Q101110111mmmmm000111nnnnnddddd` | translated |  | `simd_three_same.cpp` |
| 294 | `STx_mult_1` | STx (multiple structures) | `0Q00110000000000oooozznnnnnttttt` | translated |  | `load_store_multiple_structures.cpp` |
| 295 | `LDx_mult_1` | LDx (multiple structures) | `0Q00110001000000oooozznnnnnttttt` | translated |  | `load_store_multiple_structures.cpp` |
| 296 | `STXR` | STXRB, STXRH, STXR | `zz001000000sssss011111nnnnnttttt` | translated |  | `load_store_exclusive.cpp` |
| 297 | `STLXR` | STLXRB, STLXRH, STLXR | `zz001000000sssss111111nnnnnttttt` | translated |  | `load_store_exclusive.cpp` |
| 298 | `SQADD_1` | SQADD | `01011110zz1mmmmm000011nnnnnddddd` | translated |  | `simd_scalar_three_same.cpp` |
| 299 | `SQSUB_1` | SQSUB | `01011110zz1mmmmm001011nnnnnddddd` | translated |  | `simd_scalar_three_same.cpp` |
| 300 | `CMGT_reg_1` | CMGT (register) | `01011110zz1mmmmm001101nnnnnddddd` | translated |  | `simd_scalar_three_same.cpp` |
| 301 | `CMGE_reg_1` | CMGE (register) | `01011110zz1mmmmm001111nnnnnddddd` | translated |  | `simd_scalar_three_same.cpp` |
| 302 | `SSHL_1` | SSHL | `01011110zz1mmmmm010001nnnnnddddd` | translated | ReservedValue | `simd_scalar_three_same.cpp` |
| 303 | `SQSHL_reg_1` | SQSHL (register) | `01011110zz1mmmmm010011nnnnnddddd` | translated |  | `simd_scalar_three_same.cpp` |
| 304 | `SRSHL_1` | SRSHL | `01011110zz1mmmmm010101nnnnnddddd` | translated |  | `simd_scalar_three_same.cpp` |
| 305 | `ADD_1` | ADD (vector) | `01011110zz1mmmmm100001nnnnnddddd` | translated | ReservedValue | `simd_scalar_three_same.cpp` |
| 306 | `CMTST_1` | CMTST | `01011110zz1mmmmm100011nnnnnddddd` | translated | ReservedValue | `simd_scalar_three_same.cpp` |
| 307 | `SQDMULH_vec_1` | SQDMULH (vector) | `01011110zz1mmmmm101101nnnnnddddd` | translated | ReservedValue | `simd_scalar_three_same.cpp` |
| 308 | `UQADD_1` | UQADD | `01111110zz1mmmmm000011nnnnnddddd` | translated |  | `simd_scalar_three_same.cpp` |
| 309 | `UQSUB_1` | UQSUB | `01111110zz1mmmmm001011nnnnnddddd` | translated |  | `simd_scalar_three_same.cpp` |
| 310 | `CMHI_1` | CMHI (register) | `01111110zz1mmmmm001101nnnnnddddd` | translated |  | `simd_scalar_three_same.cpp` |
| 311 | `CMHS_1` | CMHS (register) | `01111110zz1mmmmm001111nnnnnddddd` | translated |  | `simd_scalar_three_same.cpp` |
| 312 | `USHL_1` | USHL | `01111110zz1mmmmm010001nnnnnddddd` | translated | ReservedValue | `simd_scalar_three_same.cpp` |
| 313 | `UQSHL_reg_1` | UQSHL (register) | `01111110zz1mmmmm010011nnnnnddddd` | translated |  | `simd_scalar_three_same.cpp` |
| 314 | `URSHL_1` | URSHL | `01111110zz1mmmmm010101nnnnnddddd` | translated |  | `simd_scalar_three_same.cpp` |
| 315 | `SUB_1` | SUB (vector) | `01111110zz1mmmmm100001nnnnnddddd` | translated | ReservedValue | `simd_scalar_three_same.cpp` |
| 316 | `CMEQ_reg_1` | CMEQ (register) | `01111110zz1mmmmm100011nnnnnddddd` | translated |  | `simd_scalar_three_same.cpp` |
| 317 | `SQRDMULH_vec_1` | SQRDMULH (vector) | `01111110zz1mmmmm101101nnnnnddddd` | translated | ReservedValue | `simd_scalar_three_same.cpp` |
| 318 | `SSHR_1` | SSHR | `010111110IIIIiii000001nnnnnddddd` | translated |  | `simd_scalar_shift_by_immediate.cpp` |
| 319 | `SSRA_1` | SSRA | `010111110IIIIiii000101nnnnnddddd` | translated |  | `simd_scalar_shift_by_immediate.cpp` |
| 320 | `SRSHR_1` | SRSHR | `010111110IIIIiii001001nnnnnddddd` | translated |  | `simd_scalar_shift_by_immediate.cpp` |
| 321 | `SRSRA_1` | SRSRA | `010111110IIIIiii001101nnnnnddddd` | translated |  | `simd_scalar_shift_by_immediate.cpp` |
| 322 | `SHL_1` | SHL | `010111110IIIIiii010101nnnnnddddd` | translated | ReservedValue | `simd_scalar_shift_by_immediate.cpp` |
| 323 | `SQSHL_imm_1` | SQSHL (immediate) | `010111110IIIIiii011101nnnnnddddd` | translated |  | `simd_scalar_shift_by_immediate.cpp` |
| 324 | `SQSHRN_1` | SQSHRN, SQSHRN2 | `010111110IIIIiii100101nnnnnddddd` | translated |  | `simd_scalar_shift_by_immediate.cpp` |
| 325 | `SCVTF_fix_1` | SCVTF (vector, fixed-point) | `010111110IIIIiii111001nnnnnddddd` | translated |  | `simd_scalar_shift_by_immediate.cpp` |
| 326 | `FCVTZS_fix_1` | FCVTZS (vector, fixed-point) | `010111110IIIIiii111111nnnnnddddd` | translated |  | `simd_scalar_shift_by_immediate.cpp` |
| 327 | `USHR_1` | USHR | `011111110IIIIiii000001nnnnnddddd` | translated |  | `simd_scalar_shift_by_immediate.cpp` |
| 328 | `USRA_1` | USRA | `011111110IIIIiii000101nnnnnddddd` | translated |  | `simd_scalar_shift_by_immediate.cpp` |
| 329 | `URSHR_1` | URSHR | `011111110IIIIiii001001nnnnnddddd` | translated |  | `simd_scalar_shift_by_immediate.cpp` |
| 330 | `URSRA_1` | URSRA | `011111110IIIIiii001101nnnnnddddd` | translated |  | `simd_scalar_shift_by_immediate.cpp` |
| 331 | `SRI_1` | SRI | `011111110IIIIiii010001nnnnnddddd` | translated |  | `simd_scalar_shift_by_immediate.cpp` |
| 332 | `SLI_1` | SLI | `011111110IIIIiii010101nnnnnddddd` | translated |  | `simd_scalar_shift_by_immediate.cpp` |
| 333 | `SQSHLU_1` | SQSHLU | `011111110IIIIiii011001nnnnnddddd` | translated |  | `simd_scalar_shift_by_immediate.cpp` |
| 334 | `UQSHL_imm_1` | UQSHL (immediate) | `011111110IIIIiii011101nnnnnddddd` | translated |  | `simd_scalar_shift_by_immediate.cpp` |
| 335 | `SQSHRUN_1` | SQSHRUN, SQSHRUN2 | `011111110IIIIiii100001nnnnnddddd` | translated |  | `simd_scalar_shift_by_immediate.cpp` |
| 336 | `UQSHRN_1` | UQSHRN, UQSHRN2 | `011111110IIIIiii100101nnnnnddddd` | translated |  | `simd_scalar_shift_by_immediate.cpp` |
| 337 | `UCVTF_fix_1` | UCVTF (vector, fixed-point) | `011111110IIIIiii111001nnnnnddddd` | translated |  | `simd_scalar_shift_by_immediate.cpp` |
| 338 | `FCVTZU_fix_1` | FCVTZU (vector, fixed-point) | `011111110IIIIiii111111nnnnnddddd` | translated |  | `simd_scalar_shift_by_immediate.cpp` |
| 339 | `FMLA_elt_1` | FMLA (by element) | `0101111100LMmmmm0001H0nnnnnddddd` | translated |  | `simd_scalar_x_indexed_element.cpp` |
| 340 | `FMLS_elt_1` | FMLS (by element) | `0101111100LMmmmm0101H0nnnnnddddd` | translated |  | `simd_scalar_x_indexed_element.cpp` |
| 341 | `FMAXNM_2` | FMAXNM (vector) | `0Q0011100z1mmmmm110001nnnnnddddd` | translated |  | `simd_three_same.cpp` |
| 342 | `FMLA_vec_2` | FMLA (vector) | `0Q0011100z1mmmmm110011nnnnnddddd` | translated | ReservedValue | `simd_three_same.cpp` |
| 343 | `FADD_2` | FADD (vector) | `0Q0011100z1mmmmm110101nnnnnddddd` | translated | ReservedValue | `simd_three_same.cpp` |
| 344 | `FMAX_2` | FMAX (vector) | `0Q0011100z1mmmmm111101nnnnnddddd` | translated |  | `simd_three_same.cpp` |
| 345 | `FMULX_vec_4` | FMULX | `0Q0011100z1mmmmm110111nnnnnddddd` | translated | ReservedValue | `simd_three_same.cpp` |
| 346 | `FCMEQ_reg_4` | FCMEQ (register) | `0Q0011100z1mmmmm111001nnnnnddddd` | translated |  | `simd_three_same.cpp` |
| 347 | `FRECPS_4` | FRECPS | `0Q0011100z1mmmmm111111nnnnnddddd` | translated | ReservedValue | `simd_three_same.cpp` |
| 348 | `FMINNM_2` | FMINNM (vector) | `0Q0011101z1mmmmm110001nnnnnddddd` | translated |  | `simd_three_same.cpp` |
| 349 | `FMLS_vec_2` | FMLS (vector) | `0Q0011101z1mmmmm110011nnnnnddddd` | translated | ReservedValue | `simd_three_same.cpp` |
| 350 | `FSUB_2` | FSUB (vector) | `0Q0011101z1mmmmm110101nnnnnddddd` | translated | ReservedValue | `simd_three_same.cpp` |
| 351 | `FMIN_2` | FMIN (vector) | `0Q0011101z1mmmmm111101nnnnnddddd` | translated |  | `simd_three_same.cpp` |
| 352 | `FRSQRTS_4` | FRSQRTS | `0Q0011101z1mmmmm111111nnnnnddddd` | translated | ReservedValue | `simd_three_same.cpp` |
| 353 | `FMAXNMP_vec_2` | FMAXNMP (vector) | `0Q1011100z1mmmmm110001nnnnnddddd` | translated |  | `simd_three_same.cpp` |
| 354 | `FADDP_vec_2` | FADDP (vector) | `0Q1011100z1mmmmm110101nnnnnddddd` | translated | ReservedValue | `simd_three_same.cpp` |
| 355 | `FMUL_vec_2` | FMUL (vector) | `0Q1011100z1mmmmm110111nnnnnddddd` | translated | ReservedValue | `simd_three_same.cpp` |
| 356 | `FCMGE_reg_4` | FCMGE (register) | `0Q1011100z1mmmmm111001nnnnnddddd` | translated |  | `simd_three_same.cpp` |
| 357 | `FACGE_4` | FACGE | `0Q1011100z1mmmmm111011nnnnnddddd` | translated |  | `simd_three_same.cpp` |
| 358 | `FMAXP_vec_2` | FMAXP (vector) | `0Q1011100z1mmmmm111101nnnnnddddd` | translated |  | `simd_three_same.cpp` |
| 359 | `FDIV_2` | FDIV (vector) | `0Q1011100z1mmmmm111111nnnnnddddd` | translated | ReservedValue | `simd_three_same.cpp` |
| 360 | `FMINNMP_vec_2` | FMINNMP (vector) | `0Q1011101z1mmmmm110001nnnnnddddd` | translated |  | `simd_three_same.cpp` |
| 361 | `FABD_4` | FABD | `0Q1011101z1mmmmm110101nnnnnddddd` | translated | ReservedValue | `simd_three_same.cpp` |
| 362 | `FCMGT_reg_4` | FCMGT (register) | `0Q1011101z1mmmmm111001nnnnnddddd` | translated |  | `simd_three_same.cpp` |
| 363 | `FACGT_4` | FACGT | `0Q1011101z1mmmmm111011nnnnnddddd` | translated |  | `simd_three_same.cpp` |
| 364 | `FMINP_vec_2` | FMINP (vector) | `0Q1011101z1mmmmm111101nnnnnddddd` | translated |  | `simd_three_same.cpp` |
| 365 | `SM3TT1A` | SM3TT1A | `11001110010mmmmm10ii00nnnnnddddd` | translated |  | `simd_crypto_three_register.cpp` |
| 366 | `SM3TT1B` | SM3TT1B | `11001110010mmmmm10ii01nnnnnddddd` | translated |  | `simd_crypto_three_register.cpp` |
| 367 | `SM3TT2A` | SM3TT2A | `11001110010mmmmm10ii10nnnnnddddd` | translated |  | `simd_crypto_three_register.cpp` |
| 368 | `SM3TT2B` | SM3TT2B | `11001110010mmmmm10ii11nnnnnddddd` | translated |  | `simd_crypto_three_register.cpp` |
| 369 | `FMUL_float` | FMUL (scalar) | `00011110yy1mmmmm000010nnnnnddddd` | translated | UnallocatedEncoding | `floating_point_data_processing_two_register.cpp` |
| 370 | `FDIV_float` | FDIV (scalar) | `00011110yy1mmmmm000110nnnnnddddd` | translated | UnallocatedEncoding | `floating_point_data_processing_two_register.cpp` |
| 371 | `FADD_float` | FADD (scalar) | `00011110yy1mmmmm001010nnnnnddddd` | translated | UnallocatedEncoding | `floating_point_data_processing_two_register.cpp` |
| 372 | `FSUB_float` | FSUB (scalar) | `00011110yy1mmmmm001110nnnnnddddd` | translated | UnallocatedEncoding | `floating_point_data_processing_two_register.cpp` |
| 373 | `FMAX_float` | FMAX (scalar) | `00011110yy1mmmmm010010nnnnnddddd` | translated | UnallocatedEncoding | `floating_point_data_processing_two_register.cpp` |
| 374 | `FMIN_float` | FMIN (scalar) | `00011110yy1mmmmm010110nnnnnddddd` | translated | UnallocatedEncoding | `floating_point_data_processing_two_register.cpp` |
| 375 | `FMAXNM_float` | FMAXNM (scalar) | `00011110yy1mmmmm011010nnnnnddddd` | translated | UnallocatedEncoding | `floating_point_data_processing_two_register.cpp` |
| 376 | `FMINNM_float` | FMINNM (scalar) | `00011110yy1mmmmm011110nnnnnddddd` | translated | UnallocatedEncoding | `floating_point_data_processing_two_register.cpp` |
| 377 | `FNMUL_float` | FNMUL (scalar) | `00011110yy1mmmmm100010nnnnnddddd` | translated | UnallocatedEncoding | `floating_point_data_processing_two_register.cpp` |
| 378 | `LD1R_2` | LD1R | `0Q001101110mmmmm1100zznnnnnttttt` | translated |  | `load_store_single_structure.cpp` |
| 379 | `LD3R_2` | LD3R | `0Q001101110mmmmm1110zznnnnnttttt` | translated |  | `load_store_single_structure.cpp` |
| 380 | `LD2R_2` | LD2R | `0Q001101111mmmmm1100zznnnnnttttt` | translated |  | `load_store_single_structure.cpp` |
| 381 | `LD4R_2` | LD4R | `0Q001101111mmmmm1110zznnnnnttttt` | translated |  | `load_store_single_structure.cpp` |
| 382 | `CRC32` | CRC32B, CRC32H, CRC32W, CRC32X | `z0011010110mmmmm0100zznnnnnddddd` | translated | UnallocatedEncoding | `data_processing_crc32.cpp` |
| 383 | `CRC32C` | CRC32CB, CRC32CH, CRC32CW, CRC32CX | `z0011010110mmmmm0101zznnnnnddddd` | translated | UnallocatedEncoding | `data_processing_crc32.cpp` |
| 384 | `FMLA_elt_2` | FMLA (by element) | `010111111zLMmmmm0001H0nnnnnddddd` | translated |  | `simd_scalar_x_indexed_element.cpp` |
| 385 | `FMLS_elt_2` | FMLS (by element) | `010111111zLMmmmm0101H0nnnnnddddd` | translated |  | `simd_scalar_x_indexed_element.cpp` |
| 386 | `FMUL_elt_2` | FMUL (by element) | `010111111zLMmmmm1001H0nnnnnddddd` | translated |  | `simd_scalar_x_indexed_element.cpp` |
| 387 | `FMULX_elt_2` | FMULX (by element) | `011111111zLMmmmm1001H0nnnnnddddd` | translated |  | `simd_scalar_x_indexed_element.cpp` |
| 388 | `TBL` | TBL | `0Q001110000mmmmm0LL000nnnnnddddd` | translated |  | `simd_table_lookup.cpp` |
| 389 | `TBX` | TBX | `0Q001110000mmmmm0LL100nnnnnddddd` | translated |  | `simd_table_lookup.cpp` |
| 390 | `UZP1` | UZP1 | `0Q001110zz0mmmmm000110nnnnnddddd` | translated |  | `simd_permute.cpp` |
| 391 | `TRN1` | TRN1 | `0Q001110zz0mmmmm001010nnnnnddddd` | translated |  | `simd_permute.cpp` |
| 392 | `ZIP1` | ZIP1 | `0Q001110zz0mmmmm001110nnnnnddddd` | translated | ReservedValue | `simd_permute.cpp` |
| 393 | `UZP2` | UZP2 | `0Q001110zz0mmmmm010110nnnnnddddd` | translated |  | `simd_permute.cpp` |
| 394 | `TRN2` | TRN2 | `0Q001110zz0mmmmm011010nnnnnddddd` | translated |  | `simd_permute.cpp` |
| 395 | `ZIP2` | ZIP2 | `0Q001110zz0mmmmm011110nnnnnddddd` | translated | ReservedValue | `simd_permute.cpp` |
| 396 | `SDOT_vec` | SDOT (vector) | `0Q001110zz0mmmmm100101nnnnnddddd` | translated |  | `simd_three_same_extra.cpp` |
| 397 | `UDOT_vec` | UDOT (vector) | `0Q101110zz0mmmmm100101nnnnnddddd` | translated |  | `simd_three_same_extra.cpp` |
| 398 | `SADDL` | SADDL, SADDL2 | `0Q001110zz1mmmmm000000nnnnnddddd` | translated |  | `simd_three_different.cpp` |
| 399 | `SADDW` | SADDW, SADDW2 | `0Q001110zz1mmmmm000100nnnnnddddd` | translated |  | `simd_three_different.cpp` |
| 400 | `SSUBL` | SSUBL, SSUBL2 | `0Q001110zz1mmmmm001000nnnnnddddd` | translated |  | `simd_three_different.cpp` |
| 401 | `SSUBW` | SSUBW, SSUBW2 | `0Q001110zz1mmmmm001100nnnnnddddd` | translated |  | `simd_three_different.cpp` |
| 402 | `ADDHN` | ADDHN, ADDHN2 | `0Q001110zz1mmmmm010000nnnnnddddd` | translated |  | `simd_three_same.cpp` |
| 403 | `SABAL` | SABAL, SABAL2 | `0Q001110zz1mmmmm010100nnnnnddddd` | translated |  | `simd_three_different.cpp` |
| 404 | `SUBHN` | SUBHN, SUBHN2 | `0Q001110zz1mmmmm011000nnnnnddddd` | translated |  | `simd_three_same.cpp` |
| 405 | `SABDL` | SABDL, SABDL2 | `0Q001110zz1mmmmm011100nnnnnddddd` | translated |  | `simd_three_different.cpp` |
| 406 | `SMLAL_vec` | SMLAL, SMLAL2 (vector) | `0Q001110zz1mmmmm100000nnnnnddddd` | translated |  | `simd_three_different.cpp` |
| 407 | `SMLSL_vec` | SMLSL, SMLSL2 (vector) | `0Q001110zz1mmmmm101000nnnnnddddd` | translated |  | `simd_three_different.cpp` |
| 408 | `SMULL_vec` | SMULL, SMULL2 (vector) | `0Q001110zz1mmmmm110000nnnnnddddd` | translated |  | `simd_three_different.cpp` |
| 409 | `PMULL` | PMULL, PMULL2 | `0Q001110zz1mmmmm111000nnnnnddddd` | translated | ReservedValue | `simd_three_different.cpp` |
| 410 | `UADDL` | UADDL, UADDL2 | `0Q101110zz1mmmmm000000nnnnnddddd` | translated |  | `simd_three_different.cpp` |
| 411 | `UADDW` | UADDW, UADDW2 | `0Q101110zz1mmmmm000100nnnnnddddd` | translated |  | `simd_three_different.cpp` |
| 412 | `USUBL` | USUBL, USUBL2 | `0Q101110zz1mmmmm001000nnnnnddddd` | translated |  | `simd_three_different.cpp` |
| 413 | `USUBW` | USUBW, USUBW2 | `0Q101110zz1mmmmm001100nnnnnddddd` | translated |  | `simd_three_different.cpp` |
| 414 | `RADDHN` | RADDHN, RADDHN2 | `0Q101110zz1mmmmm010000nnnnnddddd` | translated |  | `simd_three_same.cpp` |
| 415 | `UABAL` | UABAL, UABAL2 | `0Q101110zz1mmmmm010100nnnnnddddd` | translated |  | `simd_three_different.cpp` |
| 416 | `RSUBHN` | RSUBHN, RSUBHN2 | `0Q101110zz1mmmmm011000nnnnnddddd` | translated |  | `simd_three_same.cpp` |
| 417 | `UABDL` | UABDL, UABDL2 | `0Q101110zz1mmmmm011100nnnnnddddd` | translated |  | `simd_three_different.cpp` |
| 418 | `UMLAL_vec` | UMLAL, UMLAL2 (vector) | `0Q101110zz1mmmmm100000nnnnnddddd` | translated |  | `simd_three_different.cpp` |
| 419 | `UMLSL_vec` | UMLSL, UMLSL2 (vector) | `0Q101110zz1mmmmm101000nnnnnddddd` | translated |  | `simd_three_different.cpp` |
| 420 | `UMULL_vec` | UMULL, UMULL2 (vector) | `0Q101110zz1mmmmm110000nnnnnddddd` | translated |  | `simd_three_different.cpp` |
| 421 | `SQDMULL_vec_2` | SQDMULL, SQDMULL2 (vector) | `0Q001110zz1mmmmm110100nnnnnddddd` | translated | ReservedValue | `simd_three_different.cpp` |
| 422 | `SHADD` | SHADD | `0Q001110zz1mmmmm000001nnnnnddddd` | translated | ReservedValue | `simd_three_same.cpp` |
| 423 | `SQADD_2` | SQADD | `0Q001110zz1mmmmm000011nnnnnddddd` | translated |  | `simd_three_same.cpp` |
| 424 | `SRHADD` | SRHADD | `0Q001110zz1mmmmm000101nnnnnddddd` | translated |  | `simd_three_same.cpp` |
| 425 | `SHSUB` | SHSUB | `0Q001110zz1mmmmm001001nnnnnddddd` | translated | ReservedValue | `simd_three_same.cpp` |
| 426 | `SQSUB_2` | SQSUB | `0Q001110zz1mmmmm001011nnnnnddddd` | translated |  | `simd_three_same.cpp` |
| 427 | `CMGT_reg_2` | CMGT (register) | `0Q001110zz1mmmmm001101nnnnnddddd` | translated | ReservedValue | `simd_three_same.cpp` |
| 428 | `CMGE_reg_2` | CMGE (register) | `0Q001110zz1mmmmm001111nnnnnddddd` | translated | ReservedValue | `simd_three_same.cpp` |
| 429 | `SSHL_2` | SSHL | `0Q001110zz1mmmmm010001nnnnnddddd` | translated | ReservedValue | `simd_three_same.cpp` |
| 430 | `SQSHL_reg_2` | SQSHL (register) | `0Q001110zz1mmmmm010011nnnnnddddd` | translated |  | `simd_three_same.cpp` |
| 431 | `SRSHL_2` | SRSHL | `0Q001110zz1mmmmm010101nnnnnddddd` | translated |  | `simd_three_same.cpp` |
| 432 | `SMAX` | SMAX | `0Q001110zz1mmmmm011001nnnnnddddd` | translated |  | `simd_three_same.cpp` |
| 433 | `SMIN` | SMIN | `0Q001110zz1mmmmm011011nnnnnddddd` | translated |  | `simd_three_same.cpp` |
| 434 | `SABD` | SABD | `0Q001110zz1mmmmm011101nnnnnddddd` | translated |  | `simd_three_same.cpp` |
| 435 | `SABA` | SABA | `0Q001110zz1mmmmm011111nnnnnddddd` | translated |  | `simd_three_same.cpp` |
| 436 | `ADD_vector` | ADD (vector) | `0Q001110zz1mmmmm100001nnnnnddddd` | translated | ReservedValue | `simd_three_same.cpp` |
| 437 | `CMTST_2` | CMTST | `0Q001110zz1mmmmm100011nnnnnddddd` | translated | ReservedValue | `simd_three_same.cpp` |
| 438 | `MLA_vec` | MLA (vector) | `0Q001110zz1mmmmm100101nnnnnddddd` | translated | ReservedValue | `simd_three_same.cpp` |
| 439 | `MUL_vec` | MUL (vector) | `0Q001110zz1mmmmm100111nnnnnddddd` | translated | ReservedValue | `simd_three_same.cpp` |
| 440 | `SMAXP` | SMAXP | `0Q001110zz1mmmmm101001nnnnnddddd` | translated |  | `simd_three_same.cpp` |
| 441 | `SMINP` | SMINP | `0Q001110zz1mmmmm101011nnnnnddddd` | translated |  | `simd_three_same.cpp` |
| 442 | `SQDMULH_vec_2` | SQDMULH (vector) | `0Q001110zz1mmmmm101101nnnnnddddd` | translated | ReservedValue | `simd_three_same.cpp` |
| 443 | `ADDP_vec` | ADDP (vector) | `0Q001110zz1mmmmm101111nnnnnddddd` | translated | ReservedValue | `simd_three_same.cpp` |
| 444 | `UHADD` | UHADD | `0Q101110zz1mmmmm000001nnnnnddddd` | translated | ReservedValue | `simd_three_same.cpp` |
| 445 | `UQADD_2` | UQADD | `0Q101110zz1mmmmm000011nnnnnddddd` | translated |  | `simd_three_same.cpp` |
| 446 | `URHADD` | URHADD | `0Q101110zz1mmmmm000101nnnnnddddd` | translated |  | `simd_three_same.cpp` |
| 447 | `UHSUB` | UHSUB | `0Q101110zz1mmmmm001001nnnnnddddd` | translated | ReservedValue | `simd_three_same.cpp` |
| 448 | `UQSUB_2` | UQSUB | `0Q101110zz1mmmmm001011nnnnnddddd` | translated |  | `simd_three_same.cpp` |
| 449 | `CMHI_2` | CMHI (register) | `0Q101110zz1mmmmm001101nnnnnddddd` | translated | ReservedValue | `simd_three_same.cpp` |
| 450 | `CMHS_2` | CMHS (register) | `0Q101110zz1mmmmm001111nnnnnddddd` | translated | ReservedValue | `simd_three_same.cpp` |
| 451 | `USHL_2` | USHL | `0Q101110zz1mmmmm010001nnnnnddddd` | translated | ReservedValue | `simd_three_same.cpp` |
| 452 | `UQSHL_reg_2` | UQSHL (register) | `0Q101110zz1mmmmm010011nnnnnddddd` | translated |  | `simd_three_same.cpp` |
| 453 | `URSHL_2` | URSHL | `0Q101110zz1mmmmm010101nnnnnddddd` | translated |  | `simd_three_same.cpp` |
| 454 | `UMAX` | UMAX | `0Q101110zz1mmmmm011001nnnnnddddd` | translated |  | `simd_three_same.cpp` |
| 455 | `UMIN` | UMIN | `0Q101110zz1mmmmm011011nnnnnddddd` | translated |  | `simd_three_same.cpp` |
| 456 | `UABD` | UABD | `0Q101110zz1mmmmm011101nnnnnddddd` | translated | ReservedValue | `simd_three_same.cpp` |
| 457 | `UABA` | UABA | `0Q101110zz1mmmmm011111nnnnnddddd` | translated | ReservedValue | `simd_three_same.cpp` |
| 458 | `SUB_2` | SUB (vector) | `0Q101110zz1mmmmm100001nnnnnddddd` | translated | ReservedValue | `simd_three_same.cpp` |
| 459 | `CMEQ_reg_2` | CMEQ (register) | `0Q101110zz1mmmmm100011nnnnnddddd` | translated | ReservedValue | `simd_three_same.cpp` |
| 460 | `MLS_vec` | MLS (vector) | `0Q101110zz1mmmmm100101nnnnnddddd` | translated | ReservedValue | `simd_three_same.cpp` |
| 461 | `PMUL` | PMUL | `0Q101110zz1mmmmm100111nnnnnddddd` | translated | ReservedValue | `simd_three_same.cpp` |
| 462 | `UMAXP` | UMAXP | `0Q101110zz1mmmmm101001nnnnnddddd` | translated |  | `simd_three_same.cpp` |
| 463 | `UMINP` | UMINP | `0Q101110zz1mmmmm101011nnnnnddddd` | translated |  | `simd_three_same.cpp` |
| 464 | `SQRDMULH_vec_2` | SQRDMULH (vector) | `0Q101110zz1mmmmm101101nnnnnddddd` | translated | ReservedValue | `simd_three_same.cpp` |
| 465 | `SSHR_2` | SSHR | `0Q0011110IIIIiii000001nnnnnddddd` | translated |  | `simd_shift_by_immediate.cpp` |
| 466 | `SSRA_2` | SSRA | `0Q0011110IIIIiii000101nnnnnddddd` | translated |  | `simd_shift_by_immediate.cpp` |
| 467 | `SRSHR_2` | SRSHR | `0Q0011110IIIIiii001001nnnnnddddd` | translated |  | `simd_shift_by_immediate.cpp` |
| 468 | `SRSRA_2` | SRSRA | `0Q0011110IIIIiii001101nnnnnddddd` | translated |  | `simd_shift_by_immediate.cpp` |
| 469 | `SHL_2` | SHL | `0Q0011110IIIIiii010101nnnnnddddd` | translated | DecodeError, ReservedValue | `simd_shift_by_immediate.cpp` |
| 470 | `SQSHL_imm_2` | SQSHL (immediate) | `0Q0011110IIIIiii011101nnnnnddddd` | translated |  | `simd_shift_by_immediate.cpp` |
| 471 | `SHRN` | SHRN, SHRN2 | `0Q0011110IIIIiii100001nnnnnddddd` | translated |  | `simd_shift_by_immediate.cpp` |
| 472 | `RSHRN` | RSHRN, RSHRN2 | `0Q0011110IIIIiii100011nnnnnddddd` | translated |  | `simd_shift_by_immediate.cpp` |
| 473 | `SQSHRN_2` | SQSHRN, SQSHRN2 | `0Q0011110IIIIiii100101nnnnnddddd` | translated |  | `simd_shift_by_immediate.cpp` |
| 474 | `SQRSHRN_2` | SQRSHRN, SQRSHRN2 | `0Q0011110IIIIiii100111nnnnnddddd` | translated |  | `simd_shift_by_immediate.cpp` |
| 475 | `SSHLL` | SSHLL, SSHLL2 | `0Q0011110IIIIiii101001nnnnnddddd` | translated |  | `simd_shift_by_immediate.cpp` |
| 476 | `SCVTF_fix_2` | SCVTF (vector, fixed-point) | `0Q0011110IIIIiii111001nnnnnddddd` | translated |  | `simd_shift_by_immediate.cpp` |
| 477 | `FCVTZS_fix_2` | FCVTZS (vector, fixed-point) | `0Q0011110IIIIiii111111nnnnnddddd` | translated |  | `simd_shift_by_immediate.cpp` |
| 478 | `USHR_2` | USHR | `0Q1011110IIIIiii000001nnnnnddddd` | translated |  | `simd_shift_by_immediate.cpp` |
| 479 | `USRA_2` | USRA | `0Q1011110IIIIiii000101nnnnnddddd` | translated |  | `simd_shift_by_immediate.cpp` |
| 480 | `URSHR_2` | URSHR | `0Q1011110IIIIiii001001nnnnnddddd` | translated |  | `simd_shift_by_immediate.cpp` |
| 481 | `URSRA_2` | URSRA | `0Q1011110IIIIiii001101nnnnnddddd` | translated |  | `simd_shift_by_immediate.cpp` |
| 482 | `SRI_2` | SRI | `0Q1011110IIIIiii010001nnnnnddddd` | translated | DecodeError, ReservedValue | `simd_shift_by_immediate.cpp` |
| 483 | `SLI_2` | SLI | `0Q1011110IIIIiii010101nnnnnddddd` | translated | DecodeError, ReservedValue | `simd_shift_by_immediate.cpp` |
| 484 | `SQSHLU_2` | SQSHLU | `0Q1011110IIIIiii011001nnnnnddddd` | translated |  | `simd_shift_by_immediate.cpp` |
| 485 | `UQSHL_imm_2` | UQSHL (immediate) | `0Q1011110IIIIiii011101nnnnnddddd` | translated |  | `simd_shift_by_immediate.cpp` |
| 486 | `SQSHRUN_2` | SQSHRUN, SQSHRUN2 | `0Q1011110IIIIiii100001nnnnnddddd` | translated |  | `simd_shift_by_immediate.cpp` |
| 487 | `SQRSHRUN_2` | SQRSHRUN, SQRSHRUN2 | `0Q1011110IIIIiii100011nnnnnddddd` | translated |  | `simd_shift_by_immediate.cpp` |
| 488 | `UQSHRN_2` | UQSHRN, UQSHRN2 | `0Q1011110IIIIiii100101nnnnnddddd` | translated |  | `simd_shift_by_immediate.cpp` |
| 489 | `UQRSHRN_2` | UQRSHRN, UQRSHRN2 | `0Q1011110IIIIiii100111nnnnnddddd` | translated |  | `simd_shift_by_immediate.cpp` |
| 490 | `USHLL` | USHLL, USHLL2 | `0Q1011110IIIIiii101001nnnnnddddd` | translated |  | `simd_shift_by_immediate.cpp` |
| 491 | `UCVTF_fix_2` | UCVTF (vector, fixed-point) | `0Q1011110IIIIiii111001nnnnnddddd` | translated |  | `simd_shift_by_immediate.cpp` |
| 492 | `FCVTZU_fix_2` | FCVTZU (vector, fixed-point) | `0Q1011110IIIIiii111111nnnnnddddd` | translated |  | `simd_shift_by_immediate.cpp` |
| 493 | `FMLA_elt_3` | FMLA (by element) | `0Q00111100LMmmmm0001H0nnnnnddddd` | translated |  | `simd_vector_x_indexed_element.cpp` |
| 494 | `FMLS_elt_3` | FMLS (by element) | `0Q00111100LMmmmm0101H0nnnnnddddd` | translated |  | `simd_vector_x_indexed_element.cpp` |
| 495 | `UnallocatedEncoding` |  | `10111000110---------00----------` | architectural undefined | RaiseException | `impl.cpp` |
| 496 | `PRFM_unscaled_imm` | PRFM (unscaled offset) | `11111000100iiiiiiiii00nnnnnttttt` | translated |  | `load_store_register_immediate.cpp` |
| 497 | `STTRB` | STTRB | `00111000000iiiiiiiii10nnnnnttttt` | translated |  | `load_store_register_unprivileged.cpp` |
| 498 | `LDTRB` | LDTRB | `00111000010iiiiiiiii10nnnnnttttt` | translated |  | `load_store_register_unprivileged.cpp` |
| 499 | `STTRH` | STTRH | `01111000000iiiiiiiii10nnnnnttttt` | translated |  | `load_store_register_unprivileged.cpp` |
| 500 | `LDTRH` | LDTRH | `01111000010iiiiiiiii10nnnnnttttt` | translated |  | `load_store_register_unprivileged.cpp` |
| 501 | `LDTRSW` | LDTRSW | `10111000100iiiiiiiii10nnnnnttttt` | translated |  | `load_store_register_unprivileged.cpp` |
| 502 | `CCMN_reg` | CCMN (register) | `z0111010010mmmmmcccc00nnnnn0ffff` | translated |  | `data_processing_conditional_compare.cpp` |
| 503 | `CCMP_reg` | CCMP (register) | `z1111010010mmmmmcccc00nnnnn0ffff` | translated |  | `data_processing_conditional_compare.cpp` |
| 504 | `CCMN_imm` | CCMN (immediate) | `z0111010010iiiiicccc10nnnnn0ffff` | translated |  | `data_processing_conditional_compare.cpp` |
| 505 | `CCMP_imm` | CCMP (immediate) | `z1111010010iiiiicccc10nnnnn0ffff` | translated |  | `data_processing_conditional_compare.cpp` |
| 506 | `SQDMULL_elt_1` | SQDMULL, SQDMULL2 (by element) | `01011111zzLMmmmm1011H0nnnnnddddd` | translated | ReservedValue | `simd_scalar_x_indexed_element.cpp` |
| 507 | `SQDMULH_elt_1` | SQDMULH (by element) | `01011111zzLMmmmm1100H0nnnnnddddd` | translated | ReservedValue | `simd_scalar_x_indexed_element.cpp` |
| 508 | `SQRDMULH_elt_1` | SQRDMULH (by element) | `01011111zzLMmmmm1101H0nnnnnddddd` | translated | ReservedValue | `simd_scalar_x_indexed_element.cpp` |
| 509 | `INS_elt` | INS (element) | `01101110000iiiii0iiii1nnnnnddddd` | translated | ReservedValue | `simd_copy.cpp` |
| 510 | `FCADD_vec` | FCADD | `0Q101110zz0mmmmm111r01nnnnnddddd` | translated | ReservedValue | `simd_three_same_extra.cpp` |
| 511 | `FMLA_elt_4` | FMLA (by element) | `0Q0011111zLMmmmm0001H0nnnnnddddd` | translated |  | `simd_vector_x_indexed_element.cpp` |
| 512 | `FMLS_elt_4` | FMLS (by element) | `0Q0011111zLMmmmm0101H0nnnnnddddd` | translated |  | `simd_vector_x_indexed_element.cpp` |
| 513 | `FMUL_elt_4` | FMUL (by element) | `0Q0011111zLMmmmm1001H0nnnnnddddd` | translated |  | `simd_vector_x_indexed_element.cpp` |
| 514 | `FMULX_elt_4` | FMULX (by element) | `0Q1011111zLMmmmm1001H0nnnnnddddd` | translated |  | `simd_vector_x_indexed_element.cpp` |
| 515 | `SCVTF_float_fix` | SCVTF (scalar, fixed-point) | `z0011110yy000010ppppppnnnnnddddd` | translated | UNREACHABLE, UnallocatedEncoding | `floating_point_conversion_fixed_point.cpp` |
| 516 | `UCVTF_float_fix` | UCVTF (scalar, fixed-point) | `z0011110yy000011ppppppnnnnnddddd` | translated | UNREACHABLE, UnallocatedEncoding | `floating_point_conversion_fixed_point.cpp` |
| 517 | `FCVTZS_float_fix` | FCVTZS (scalar, fixed-point) | `z0011110yy011000ppppppnnnnnddddd` | translated | UNREACHABLE, UnallocatedEncoding | `floating_point_conversion_fixed_point.cpp` |
| 518 | `FCVTZU_float_fix` | FCVTZU (scalar, fixed-point) | `z0011110yy011001ppppppnnnnnddddd` | translated | UNREACHABLE, UnallocatedEncoding | `floating_point_conversion_fixed_point.cpp` |
| 519 | `MSR_reg` | MSR (register) | `110101010001poooNNNNMMMMooottttt` | translated | UNREACHABLE | `system.cpp` |
| 520 | `MRS` | MRS | `110101010011poooNNNNMMMMooottttt` | translated | UNREACHABLE | `system.cpp` |
| 521 | `UnallocatedEncoding` |  | `111110001-0---------00----------` | architectural undefined | RaiseException | `impl.cpp` |
| 522 | `UnallocatedEncoding` |  | `10111000110----------1----------` | architectural undefined | RaiseException | `impl.cpp` |
| 523 | `CSEL` | CSEL | `z0011010100mmmmmcccc00nnnnnddddd` | translated |  | `data_processing_conditional_select.cpp` |
| 524 | `CSINC` | CSINC | `z0011010100mmmmmcccc01nnnnnddddd` | translated |  | `data_processing_conditional_select.cpp` |
| 525 | `CSINV` | CSINV | `z1011010100mmmmmcccc00nnnnnddddd` | translated |  | `data_processing_conditional_select.cpp` |
| 526 | `CSNEG` | CSNEG | `z1011010100mmmmmcccc01nnnnnddddd` | translated |  | `data_processing_conditional_select.cpp` |
| 527 | `SMADDL` | SMADDL | `10011011001mmmmm0aaaaannnnnddddd` | translated |  | `data_processing_multiply.cpp` |
| 528 | `SMSUBL` | SMSUBL | `10011011001mmmmm1aaaaannnnnddddd` | translated |  | `data_processing_multiply.cpp` |
| 529 | `UMADDL` | UMADDL | `10011011101mmmmm0aaaaannnnnddddd` | translated |  | `data_processing_multiply.cpp` |
| 530 | `UMSUBL` | UMSUBL | `10011011101mmmmm1aaaaannnnnddddd` | translated |  | `data_processing_multiply.cpp` |
| 531 | `EXT` | EXT | `0Q101110000mmmmm0iiii0nnnnnddddd` | translated | ReservedValue | `simd_extract.cpp` |
| 532 | `FCMLA_vec` | FCMLA | `0Q101110zz0mmmmm110rr1nnnnnddddd` | translated | ReservedValue | `simd_three_same_extra.cpp` |
| 533 | `SMLAL_elt` | SMLAL, SMLAL2 (by element) | `0Q001111zzLMmmmm0010H0nnnnnddddd` | translated |  | `simd_vector_x_indexed_element.cpp` |
| 534 | `SMLSL_elt` | SMLSL, SMLSL2 (by element) | `0Q001111zzLMmmmm0110H0nnnnnddddd` | translated |  | `simd_vector_x_indexed_element.cpp` |
| 535 | `MUL_elt` | MUL (by element) | `0Q001111zzLMmmmm1000H0nnnnnddddd` | translated |  | `simd_vector_x_indexed_element.cpp` |
| 536 | `SMULL_elt` | SMULL, SMULL2 (by element) | `0Q001111zzLMmmmm1010H0nnnnnddddd` | translated |  | `simd_vector_x_indexed_element.cpp` |
| 537 | `SQDMULL_elt_2` | SQDMULL, SQDMULL2 (by element) | `0Q001111zzLMmmmm1011H0nnnnnddddd` | translated | ReservedValue | `simd_vector_x_indexed_element.cpp` |
| 538 | `SQDMULH_elt_2` | SQDMULH (by element) | `0Q001111zzLMmmmm1100H0nnnnnddddd` | translated | ReservedValue | `simd_vector_x_indexed_element.cpp` |
| 539 | `SQRDMULH_elt_2` | SQRDMULH (by element) | `0Q001111zzLMmmmm1101H0nnnnnddddd` | translated | ReservedValue | `simd_vector_x_indexed_element.cpp` |
| 540 | `SDOT_elt` | SDOT (by element) | `0Q001111zzLMmmmm1110H0nnnnnddddd` | translated |  | `simd_vector_x_indexed_element.cpp` |
| 541 | `MLA_elt` | MLA (by element) | `0Q101111zzLMmmmm0000H0nnnnnddddd` | translated |  | `simd_vector_x_indexed_element.cpp` |
| 542 | `UMLAL_elt` | UMLAL, UMLAL2 (by element) | `0Q101111zzLMmmmm0010H0nnnnnddddd` | translated |  | `simd_vector_x_indexed_element.cpp` |
| 543 | `MLS_elt` | MLS (by element) | `0Q101111zzLMmmmm0100H0nnnnnddddd` | translated |  | `simd_vector_x_indexed_element.cpp` |
| 544 | `UMLSL_elt` | UMLSL, UMLSL2 (by element) | `0Q101111zzLMmmmm0110H0nnnnnddddd` | translated |  | `simd_vector_x_indexed_element.cpp` |
| 545 | `UMULL_elt` | UMULL, UMULL2 (by element) | `0Q101111zzLMmmmm1010H0nnnnnddddd` | translated |  | `simd_vector_x_indexed_element.cpp` |
| 546 | `UDOT_elt` | UDOT (by element) | `0Q101111zzLMmmmm1110H0nnnnnddddd` | translated |  | `simd_vector_x_indexed_element.cpp` |
| 547 | `EOR3` | EOR3 | `11001110000mmmmm0aaaaannnnnddddd` | translated |  | `simd_crypto_four_register.cpp` |
| 548 | `BCAX` | BCAX | `11001110001mmmmm0aaaaannnnnddddd` | translated |  | `simd_crypto_four_register.cpp` |
| 549 | `SM3SS1` | SM3SS1 | `11001110010mmmmm0aaaaannnnnddddd` | translated |  | `simd_crypto_four_register.cpp` |
| 550 | `FCCMP_float` | FCCMP | `00011110yy1mmmmmcccc01nnnnn0ffff` | translated |  | `floating_point_conditional_compare.cpp` |
| 551 | `FCCMPE_float` | FCCMPE | `00011110yy1mmmmmcccc01nnnnn1ffff` | translated |  | `floating_point_conditional_compare.cpp` |
| 552 | `ST1_sngl_2` | ST1 (single structure) | `0Q001101100mmmmmoo0Szznnnnnttttt` | translated |  | `load_store_single_structure.cpp` |
| 553 | `ST3_sngl_2` | ST3 (single structure) | `0Q001101100mmmmmoo1Szznnnnnttttt` | translated |  | `load_store_single_structure.cpp` |
| 554 | `ST2_sngl_2` | ST2 (single structure) | `0Q001101101mmmmmoo0Szznnnnnttttt` | translated |  | `load_store_single_structure.cpp` |
| 555 | `ST4_sngl_2` | ST4 (single structure) | `0Q001101101mmmmmoo1Szznnnnnttttt` | translated |  | `load_store_single_structure.cpp` |
| 556 | `LD1_sngl_2` | LD1 (single structure) | `0Q001101110mmmmmoo0Szznnnnnttttt` | translated |  | `load_store_single_structure.cpp` |
| 557 | `LD3_sngl_2` | LD3 (single structure) | `0Q001101110mmmmmoo1Szznnnnnttttt` | translated |  | `load_store_single_structure.cpp` |
| 558 | `LD2_sngl_2` | LD2 (single structure) | `0Q001101111mmmmmoo0Szznnnnnttttt` | translated |  | `load_store_single_structure.cpp` |
| 559 | `LD4_sngl_2` | LD4 (single structure) | `0Q001101111mmmmmoo1Szznnnnnttttt` | translated |  | `load_store_single_structure.cpp` |
| 560 | `STXP` | STXP | `1z001000001sssss0uuuuunnnnnttttt` | translated |  | `load_store_exclusive.cpp` |
| 561 | `STLXP` | STLXP | `1z001000001sssss1uuuuunnnnnttttt` | translated |  | `load_store_exclusive.cpp` |
| 562 | `UnallocatedEncoding` |  | `111110001-0----------1----------` | architectural undefined | RaiseException | `impl.cpp` |
| 563 | `LDTRSB` | LDTRSB | `00111000oo0iiiiiiiii10nnnnnttttt` | translated |  | `load_store_register_unprivileged.cpp` |
| 564 | `LDTRSH` | LDTRSH | `01111000oo0iiiiiiiii10nnnnnttttt` | translated |  | `load_store_register_unprivileged.cpp` |
| 565 | `STTR` | STTR | `zz111000000iiiiiiiii10nnnnnttttt` | translated |  | `load_store_register_unprivileged.cpp` |
| 566 | `LDTR` | LDTR | `zz111000010iiiiiiiii10nnnnnttttt` | translated |  | `load_store_register_unprivileged.cpp` |
| 567 | `MADD` | MADD | `z0011011000mmmmm0aaaaannnnnddddd` | translated |  | `data_processing_multiply.cpp` |
| 568 | `MSUB` | MSUB | `z0011011000mmmmm1aaaaannnnnddddd` | translated |  | `data_processing_multiply.cpp` |
| 569 | `XAR` | XAR | `11001110100mmmmmiiiiiinnnnnddddd` | translated |  | `simd_sha512.cpp` |
| 570 | `FCSEL_float` | FCSEL | `00011110yy1mmmmmcccc11nnnnnddddd` | translated | UnallocatedEncoding | `floating_point_conditional_select.cpp` |
| 571 | `STx_mult_2` | STx (multiple structures) | `0Q001100100mmmmmoooozznnnnnttttt` | translated |  | `load_store_multiple_structures.cpp` |
| 572 | `LDx_mult_2` | LDx (multiple structures) | `0Q001100110mmmmmoooozznnnnnttttt` | translated |  | `load_store_multiple_structures.cpp` |
| 573 | `PRFM_imm` | PRFM (immediate) | `1111100110iiiiiiiiiiiinnnnnttttt` | translated |  | `load_store_register_immediate.cpp` |
| 574 | `STUR_fpsimd` | STUR (SIMD&FP) | `zz111100o00iiiiiiiii00nnnnnttttt` | translated | UnallocatedEncoding | `load_store_register_immediate.cpp` |
| 575 | `LDUR_fpsimd` | LDUR (SIMD&FP) | `zz111100o10iiiiiiiii00nnnnnttttt` | translated | UnallocatedEncoding | `load_store_register_immediate.cpp` |
| 576 | `UnallocatedEncoding` |  | `1111100111----------------------` | architectural undefined | RaiseException | `impl.cpp` |
| 577 | `UnallocatedEncoding` |  | `1011100111----------------------` | architectural undefined | RaiseException | `impl.cpp` |
| 578 | `STRx_reg` | STRx (register) | `zz111000o01mmmmmxxxS10nnnnnttttt` | translated | UnallocatedEncoding | `load_store_register_register_offset.cpp` |
| 579 | `LDRx_reg` | LDRx (register) | `zz111000o11mmmmmxxxS10nnnnnttttt` | translated | UnallocatedEncoding | `load_store_register_register_offset.cpp` |
| 580 | `STR_reg_fpsimd` | STR (register, SIMD&FP) | `zz111100o01mmmmmxxxS10nnnnnttttt` | translated | UnallocatedEncoding | `load_store_register_register_offset.cpp` |
| 581 | `LDR_reg_fpsimd` | LDR (register, SIMD&FP) | `zz111100o11mmmmmxxxS10nnnnnttttt` | translated | UnallocatedEncoding | `load_store_register_register_offset.cpp` |
| 582 | `ADD_ext` | ADD (extended register) | `z0001011001mmmmmxxxiiinnnnnddddd` | translated | ReservedValue | `data_processing_addsub.cpp` |
| 583 | `ADDS_ext` | ADDS (extended register) | `z0101011001mmmmmxxxiiinnnnnddddd` | translated | ReservedValue | `data_processing_addsub.cpp` |
| 584 | `SUB_ext` | SUB (extended register) | `z1001011001mmmmmxxxiiinnnnnddddd` | translated | ReservedValue | `data_processing_addsub.cpp` |
| 585 | `SUBS_ext` | SUBS (extended register) | `z1101011001mmmmmxxxiiinnnnnddddd` | translated | ReservedValue | `data_processing_addsub.cpp` |
| 586 | `FCMLA_elt` | FCMLA (by element) | `0Q101111zzLMmmmm0rr1H0nnnnnddddd` | translated | ReservedValue | `simd_vector_x_indexed_element.cpp` |
| 587 | `FMADD_float` | FMADD | `00011111yy0mmmmm0aaaaannnnnddddd` | translated | UnallocatedEncoding | `floating_point_data_processing_three_register.cpp` |
| 588 | `FMSUB_float` | FMSUB | `00011111yy0mmmmm1aaaaannnnnddddd` | translated | UnallocatedEncoding | `floating_point_data_processing_three_register.cpp` |
| 589 | `FNMADD_float` | FNMADD | `00011111yy1mmmmm0aaaaannnnnddddd` | translated | UnallocatedEncoding | `floating_point_data_processing_three_register.cpp` |
| 590 | `FNMSUB_float` | FNMSUB | `00011111yy1mmmmm1aaaaannnnnddddd` | translated | UnallocatedEncoding | `floating_point_data_processing_three_register.cpp` |
| 591 | `EXTR` | EXTR | `z00100111N0mmmmmssssssnnnnnddddd` | translated | ReservedValue, UnallocatedEncoding | `data_processing_bitfield.cpp` |
| 592 | `B_cond` | B.cond | `01010100iiiiiiiiiiiiiiiiiii0cccc` | translated |  | `a64_branch.cpp` |
| 593 | `STURx_LDURx` | STURx/LDURx | `zz111000oo0iiiiiiiii00nnnnnttttt` | translated |  | `load_store_register_immediate.cpp` |
| 594 | `STR_imm_fpsimd_1` | STR (immediate, SIMD&FP) | `zz111100o00iiiiiiiiip1nnnnnttttt` | translated | UnallocatedEncoding | `load_store_register_immediate.cpp` |
| 595 | `LDR_imm_fpsimd_1` | LDR (immediate, SIMD&FP) | `zz111100o10iiiiiiiiip1nnnnnttttt` | translated | UnallocatedEncoding | `load_store_register_immediate.cpp` |
| 596 | `AND_imm` | AND (immediate) | `z00100100Nrrrrrrssssssnnnnnddddd` | translated | ReservedValue | `data_processing_logical.cpp` |
| 597 | `ORR_imm` | ORR (immediate) | `z01100100Nrrrrrrssssssnnnnnddddd` | translated | ReservedValue | `data_processing_logical.cpp` |
| 598 | `EOR_imm` | EOR (immediate) | `z10100100Nrrrrrrssssssnnnnnddddd` | translated | ReservedValue | `data_processing_logical.cpp` |
| 599 | `ANDS_imm` | ANDS (immediate) | `z11100100Nrrrrrrssssssnnnnnddddd` | translated | ReservedValue | `data_processing_logical.cpp` |
| 600 | `MOVN` | MOVN | `z00100101ssiiiiiiiiiiiiiiiiddddd` | translated | UnallocatedEncoding | `move_wide.cpp` |
| 601 | `MOVZ` | MOVZ | `z10100101ssiiiiiiiiiiiiiiiiddddd` | translated | UnallocatedEncoding | `move_wide.cpp` |
| 602 | `MOVK` | MOVK | `z11100101ssiiiiiiiiiiiiiiiiddddd` | translated | UnallocatedEncoding | `move_wide.cpp` |
| 603 | `SBFM` | SBFM | `z00100110Nrrrrrrssssssnnnnnddddd` | translated | ReservedValue | `data_processing_bitfield.cpp` |
| 604 | `BFM` | BFM | `z01100110Nrrrrrrssssssnnnnnddddd` | translated | ReservedValue | `data_processing_bitfield.cpp` |
| 605 | `UBFM` | UBFM | `z10100110Nrrrrrrssssssnnnnnddddd` | translated | ReservedValue | `data_processing_bitfield.cpp` |
| 606 | `LDRSW_lit` | LDRSW (literal) | `10011000iiiiiiiiiiiiiiiiiiittttt` | translated |  | `load_store_load_literal.cpp` |
| 607 | `PRFM_lit` | PRFM (literal) | `11011000iiiiiiiiiiiiiiiiiiittttt` | translated |  | `load_store_load_literal.cpp` |
| 608 | `STNP_LDNP_gen` | STNP/LDNP | `o01010000Liiiiiiiuuuuunnnnnttttt` | translated |  | `load_store_no_allocate_pair.cpp` |
| 609 | `STRx_LDRx_imm_1` | STRx/LDRx (immediate) | `zz111000oo0iiiiiiiiip1nnnnnttttt` | translated |  | `load_store_register_immediate.cpp` |
| 610 | `AND_shift` | AND (shifted register) | `z0001010ss0mmmmmiiiiiinnnnnddddd` | translated | ReservedValue | `data_processing_logical.cpp` |
| 611 | `BIC_shift` | BIC (shifted register) | `z0001010ss1mmmmmiiiiiinnnnnddddd` | translated | ReservedValue | `data_processing_logical.cpp` |
| 612 | `ORR_shift` | ORR (shifted register) | `z0101010ss0mmmmmiiiiiinnnnnddddd` | translated | ReservedValue | `data_processing_logical.cpp` |
| 613 | `ORN_shift` | ORN (shifted register) | `z0101010ss1mmmmmiiiiiinnnnnddddd` | translated | ReservedValue | `data_processing_logical.cpp` |
| 614 | `EOR_shift` | EOR (shifted register) | `z1001010ss0mmmmmiiiiiinnnnnddddd` | translated | ReservedValue | `data_processing_logical.cpp` |
| 615 | `EON` | EON (shifted register) | `z1001010ss1mmmmmiiiiiinnnnnddddd` | translated | ReservedValue | `data_processing_logical.cpp` |
| 616 | `ANDS_shift` | ANDS (shifted register) | `z1101010ss0mmmmmiiiiiinnnnnddddd` | translated | ReservedValue | `data_processing_logical.cpp` |
| 617 | `BICS` | BICS (shifted register) | `z1101010ss1mmmmmiiiiiinnnnnddddd` | translated | ReservedValue | `data_processing_logical.cpp` |
| 618 | `ADD_shift` | ADD (shifted register) | `z0001011ss0mmmmmiiiiiinnnnnddddd` | translated | ReservedValue | `data_processing_addsub.cpp` |
| 619 | `ADDS_shift` | ADDS (shifted register) | `z0101011ss0mmmmmiiiiiinnnnnddddd` | translated | ReservedValue | `data_processing_addsub.cpp` |
| 620 | `SUB_shift` | SUB (shifted register) | `z1001011ss0mmmmmiiiiiinnnnnddddd` | translated | ReservedValue | `data_processing_addsub.cpp` |
| 621 | `SUBS_shift` | SUBS (shifted register) | `z1101011ss0mmmmmiiiiiinnnnnddddd` | translated | ReservedValue | `data_processing_addsub.cpp` |
| 622 | `ADD_imm` | ADD (immediate) | `z0010001ssiiiiiiiiiiiinnnnnddddd` | translated | ReservedValue | `data_processing_addsub.cpp` |
| 623 | `ADDS_imm` | ADDS (immediate) | `z0110001ssiiiiiiiiiiiinnnnnddddd` | translated | ReservedValue | `data_processing_addsub.cpp` |
| 624 | `SUB_imm` | SUB (immediate) | `z1010001ssiiiiiiiiiiiinnnnnddddd` | translated | ReservedValue | `data_processing_addsub.cpp` |
| 625 | `SUBS_imm` | SUBS (immediate) | `z1110001ssiiiiiiiiiiiinnnnnddddd` | translated | ReservedValue | `data_processing_addsub.cpp` |
| 626 | `CBZ` | CBZ | `z0110100iiiiiiiiiiiiiiiiiiittttt` | translated |  | `a64_branch.cpp` |
| 627 | `CBNZ` | CBNZ | `z0110101iiiiiiiiiiiiiiiiiiittttt` | translated |  | `a64_branch.cpp` |
| 628 | `TBZ` | TBZ | `b0110110bbbbbiiiiiiiiiiiiiittttt` | translated |  | `a64_branch.cpp` |
| 629 | `TBNZ` | TBNZ | `b0110111bbbbbiiiiiiiiiiiiiittttt` | translated |  | `a64_branch.cpp` |
| 630 | `LDR_lit_gen` | LDR (literal) | `0z011000iiiiiiiiiiiiiiiiiiittttt` | translated |  | `load_store_load_literal.cpp` |
| 631 | `STNP_LDNP_fpsimd` | STNP/LDNP (SIMD&FP) | `oo1011000Liiiiiiiuuuuunnnnnttttt` | translated |  | `load_store_no_allocate_pair.cpp` |
| 632 | `UnallocatedEncoding` |  | `--1010000-----------------------` | architectural undefined | RaiseException | `impl.cpp` |
| 633 | `UnallocatedEncoding` |  | `--1011000-----------------------` | architectural undefined | RaiseException | `impl.cpp` |
| 634 | `STR_imm_fpsimd_2` | STR (immediate, SIMD&FP) | `zz111101o0iiiiiiiiiiiinnnnnttttt` | translated | UnallocatedEncoding | `load_store_register_immediate.cpp` |
| 635 | `LDR_imm_fpsimd_2` | LDR (immediate, SIMD&FP) | `zz111101o1iiiiiiiiiiiinnnnnttttt` | translated | UnallocatedEncoding | `load_store_register_immediate.cpp` |
| 636 | `ADR` | ADR | `0ii10000iiiiiiiiiiiiiiiiiiiddddd` | translated |  | `data_processing_pcrel.cpp` |
| 637 | `ADRP` | ADRP | `1ii10000iiiiiiiiiiiiiiiiiiiddddd` | translated |  | `data_processing_pcrel.cpp` |
| 638 | `B_uncond` | B | `000101iiiiiiiiiiiiiiiiiiiiiiiiii` | translated |  | `a64_branch.cpp` |
| 639 | `BL` | BL | `100101iiiiiiiiiiiiiiiiiiiiiiiiii` | translated |  | `a64_branch.cpp` |
| 640 | `LDR_lit_fpsimd` | LDR (literal, SIMD&FP) | `oo011100iiiiiiiiiiiiiiiiiiittttt` | translated | UnallocatedEncoding | `load_store_load_literal.cpp` |
| 641 | `STRx_LDRx_imm_2` | STRx/LDRx (immediate) | `zz111001ooiiiiiiiiiiiinnnnnttttt` | translated |  | `load_store_register_immediate.cpp` |
| 642 | `STP_LDP_gen` | STP/LDP | `oo10100pwLiiiiiiiuuuuunnnnnttttt` | translated | UNREACHABLE, UnallocatedEncoding, UnpredictableInstruction | `load_store_register_pair.cpp` |
| 643 | `STP_LDP_fpsimd` | STP/LDP (SIMD&FP) | `oo10110pwLiiiiiiiuuuuunnnnnttttt` | translated | UNREACHABLE, UnallocatedEncoding, UnpredictableInstruction | `load_store_register_pair.cpp` |

### arm

| Row | Handler | Name | Encoding | Status | Guards | Source |
|---:|---|---|---|---|---|---|
| 1 | `arm_CLREX` | CLREX | `11110101011111111111000000011111` | translated |  | `synchronization.cpp` |
| 2 | `arm_SETEND` | SETEND | `1111000100000001000000e000000000` | translated |  | `status_register_access.cpp` |
| 3 | `arm_DMB` | DMB | `1111010101111111111100000101oooo` | translated |  | `barrier.cpp` |
| 4 | `arm_DSB` | DSB | `1111010101111111111100000100oooo` | translated |  | `barrier.cpp` |
| 5 | `arm_ISB` | ISB | `1111010101111111111100000110oooo` | translated |  | `barrier.cpp` |
| 6 | `arm_SEV` | SEV | `----0011001000001111000000000100` | translated | RaiseException | `hint.cpp` |
| 7 | `arm_SEVL` | SEVL | `----0011001000001111000000000101` | translated | RaiseException | `hint.cpp` |
| 8 | `arm_WFE` | WFE | `----0011001000001111000000000010` | translated | RaiseException | `hint.cpp` |
| 9 | `arm_WFI` | WFI | `----0011001000001111000000000011` | translated | RaiseException | `hint.cpp` |
| 10 | `arm_YIELD` | YIELD | `----0011001000001111000000000001` | translated | RaiseException | `hint.cpp` |
| 11 | `arm_NOP` | NOP | `----0011001000001111000000000000` | translated |  | `a32_translate_impl.cpp` |
| 12 | `arm_RFE` | RFE | `1111100--0-1----0000101000000000` | unreachable stub | UNREACHABLE | `status_register_access.cpp` |
| 13 | `arm_BLX_reg` | BLX (reg) | `cccc000100101111111111110011mmmm` | translated | UnpredictableInstruction | `a32_branch.cpp` |
| 14 | `arm_BX` | BX | `cccc000100101111111111110001mmmm` | translated |  | `a32_branch.cpp` |
| 15 | `arm_BXJ` | BXJ | `cccc000100101111111111110010mmmm` | translated |  | `a32_branch.cpp` |
| 16 | `arm_NOP` | Reserved Hint | `----001100100000111100000000----` | translated |  | `a32_translate_impl.cpp` |
| 17 | `arm_MRS` | MRS | `cccc000100001111dddd000000000000` | translated | UnpredictableInstruction | `status_register_access.cpp` |
| 18 | `arm_SRS` | SRS | `1111100--1-0110100000101000-----` | unreachable stub | UNREACHABLE | `status_register_access.cpp` |
| 19 | `arm_CPS` | CPS | `111100010000---00000000---0-----` | unreachable stub | UNREACHABLE | `status_register_access.cpp` |
| 20 | `arm_STL` | STL | `cccc00011000nnnn111111001001tttt` | translated | UnpredictableInstruction | `synchronization.cpp` |
| 21 | `arm_LDA` | LDA | `cccc00011001nnnndddd110010011111` | translated | UnpredictableInstruction | `synchronization.cpp` |
| 22 | `arm_LDAEX` | LDAEX | `cccc00011001nnnndddd111010011111` | translated | UnpredictableInstruction | `synchronization.cpp` |
| 23 | `arm_LDREX` | LDREX | `cccc00011001nnnndddd111110011111` | translated | UnpredictableInstruction | `synchronization.cpp` |
| 24 | `arm_LDAEXD` | LDAEXD | `cccc00011011nnnndddd111010011111` | translated | UnpredictableInstruction | `synchronization.cpp` |
| 25 | `arm_LDREXD` | LDREXD | `cccc00011011nnnndddd111110011111` | translated | UnpredictableInstruction | `synchronization.cpp` |
| 26 | `arm_STLB` | STLB | `cccc00011100nnnn111111001001tttt` | translated | UnpredictableInstruction | `synchronization.cpp` |
| 27 | `arm_LDAB` | LDAB | `cccc00011101nnnndddd110010011111` | translated | UnpredictableInstruction | `synchronization.cpp` |
| 28 | `arm_LDAEXB` | LDAEXB | `cccc00011101nnnndddd111010011111` | translated | UnpredictableInstruction | `synchronization.cpp` |
| 29 | `arm_LDREXB` | LDREXB | `cccc00011101nnnndddd111110011111` | translated | UnpredictableInstruction | `synchronization.cpp` |
| 30 | `arm_STLH` | STLH | `cccc00011110nnnn111111001001mmmm` | translated | UnpredictableInstruction | `synchronization.cpp` |
| 31 | `arm_LDAH` | LDAH | `cccc00011111nnnndddd110010011111` | translated | UnpredictableInstruction | `synchronization.cpp` |
| 32 | `arm_LDAEXH` | LDAEXH | `cccc00011111nnnndddd111010011111` | translated | UnpredictableInstruction | `synchronization.cpp` |
| 33 | `arm_LDREXH` | LDREXH | `cccc00011111nnnndddd111110011111` | translated | UnpredictableInstruction | `synchronization.cpp` |
| 34 | `arm_CLZ` | CLZ | `cccc000101101111dddd11110001mmmm` | translated | UnpredictableInstruction | `misc.cpp` |
| 35 | `arm_RBIT` | RBIT | `cccc011011111111dddd11110011mmmm` | translated | UnpredictableInstruction | `reversal.cpp` |
| 36 | `arm_REV` | REV | `cccc011010111111dddd11110011mmmm` | translated | UnpredictableInstruction | `reversal.cpp` |
| 37 | `arm_REV16` | REV16 | `cccc011010111111dddd11111011mmmm` | translated | UnpredictableInstruction | `reversal.cpp` |
| 38 | `arm_REVSH` | REVSH | `cccc011011111111dddd11111011mmmm` | translated | UnpredictableInstruction | `reversal.cpp` |
| 39 | `arm_MSR_reg` | MSR (reg) | `cccc00010010mmmm111100000000nnnn` | translated | UnpredictableInstruction | `status_register_access.cpp` |
| 40 | `arm_SXTB` | SXTB | `cccc011010101111ddddrr000111mmmm` | translated | UnpredictableInstruction | `extension.cpp` |
| 41 | `arm_SXTB16` | SXTB16 | `cccc011010001111ddddrr000111mmmm` | translated | UnpredictableInstruction | `extension.cpp` |
| 42 | `arm_SXTH` | SXTH | `cccc011010111111ddddrr000111mmmm` | translated | UnpredictableInstruction | `extension.cpp` |
| 43 | `arm_UXTB` | UXTB | `cccc011011101111ddddrr000111mmmm` | translated | UnpredictableInstruction | `extension.cpp` |
| 44 | `arm_UXTB16` | UXTB16 | `cccc011011001111ddddrr000111mmmm` | translated | UnpredictableInstruction | `extension.cpp` |
| 45 | `arm_UXTH` | UXTH | `cccc011011111111ddddrr000111mmmm` | translated | UnpredictableInstruction | `extension.cpp` |
| 46 | `arm_UDF` | UDF | `111001111111------------1111----` | architectural undefined | UndefinedInstruction | `a32_exception_generating.cpp` |
| 47 | `arm_NOP` | Reserved Hint | `----0011001000001111------------` | translated |  | `a32_translate_impl.cpp` |
| 48 | `arm_SWP` | SWP | `cccc00010000nnnntttt00001001uuuu` | translated | UnpredictableInstruction | `synchronization.cpp` |
| 49 | `arm_SWPB` | SWPB | `cccc00010100nnnntttt00001001uuuu` | translated | UnpredictableInstruction | `synchronization.cpp` |
| 50 | `arm_STLEX` | STLEX | `cccc00011000nnnndddd11101001tttt` | translated | UnpredictableInstruction | `synchronization.cpp` |
| 51 | `arm_STREX` | STREX | `cccc00011000nnnndddd11111001mmmm` | translated | UnpredictableInstruction | `synchronization.cpp` |
| 52 | `arm_STLEXD` | STLEXD | `cccc00011010nnnndddd11101001mmmm` | translated | UnpredictableInstruction | `synchronization.cpp` |
| 53 | `arm_STREXD` | STREXD | `cccc00011010nnnndddd11111001mmmm` | translated | UnpredictableInstruction | `synchronization.cpp` |
| 54 | `arm_STLEXB` | STLEXB | `cccc00011100nnnndddd11101001mmmm` | translated | UnpredictableInstruction | `synchronization.cpp` |
| 55 | `arm_STREXB` | STREXB | `cccc00011100nnnndddd11111001mmmm` | translated | UnpredictableInstruction | `synchronization.cpp` |
| 56 | `arm_STLEXH` | STLEXH | `cccc00011110nnnndddd11101001mmmm` | translated | UnpredictableInstruction | `synchronization.cpp` |
| 57 | `arm_STREXH` | STREXH | `cccc00011110nnnndddd11111001mmmm` | translated | UnpredictableInstruction | `synchronization.cpp` |
| 58 | `arm_SEL` | SEL | `cccc01101000nnnndddd11111011mmmm` | translated | UnpredictableInstruction | `misc.cpp` |
| 59 | `arm_USAD8` | USAD8 | `cccc01111000dddd1111mmmm0001nnnn` | translated | UnpredictableInstruction | `parallel.cpp` |
| 60 | `arm_SSAT16` | SSAT16 | `cccc01101010vvvvdddd11110011nnnn` | translated | UnpredictableInstruction | `saturated.cpp` |
| 61 | `arm_USAT16` | USAT16 | `cccc01101110vvvvdddd11110011nnnn` | translated | UnpredictableInstruction | `saturated.cpp` |
| 62 | `arm_SDIV` | SDIV | `cccc01110001dddd1111mmmm0001nnnn` | translated |  | `divide.cpp` |
| 63 | `arm_UDIV` | UDIV | `cccc01110011dddd1111mmmm0001nnnn` | translated |  | `divide.cpp` |
| 64 | `arm_SADD8` | SADD8 | `cccc01100001nnnndddd11111001mmmm` | translated | UnpredictableInstruction | `parallel.cpp` |
| 65 | `arm_SADD16` | SADD16 | `cccc01100001nnnndddd11110001mmmm` | translated | UnpredictableInstruction | `parallel.cpp` |
| 66 | `arm_SASX` | SASX | `cccc01100001nnnndddd11110011mmmm` | translated | UnpredictableInstruction | `parallel.cpp` |
| 67 | `arm_SSAX` | SSAX | `cccc01100001nnnndddd11110101mmmm` | translated | UnpredictableInstruction | `parallel.cpp` |
| 68 | `arm_SSUB8` | SSUB8 | `cccc01100001nnnndddd11111111mmmm` | translated | UnpredictableInstruction | `parallel.cpp` |
| 69 | `arm_SSUB16` | SSUB16 | `cccc01100001nnnndddd11110111mmmm` | translated | UnpredictableInstruction | `parallel.cpp` |
| 70 | `arm_UADD8` | UADD8 | `cccc01100101nnnndddd11111001mmmm` | translated | UnpredictableInstruction | `parallel.cpp` |
| 71 | `arm_UADD16` | UADD16 | `cccc01100101nnnndddd11110001mmmm` | translated | UnpredictableInstruction | `parallel.cpp` |
| 72 | `arm_UASX` | UASX | `cccc01100101nnnndddd11110011mmmm` | translated | UnpredictableInstruction | `parallel.cpp` |
| 73 | `arm_USAX` | USAX | `cccc01100101nnnndddd11110101mmmm` | translated | UnpredictableInstruction | `parallel.cpp` |
| 74 | `arm_USUB8` | USUB8 | `cccc01100101nnnndddd11111111mmmm` | translated | UnpredictableInstruction | `parallel.cpp` |
| 75 | `arm_USUB16` | USUB16 | `cccc01100101nnnndddd11110111mmmm` | translated | UnpredictableInstruction | `parallel.cpp` |
| 76 | `arm_QADD8` | QADD8 | `cccc01100010nnnndddd11111001mmmm` | translated | UnpredictableInstruction | `parallel.cpp` |
| 77 | `arm_QADD16` | QADD16 | `cccc01100010nnnndddd11110001mmmm` | translated | UnpredictableInstruction | `parallel.cpp` |
| 78 | `arm_QASX` | QASX | `cccc01100010nnnndddd11110011mmmm` | translated | UnpredictableInstruction | `saturated.cpp` |
| 79 | `arm_QSAX` | QSAX | `cccc01100010nnnndddd11110101mmmm` | translated | UnpredictableInstruction | `saturated.cpp` |
| 80 | `arm_QSUB8` | QSUB8 | `cccc01100010nnnndddd11111111mmmm` | translated | UnpredictableInstruction | `parallel.cpp` |
| 81 | `arm_QSUB16` | QSUB16 | `cccc01100010nnnndddd11110111mmmm` | translated | UnpredictableInstruction | `parallel.cpp` |
| 82 | `arm_UQADD8` | UQADD8 | `cccc01100110nnnndddd11111001mmmm` | translated | UnpredictableInstruction | `parallel.cpp` |
| 83 | `arm_UQADD16` | UQADD16 | `cccc01100110nnnndddd11110001mmmm` | translated | UnpredictableInstruction | `parallel.cpp` |
| 84 | `arm_UQASX` | UQASX | `cccc01100110nnnndddd11110011mmmm` | translated | UnpredictableInstruction | `saturated.cpp` |
| 85 | `arm_UQSAX` | UQSAX | `cccc01100110nnnndddd11110101mmmm` | translated | UnpredictableInstruction | `saturated.cpp` |
| 86 | `arm_UQSUB8` | UQSUB8 | `cccc01100110nnnndddd11111111mmmm` | translated | UnpredictableInstruction | `parallel.cpp` |
| 87 | `arm_UQSUB16` | UQSUB16 | `cccc01100110nnnndddd11110111mmmm` | translated | UnpredictableInstruction | `parallel.cpp` |
| 88 | `arm_SHADD8` | SHADD8 | `cccc01100011nnnndddd11111001mmmm` | translated | UnpredictableInstruction | `parallel.cpp` |
| 89 | `arm_SHADD16` | SHADD16 | `cccc01100011nnnndddd11110001mmmm` | translated | UnpredictableInstruction | `parallel.cpp` |
| 90 | `arm_SHASX` | SHASX | `cccc01100011nnnndddd11110011mmmm` | translated | UnpredictableInstruction | `parallel.cpp` |
| 91 | `arm_SHSAX` | SHSAX | `cccc01100011nnnndddd11110101mmmm` | translated | UnpredictableInstruction | `parallel.cpp` |
| 92 | `arm_SHSUB8` | SHSUB8 | `cccc01100011nnnndddd11111111mmmm` | translated | UnpredictableInstruction | `parallel.cpp` |
| 93 | `arm_SHSUB16` | SHSUB16 | `cccc01100011nnnndddd11110111mmmm` | translated | UnpredictableInstruction | `parallel.cpp` |
| 94 | `arm_UHADD8` | UHADD8 | `cccc01100111nnnndddd11111001mmmm` | translated | UnpredictableInstruction | `parallel.cpp` |
| 95 | `arm_UHADD16` | UHADD16 | `cccc01100111nnnndddd11110001mmmm` | translated | UnpredictableInstruction | `parallel.cpp` |
| 96 | `arm_UHASX` | UHASX | `cccc01100111nnnndddd11110011mmmm` | translated | UnpredictableInstruction | `parallel.cpp` |
| 97 | `arm_UHSAX` | UHSAX | `cccc01100111nnnndddd11110101mmmm` | translated | UnpredictableInstruction | `parallel.cpp` |
| 98 | `arm_UHSUB8` | UHSUB8 | `cccc01100111nnnndddd11111111mmmm` | translated | UnpredictableInstruction | `parallel.cpp` |
| 99 | `arm_UHSUB16` | UHSUB16 | `cccc01100111nnnndddd11110111mmmm` | translated | UnpredictableInstruction | `parallel.cpp` |
| 100 | `arm_QADD` | QADD | `cccc00010000nnnndddd00000101mmmm` | translated | UnpredictableInstruction | `saturated.cpp` |
| 101 | `arm_QSUB` | QSUB | `cccc00010010nnnndddd00000101mmmm` | translated | UnpredictableInstruction | `saturated.cpp` |
| 102 | `arm_QDADD` | QDADD | `cccc00010100nnnndddd00000101mmmm` | translated | UnpredictableInstruction | `saturated.cpp` |
| 103 | `arm_QDSUB` | QDSUB | `cccc00010110nnnndddd00000101mmmm` | translated | UnpredictableInstruction | `saturated.cpp` |
| 104 | `arm_PLD_reg` | PLD (reg) | `11110111uz01nnnn1111iiiiitt0mmmm` | translated | RaiseException | `hint.cpp` |
| 105 | `arm_LDRHT_imm` | LDRHT (A1) | `cccc0000U111nnnnttttiiii1011jjjj` | translated | UnpredictableInstruction | `load_store.cpp` |
| 106 | `arm_LDRHT_reg` | LDRHT (A2) | `cccc0000U011nnnntttt00001011mmmm` | translated | UnpredictableInstruction | `load_store.cpp` |
| 107 | `arm_LDRSBT_reg` | LDRSBT (A2) | `cccc0000U011nnnntttt00001101mmmm` | translated | UnpredictableInstruction | `load_store.cpp` |
| 108 | `arm_LDRSHT_reg` | LDRSHT (A2) | `cccc0000U011nnnntttt00001111mmmm` | translated | UnpredictableInstruction | `load_store.cpp` |
| 109 | `arm_STRHT_reg` | STRHT (A2) | `cccc0000U010nnnntttt00001011mmmm` | translated | UnpredictableInstruction | `load_store.cpp` |
| 110 | `arm_LDRD_lit` | LDRD (lit) | `cccc0001u1001111ttttvvvv1101vvvv` | translated | UnpredictableInstruction | `load_store.cpp` |
| 111 | `arm_LDRSB_lit` | LDRSB (lit) | `cccc0001u1011111ttttvvvv1101vvvv` | translated | UnpredictableInstruction | `load_store.cpp` |
| 112 | `arm_LDRSH_lit` | LDRSH (lit) | `cccc0001u1011111ttttvvvv1111vvvv` | translated | UnpredictableInstruction | `load_store.cpp` |
| 113 | `arm_MUL` | MUL | `cccc0000000Sdddd0000mmmm1001nnnn` | translated | UnpredictableInstruction | `multiply.cpp` |
| 114 | `arm_SMULWy` | SMULWY | `cccc00010010dddd0000mmmm1y10nnnn` | translated | UnpredictableInstruction | `multiply.cpp` |
| 115 | `arm_SMMUL` | SMMUL | `cccc01110101dddd1111mmmm00R1nnnn` | translated | UnpredictableInstruction | `multiply.cpp` |
| 116 | `arm_SMUAD` | SMUAD | `cccc01110000dddd1111mmmm00M1nnnn` | translated | UnpredictableInstruction | `multiply.cpp` |
| 117 | `arm_SMUSD` | SMUSD | `cccc01110000dddd1111mmmm01M1nnnn` | translated | UnpredictableInstruction | `multiply.cpp` |
| 118 | `arm_CRC32` | CRC32 | `cccc00010zz0nnnndddd00000100mmmm` | translated |  | `a32_crc32.cpp` |
| 119 | `arm_CRC32C` | CRC32C | `cccc00010zz0nnnndddd00100100mmmm` | translated |  | `a32_crc32.cpp` |
| 120 | `arm_CMN_rsr` | CMN (rsr) | `cccc00010111nnnn0000ssss0rr1mmmm` | translated | UnpredictableInstruction | `data_processing.cpp` |
| 121 | `arm_CMP_rsr` | CMP (rsr) | `cccc00010101nnnn0000ssss0rr1mmmm` | translated | UnpredictableInstruction | `data_processing.cpp` |
| 122 | `arm_TEQ_rsr` | TEQ (rsr) | `cccc00010011nnnn0000ssss0rr1mmmm` | translated | UnpredictableInstruction | `data_processing.cpp` |
| 123 | `arm_TST_rsr` | TST (rsr) | `cccc00010001nnnn0000ssss0rr1mmmm` | translated | UnpredictableInstruction | `data_processing.cpp` |
| 124 | `arm_SXTAB` | SXTAB | `cccc01101010nnnnddddrr000111mmmm` | translated | UnpredictableInstruction | `extension.cpp` |
| 125 | `arm_SXTAB16` | SXTAB16 | `cccc01101000nnnnddddrr000111mmmm` | translated | UnpredictableInstruction | `extension.cpp` |
| 126 | `arm_SXTAH` | SXTAH | `cccc01101011nnnnddddrr000111mmmm` | translated | UnpredictableInstruction | `extension.cpp` |
| 127 | `arm_UXTAB` | UXTAB | `cccc01101110nnnnddddrr000111mmmm` | translated | UnpredictableInstruction | `extension.cpp` |
| 128 | `arm_UXTAB16` | UXTAB16 | `cccc01101100nnnnddddrr000111mmmm` | translated | UnpredictableInstruction | `extension.cpp` |
| 129 | `arm_UXTAH` | UXTAH | `cccc01101111nnnnddddrr000111mmmm` | translated | UnpredictableInstruction | `extension.cpp` |
| 130 | `arm_PLD_imm` | PLD (imm) | `11110101uz01nnnn1111iiiiiiiiiiii` | translated | RaiseException | `hint.cpp` |
| 131 | `arm_BFC` | BFC | `cccc0111110vvvvvddddvvvvv0011111` | translated | UnpredictableInstruction | `misc.cpp` |
| 132 | `arm_SMULxy` | SMULXY | `cccc00010110dddd0000mmmm1xy0nnnn` | translated | UnpredictableInstruction | `multiply.cpp` |
| 133 | `arm_CMN_reg` | CMN (reg) | `cccc00010111nnnn0000vvvvvrr0mmmm` | translated |  | `data_processing.cpp` |
| 134 | `arm_CMP_reg` | CMP (reg) | `cccc00010101nnnn0000vvvvvrr0mmmm` | translated |  | `data_processing.cpp` |
| 135 | `arm_MOV_rsr` | MOV (rsr) | `cccc0001101S0000ddddssss0rr1mmmm` | translated | UnpredictableInstruction | `data_processing.cpp` |
| 136 | `arm_MVN_rsr` | MVN (rsr) | `cccc0001111S0000ddddssss0rr1mmmm` | translated | UnpredictableInstruction | `data_processing.cpp` |
| 137 | `arm_TEQ_reg` | TEQ (reg) | `cccc00010011nnnn0000vvvvvrr0mmmm` | translated |  | `data_processing.cpp` |
| 138 | `arm_TST_reg` | TST (reg) | `cccc00010001nnnn0000vvvvvrr0mmmm` | translated |  | `data_processing.cpp` |
| 139 | `arm_LDRD_reg` | LDRD (reg) | `cccc000pu0w0nnnntttt00001101mmmm` | translated | UnpredictableInstruction | `load_store.cpp` |
| 140 | `arm_LDRH_lit` | LDRH (lit) | `cccc000pu1w11111ttttvvvv1011vvvv` | translated | UnpredictableInstruction | `load_store.cpp` |
| 141 | `arm_LDRH_reg` | LDRH (reg) | `cccc000pu0w1nnnntttt00001011mmmm` | translated | UnpredictableInstruction | `load_store.cpp` |
| 142 | `arm_LDRSB_reg` | LDRSB (reg) | `cccc000pu0w1nnnntttt00001101mmmm` | translated | UnpredictableInstruction | `load_store.cpp` |
| 143 | `arm_LDRSH_reg` | LDRSH (reg) | `cccc000pu0w1nnnntttt00001111mmmm` | translated | UnpredictableInstruction | `load_store.cpp` |
| 144 | `arm_STRD_reg` | STRD (reg) | `cccc000pu0w0nnnntttt00001111mmmm` | translated | UnpredictableInstruction | `load_store.cpp` |
| 145 | `arm_STRH_reg` | STRH (reg) | `cccc000pu0w0nnnntttt00001011mmmm` | translated | UnpredictableInstruction | `load_store.cpp` |
| 146 | `arm_CMN_imm` | CMN (imm) | `cccc00110111nnnn0000rrrrvvvvvvvv` | translated |  | `data_processing.cpp` |
| 147 | `arm_CMP_imm` | CMP (imm) | `cccc00110101nnnn0000rrrrvvvvvvvv` | translated |  | `data_processing.cpp` |
| 148 | `arm_MOV_reg` | MOV (reg) | `cccc0001101S0000ddddvvvvvrr0mmmm` | translated | UnpredictableInstruction | `data_processing.cpp` |
| 149 | `arm_MVN_reg` | MVN (reg) | `cccc0001111S0000ddddvvvvvrr0mmmm` | translated | UnpredictableInstruction | `data_processing.cpp` |
| 150 | `arm_TEQ_imm` | TEQ (imm) | `cccc00110011nnnn0000rrrrvvvvvvvv` | translated |  | `data_processing.cpp` |
| 151 | `arm_TST_imm` | TST (imm) | `cccc00110001nnnn0000rrrrvvvvvvvv` | translated |  | `data_processing.cpp` |
| 152 | `arm_BKPT` | BKPT | `cccc00010010vvvvvvvvvvvv0111vvvv` | translated | RaiseException, UnpredictableInstruction | `a32_exception_generating.cpp` |
| 153 | `arm_USADA8` | USADA8 | `cccc01111000ddddaaaammmm0001nnnn` | translated | UnpredictableInstruction | `parallel.cpp` |
| 154 | `arm_MLS` | MLS | `cccc00000110ddddaaaammmm1001nnnn` | translated | UnpredictableInstruction | `multiply.cpp` |
| 155 | `arm_UMAAL` | UMAAL | `cccc00000100ddddaaaammmm1001nnnn` | translated | UnpredictableInstruction | `multiply.cpp` |
| 156 | `arm_MSR_imm` | MSR (imm) | `cccc00110010mmmm1111rrrrvvvvvvvv` | translated |  | `status_register_access.cpp` |
| 157 | `arm_MOV_imm` | MOV (imm) | `cccc0011101S0000ddddrrrrvvvvvvvv` | translated | UnpredictableInstruction | `data_processing.cpp` |
| 158 | `arm_MVN_imm` | MVN (imm) | `cccc0011111S0000ddddrrrrvvvvvvvv` | translated | UnpredictableInstruction | `data_processing.cpp` |
| 159 | `arm_LDRHT_imm` | LDRHT (A1) | `cccc0000U111nnnnttttiiii1011jjjj` | translated | UnpredictableInstruction | `load_store.cpp` |
| 160 | `arm_LDRSBT_imm` | LDRSBT (A1) | `cccc0000U111nnnnttttiiii1101jjjj` | translated | UnpredictableInstruction | `load_store.cpp` |
| 161 | `arm_LDRSHT_imm` | LDRSHT (A1) | `cccc0000U111nnnnttttiiii1111jjjj` | translated | UnpredictableInstruction | `load_store.cpp` |
| 162 | `arm_STRHT_imm` | STRHT (A1) | `cccc0000U110nnnnttttiiii1011jjjj` | translated | UnpredictableInstruction | `load_store.cpp` |
| 163 | `arm_LDR_lit` | LDR (lit) | `cccc0101u0011111ttttvvvvvvvvvvvv` | translated |  | `load_store.cpp` |
| 164 | `arm_LDRB_lit` | LDRB (lit) | `cccc0101u1011111ttttvvvvvvvvvvvv` | translated | UnpredictableInstruction | `load_store.cpp` |
| 165 | `arm_PKHBT` | PKHBT | `cccc01101000nnnnddddvvvvv001mmmm` | translated | UnpredictableInstruction | `packing.cpp` |
| 166 | `arm_PKHTB` | PKHTB | `cccc01101000nnnnddddvvvvv101mmmm` | translated | UnpredictableInstruction | `packing.cpp` |
| 167 | `arm_MLA` | MLA | `cccc0000001Sddddaaaammmm1001nnnn` | translated | UnpredictableInstruction | `multiply.cpp` |
| 168 | `arm_SMLAL` | SMLAL | `cccc0000111Sddddaaaammmm1001nnnn` | translated | UnpredictableInstruction | `multiply.cpp` |
| 169 | `arm_SMULL` | SMULL | `cccc0000110Sddddaaaammmm1001nnnn` | translated | UnpredictableInstruction | `multiply.cpp` |
| 170 | `arm_UMLAL` | UMLAL | `cccc0000101Sddddaaaammmm1001nnnn` | translated | UnpredictableInstruction | `multiply.cpp` |
| 171 | `arm_UMULL` | UMULL | `cccc0000100Sddddaaaammmm1001nnnn` | translated | UnpredictableInstruction | `multiply.cpp` |
| 172 | `arm_SMLAWy` | SMLAWY | `cccc00010010ddddaaaammmm1y00nnnn` | translated | UnpredictableInstruction | `multiply.cpp` |
| 173 | `arm_SMMLA` | SMMLA | `cccc01110101ddddaaaammmm00R1nnnn` | translated | UnpredictableInstruction | `multiply.cpp` |
| 174 | `arm_SMMLS` | SMMLS | `cccc01110101ddddaaaammmm11R1nnnn` | translated | UnpredictableInstruction | `multiply.cpp` |
| 175 | `arm_SMLAD` | SMLAD | `cccc01110000ddddaaaammmm00M1nnnn` | translated | UnpredictableInstruction | `multiply.cpp` |
| 176 | `arm_SMLALD` | SMLALD | `cccc01110100ddddaaaammmm00M1nnnn` | translated | UnpredictableInstruction | `multiply.cpp` |
| 177 | `arm_SMLSD` | SMLSD | `cccc01110000ddddaaaammmm01M1nnnn` | translated | UnpredictableInstruction | `multiply.cpp` |
| 178 | `arm_SMLSLD` | SMLSLD | `cccc01110100ddddaaaammmm01M1nnnn` | translated | UnpredictableInstruction | `multiply.cpp` |
| 179 | `arm_BFI` | BFI | `cccc0111110vvvvvddddvvvvv001nnnn` | translated | UnpredictableInstruction | `misc.cpp` |
| 180 | `arm_SBFX` | SBFX | `cccc0111101wwwwwddddvvvvv101nnnn` | translated | UnpredictableInstruction | `misc.cpp` |
| 181 | `arm_UBFX` | UBFX | `cccc0111111wwwwwddddvvvvv101nnnn` | translated | UnpredictableInstruction | `misc.cpp` |
| 182 | `arm_SMLALxy` | SMLALXY | `cccc00010100ddddaaaammmm1xy0nnnn` | translated | UnpredictableInstruction | `multiply.cpp` |
| 183 | `arm_SMLAxy` | SMLAXY | `cccc00010000ddddaaaammmm1xy0nnnn` | translated | UnpredictableInstruction | `multiply.cpp` |
| 184 | `arm_ADC_rsr` | ADC (rsr) | `cccc0000101Snnnnddddssss0rr1mmmm` | translated | UnpredictableInstruction | `data_processing.cpp` |
| 185 | `arm_ADD_rsr` | ADD (rsr) | `cccc0000100Snnnnddddssss0rr1mmmm` | translated | UnpredictableInstruction | `data_processing.cpp` |
| 186 | `arm_AND_rsr` | AND (rsr) | `cccc0000000Snnnnddddssss0rr1mmmm` | translated | UnpredictableInstruction | `data_processing.cpp` |
| 187 | `arm_BIC_rsr` | BIC (rsr) | `cccc0001110Snnnnddddssss0rr1mmmm` | translated | UnpredictableInstruction | `data_processing.cpp` |
| 188 | `arm_EOR_rsr` | EOR (rsr) | `cccc0000001Snnnnddddssss0rr1mmmm` | translated | UnpredictableInstruction | `data_processing.cpp` |
| 189 | `arm_ORR_rsr` | ORR (rsr) | `cccc0001100Snnnnddddssss0rr1mmmm` | translated | UnpredictableInstruction | `data_processing.cpp` |
| 190 | `arm_RSB_rsr` | RSB (rsr) | `cccc0000011Snnnnddddssss0rr1mmmm` | translated | UnpredictableInstruction | `data_processing.cpp` |
| 191 | `arm_RSC_rsr` | RSC (rsr) | `cccc0000111Snnnnddddssss0rr1mmmm` | translated | UnpredictableInstruction | `data_processing.cpp` |
| 192 | `arm_SBC_rsr` | SBC (rsr) | `cccc0000110Snnnnddddssss0rr1mmmm` | translated | UnpredictableInstruction | `data_processing.cpp` |
| 193 | `arm_SUB_rsr` | SUB (rsr) | `cccc0000010Snnnnddddssss0rr1mmmm` | translated | UnpredictableInstruction | `data_processing.cpp` |
| 194 | `arm_LDRD_imm` | LDRD (imm) | `cccc000pu1w0nnnnttttvvvv1101vvvv` | translated | UnpredictableInstruction | `load_store.cpp` |
| 195 | `arm_LDRH_imm` | LDRH (imm) | `cccc000pu1w1nnnnttttvvvv1011vvvv` | translated | UnpredictableInstruction | `load_store.cpp` |
| 196 | `arm_LDRSB_imm` | LDRSB (imm) | `cccc000pu1w1nnnnttttvvvv1101vvvv` | translated | UnpredictableInstruction | `load_store.cpp` |
| 197 | `arm_LDRSH_imm` | LDRSH (imm) | `cccc000pu1w1nnnnttttvvvv1111vvvv` | translated | UnpredictableInstruction | `load_store.cpp` |
| 198 | `arm_STRD_imm` | STRD (imm) | `cccc000pu1w0nnnnttttvvvv1111vvvv` | translated | UnpredictableInstruction | `load_store.cpp` |
| 199 | `arm_STRH_imm` | STRH (imm) | `cccc000pu1w0nnnnttttvvvv1011vvvv` | translated | UnpredictableInstruction | `load_store.cpp` |
| 200 | `arm_SSAT` | SSAT | `cccc0110101vvvvvddddvvvvvr01nnnn` | translated | UnpredictableInstruction | `saturated.cpp` |
| 201 | `arm_USAT` | USAT | `cccc0110111vvvvvddddvvvvvr01nnnn` | translated | UnpredictableInstruction | `saturated.cpp` |
| 202 | `arm_MCRR` | MCRR | `cccc11000100uuuuttttppppooooMMMM` | translated | UnpredictableInstruction | `coprocessor.cpp` |
| 203 | `arm_MRRC` | MRRC | `cccc11000101uuuuttttppppooooMMMM` | translated | UnpredictableInstruction | `coprocessor.cpp` |
| 204 | `arm_ADC_reg` | ADC (reg) | `cccc0000101Snnnnddddvvvvvrr0mmmm` | translated | UnpredictableInstruction | `data_processing.cpp` |
| 205 | `arm_ADD_reg` | ADD (reg) | `cccc0000100Snnnnddddvvvvvrr0mmmm` | translated | UnpredictableInstruction | `data_processing.cpp` |
| 206 | `arm_AND_reg` | AND (reg) | `cccc0000000Snnnnddddvvvvvrr0mmmm` | translated | UnpredictableInstruction | `data_processing.cpp` |
| 207 | `arm_BIC_reg` | BIC (reg) | `cccc0001110Snnnnddddvvvvvrr0mmmm` | translated | UnpredictableInstruction | `data_processing.cpp` |
| 208 | `arm_EOR_reg` | EOR (reg) | `cccc0000001Snnnnddddvvvvvrr0mmmm` | translated | UnpredictableInstruction | `data_processing.cpp` |
| 209 | `arm_ORR_reg` | ORR (reg) | `cccc0001100Snnnnddddvvvvvrr0mmmm` | translated | UnpredictableInstruction | `data_processing.cpp` |
| 210 | `arm_RSB_reg` | RSB (reg) | `cccc0000011Snnnnddddvvvvvrr0mmmm` | translated | UnpredictableInstruction | `data_processing.cpp` |
| 211 | `arm_RSC_reg` | RSC (reg) | `cccc0000111Snnnnddddvvvvvrr0mmmm` | translated | UnpredictableInstruction | `data_processing.cpp` |
| 212 | `arm_SBC_reg` | SBC (reg) | `cccc0000110Snnnnddddvvvvvrr0mmmm` | translated | UnpredictableInstruction | `data_processing.cpp` |
| 213 | `arm_SUB_reg` | SUB (reg) | `cccc0000010Snnnnddddvvvvvrr0mmmm` | translated | UnpredictableInstruction | `data_processing.cpp` |
| 214 | `arm_LDRBT_reg` | LDRBT (A2) | `cccc0110U111nnnnttttiiiiiss0mmmm` | translated | UnpredictableInstruction | `load_store.cpp` |
| 215 | `arm_LDRT_reg` | LDRT (A2) | `cccc0110U011nnnnttttiiiiiss0mmmm` | translated | UnpredictableInstruction | `load_store.cpp` |
| 216 | `arm_STRBT_reg` | STRBT (A2) | `cccc0110U110nnnnttttiiiiiss0mmmm` | translated | UnpredictableInstruction | `load_store.cpp` |
| 217 | `arm_STRT_reg` | STRT (A2) | `cccc0110U010nnnnttttiiiiiss0mmmm` | translated | UnpredictableInstruction | `load_store.cpp` |
| 218 | `arm_MOVT` | MOVT | `cccc00110100vvvvddddvvvvvvvvvvvv` | translated | UnpredictableInstruction | `misc.cpp` |
| 219 | `arm_MOVW` | MOVW | `cccc00110000vvvvddddvvvvvvvvvvvv` | translated | UnpredictableInstruction | `misc.cpp` |
| 220 | `arm_BLX_imm` | BLX (imm) | `1111101hvvvvvvvvvvvvvvvvvvvvvvvv` | translated |  | `a32_branch.cpp` |
| 221 | `arm_ADC_imm` | ADC (imm) | `cccc0010101Snnnnddddrrrrvvvvvvvv` | translated | UnpredictableInstruction | `data_processing.cpp` |
| 222 | `arm_ADD_imm` | ADD (imm) | `cccc0010100Snnnnddddrrrrvvvvvvvv` | translated | UnpredictableInstruction | `data_processing.cpp` |
| 223 | `arm_AND_imm` | AND (imm) | `cccc0010000Snnnnddddrrrrvvvvvvvv` | translated | UnpredictableInstruction | `data_processing.cpp` |
| 224 | `arm_BIC_imm` | BIC (imm) | `cccc0011110Snnnnddddrrrrvvvvvvvv` | translated | UnpredictableInstruction | `data_processing.cpp` |
| 225 | `arm_EOR_imm` | EOR (imm) | `cccc0010001Snnnnddddrrrrvvvvvvvv` | translated | UnpredictableInstruction | `data_processing.cpp` |
| 226 | `arm_ORR_imm` | ORR (imm) | `cccc0011100Snnnnddddrrrrvvvvvvvv` | translated | UnpredictableInstruction | `data_processing.cpp` |
| 227 | `arm_RSB_imm` | RSB (imm) | `cccc0010011Snnnnddddrrrrvvvvvvvv` | translated | UnpredictableInstruction | `data_processing.cpp` |
| 228 | `arm_RSC_imm` | RSC (imm) | `cccc0010111Snnnnddddrrrrvvvvvvvv` | translated | UnpredictableInstruction | `data_processing.cpp` |
| 229 | `arm_SBC_imm` | SBC (imm) | `cccc0010110Snnnnddddrrrrvvvvvvvv` | translated | UnpredictableInstruction | `data_processing.cpp` |
| 230 | `arm_SUB_imm` | SUB (imm) | `cccc0010010Snnnnddddrrrrvvvvvvvv` | translated | UnpredictableInstruction | `data_processing.cpp` |
| 231 | `arm_LDRBT_imm` | LDRBT (A1) | `cccc0100U111nnnnttttiiiiiiiiiiii` | translated | UnpredictableInstruction | `load_store.cpp` |
| 232 | `arm_LDRT_imm` | LDRT (A1) | `cccc0100U011nnnnttttiiiiiiiiiiii` | translated | UnpredictableInstruction | `load_store.cpp` |
| 233 | `arm_STRBT_imm` | STRBT (A1) | `cccc0100U110nnnnttttiiiiiiiiiiii` | translated | UnpredictableInstruction | `load_store.cpp` |
| 234 | `arm_STRT_imm` | STRT (A1) | `cccc0100U010nnnnttttiiiiiiiiiiii` | translated | UnpredictableInstruction | `load_store.cpp` |
| 235 | `arm_LDM` | LDM | `cccc100010w1nnnnxxxxxxxxxxxxxxxx` | translated | UnpredictableInstruction | `load_store.cpp` |
| 236 | `arm_LDMDA` | LDMDA | `cccc100000w1nnnnxxxxxxxxxxxxxxxx` | translated | UnpredictableInstruction | `load_store.cpp` |
| 237 | `arm_LDMDB` | LDMDB | `cccc100100w1nnnnxxxxxxxxxxxxxxxx` | translated | UnpredictableInstruction | `load_store.cpp` |
| 238 | `arm_LDMIB` | LDMIB | `cccc100110w1nnnnxxxxxxxxxxxxxxxx` | translated | UnpredictableInstruction | `load_store.cpp` |
| 239 | `arm_STM` | STM | `cccc100010w0nnnnxxxxxxxxxxxxxxxx` | translated | UnpredictableInstruction | `load_store.cpp` |
| 240 | `arm_STMDA` | STMDA | `cccc100000w0nnnnxxxxxxxxxxxxxxxx` | translated | UnpredictableInstruction | `load_store.cpp` |
| 241 | `arm_STMDB` | STMDB | `cccc100100w0nnnnxxxxxxxxxxxxxxxx` | translated | UnpredictableInstruction | `load_store.cpp` |
| 242 | `arm_STMIB` | STMIB | `cccc100110w0nnnnxxxxxxxxxxxxxxxx` | translated | UnpredictableInstruction | `load_store.cpp` |
| 243 | `arm_MCR` | MCR | `cccc1110ooo0NNNNttttppppooo1MMMM` | translated | UnpredictableInstruction | `coprocessor.cpp` |
| 244 | `arm_MRC` | MRC | `cccc1110ooo1NNNNttttppppooo1MMMM` | translated |  | `coprocessor.cpp` |
| 245 | `arm_LDR_reg` | LDR (reg) | `cccc011pu0w1nnnnttttvvvvvrr0mmmm` | translated | UnpredictableInstruction | `load_store.cpp` |
| 246 | `arm_LDRB_reg` | LDRB (reg) | `cccc011pu1w1nnnnttttvvvvvrr0mmmm` | translated | UnpredictableInstruction | `load_store.cpp` |
| 247 | `arm_STR_reg` | STR (reg) | `cccc011pu0w0nnnnttttvvvvvrr0mmmm` | translated | UnpredictableInstruction | `load_store.cpp` |
| 248 | `arm_STRB_reg` | STRB (reg) | `cccc011pu1w0nnnnttttvvvvvrr0mmmm` | translated | UnpredictableInstruction | `load_store.cpp` |
| 249 | `arm_LDM_usr` | LDM (usr reg) | `----100--101--------------------` | unreachable stub | UNREACHABLE | `load_store.cpp` |
| 250 | `arm_LDM_eret` | LDM (exce ret) | `----100--1-1----1---------------` | unreachable stub | UNREACHABLE | `load_store.cpp` |
| 251 | `arm_STM_usr` | STM (usr reg) | `----100--100--------------------` | unreachable stub | UNREACHABLE | `load_store.cpp` |
| 252 | `arm_CDP` | CDP | `cccc1110ooooNNNNDDDDppppooo0MMMM` | translated |  | `coprocessor.cpp` |
| 253 | `arm_LDR_imm` | LDR (imm) | `cccc010pu0w1nnnnttttvvvvvvvvvvvv` | translated | UnpredictableInstruction | `load_store.cpp` |
| 254 | `arm_LDRB_imm` | LDRB (imm) | `cccc010pu1w1nnnnttttvvvvvvvvvvvv` | translated | UnpredictableInstruction | `load_store.cpp` |
| 255 | `arm_STR_imm` | STR (imm) | `cccc010pu0w0nnnnttttvvvvvvvvvvvv` | translated | UnpredictableInstruction | `load_store.cpp` |
| 256 | `arm_STRB_imm` | STRB (imm) | `cccc010pu1w0nnnnttttvvvvvvvvvvvv` | translated | UnpredictableInstruction | `load_store.cpp` |
| 257 | `arm_B` | B | `cccc1010vvvvvvvvvvvvvvvvvvvvvvvv` | translated |  | `a32_branch.cpp` |
| 258 | `arm_BL` | BL | `cccc1011vvvvvvvvvvvvvvvvvvvvvvvv` | translated |  | `a32_branch.cpp` |
| 259 | `arm_LDC` | LDC | `cccc110pudw1nnnnDDDDppppvvvvvvvv` | translated |  | `coprocessor.cpp` |
| 260 | `arm_STC` | STC | `cccc110pudw0nnnnDDDDppppvvvvvvvv` | translated | UnpredictableInstruction | `coprocessor.cpp` |
| 261 | `arm_SVC` | SVC | `cccc1111vvvvvvvvvvvvvvvvvvvvvvvv` | translated |  | `a32_exception_generating.cpp` |

### asimd

| Row | Handler | Name | Encoding | Status | Guards | Source |
|---:|---|---|---|---|---|---|
| 1 | `asimd_VMOV_imm` | VBIC, VMOV, VMVN, VORR (immediate) | `1111001a1D000bcdVVVVmmmm0Qo1efgh` | translated | UNREACHABLE, UndefinedInstruction | `asimd_one_reg_modified_immediate.cpp` |
| 2 | `asimd_VEXT` | VEXT | `111100101D11nnnnddddiiiiNQM0mmmm` | translated | UndefinedInstruction | `asimd_misc.cpp` |
| 3 | `asimd_VTBL` | VTBL | `111100111D11nnnndddd10zzN0M0mmmm` | translated |  | `asimd_misc.cpp` |
| 4 | `asimd_VTBX` | VTBX | `111100111D11nnnndddd10zzN1M0mmmm` | translated |  | `asimd_misc.cpp` |
| 5 | `asimd_VDUP_scalar` | VDUP (scalar) | `111100111D11iiiidddd11000QM0mmmm` | translated | UndefinedInstruction | `asimd_misc.cpp` |
| 6 | `asimd_VSWP` | VSWP | `111100111D110010dddd00000QM0mmmm` | translated | UndefinedInstruction | `asimd_two_regs_misc.cpp` |
| 7 | `asimd_VMOVN` | VMOVN | `111100111D11zz10dddd001000M0mmmm` | translated | UndefinedInstruction | `asimd_two_regs_misc.cpp` |
| 8 | `asimd_VQMOVUN` | VQMOVUN | `111100111D11zz10dddd001001M0mmmm` | translated | UndefinedInstruction | `asimd_two_regs_misc.cpp` |
| 9 | `asimd_VSHLL_max` | VSHLL_max | `111100111D11zz10dddd001100M0mmmm` | translated | UndefinedInstruction | `asimd_two_regs_misc.cpp` |
| 10 | `v8_AESE` | AESE | `111100111D11zz00dddd001100M0mmmm` | translated | UndefinedInstruction | `asimd_two_regs_misc.cpp` |
| 11 | `v8_AESD` | AESD | `111100111D11zz00dddd001101M0mmmm` | translated | UndefinedInstruction | `asimd_two_regs_misc.cpp` |
| 12 | `v8_AESMC` | AESMC | `111100111D11zz00dddd001110M0mmmm` | translated | UndefinedInstruction | `asimd_two_regs_misc.cpp` |
| 13 | `v8_AESIMC` | AESIMC | `111100111D11zz00dddd001111M0mmmm` | translated | UndefinedInstruction | `asimd_two_regs_misc.cpp` |
| 14 | `arm_UDF` | UNALLOCATED | `111100111-11--01----001010-0----` | architectural undefined | UndefinedInstruction | `a32_exception_generating.cpp` |
| 15 | `arm_UDF` | UNALLOCATED (SHA1H) | `111100111-11--01----001011-0----` | architectural undefined | UndefinedInstruction | `a32_exception_generating.cpp` |
| 16 | `arm_UDF` | UNALLOCATED (SHA1SU1) | `111100111-11--10----001110-0----` | architectural undefined | UndefinedInstruction | `a32_exception_generating.cpp` |
| 17 | `v8_SHA256SU0` | SHA256SU0 | `111100111D11zz10dddd001111M0mmmm` | translated | UndefinedInstruction | `asimd_two_regs_misc.cpp` |
| 18 | `asimd_VCLS` | VCLS | `111100111D11zz00dddd01000QM0mmmm` | translated | UndefinedInstruction | `asimd_two_regs_misc.cpp` |
| 19 | `asimd_VCLZ` | VCLZ | `111100111D11zz00dddd01001QM0mmmm` | translated | UndefinedInstruction | `asimd_two_regs_misc.cpp` |
| 20 | `asimd_VCNT` | VCNT | `111100111D11zz00dddd01010QM0mmmm` | translated | UndefinedInstruction | `asimd_two_regs_misc.cpp` |
| 21 | `asimd_VMVN_reg` | VMVN_reg | `111100111D11zz00dddd01011QM0mmmm` | translated | UndefinedInstruction | `asimd_two_regs_misc.cpp` |
| 22 | `asimd_VQABS` | VQABS | `111100111D11zz00dddd01110QM0mmmm` | translated | UndefinedInstruction | `asimd_two_regs_misc.cpp` |
| 23 | `asimd_VQNEG` | VQNEG | `111100111D11zz00dddd01111QM0mmmm` | translated | UndefinedInstruction | `asimd_two_regs_misc.cpp` |
| 24 | `arm_UDF` | UNALLOCATED | `111100111-11--01----01101--0----` | architectural undefined | UndefinedInstruction | `a32_exception_generating.cpp` |
| 25 | `arm_UDF` | UNALLOCATED | `111100111-11--10----00000--0----` | architectural undefined | UndefinedInstruction | `a32_exception_generating.cpp` |
| 26 | `asimd_VTRN` | VTRN | `111100111D11zz10dddd00001QM0mmmm` | translated | UndefinedInstruction, UnpredictableInstruction | `asimd_two_regs_misc.cpp` |
| 27 | `asimd_VUZP` | VUZP | `111100111D11zz10dddd00010QM0mmmm` | translated | UndefinedInstruction, UnpredictableInstruction | `asimd_two_regs_misc.cpp` |
| 28 | `asimd_VZIP` | VZIP | `111100111D11zz10dddd00011QM0mmmm` | translated | UndefinedInstruction, UnpredictableInstruction | `asimd_two_regs_misc.cpp` |
| 29 | `asimd_VQMOVN` | VQMOVN | `111100111D11zz10dddd00101oM0mmmm` | translated | UndefinedInstruction | `asimd_two_regs_misc.cpp` |
| 30 | `v8_VRINTN` | VRINTN | `111100111D11zz10dddd01000QM0mmmm` | translated |  | `asimd_two_regs_misc.cpp` |
| 31 | `v8_VRINTX` | VRINTX | `111100111D11zz10dddd01001QM0mmmm` | translated |  | `asimd_two_regs_misc.cpp` |
| 32 | `v8_VRINTA` | VRINTA | `111100111D11zz10dddd01010QM0mmmm` | translated |  | `asimd_two_regs_misc.cpp` |
| 33 | `v8_VRINTZ` | VRINTZ | `111100111D11zz10dddd01011QM0mmmm` | translated |  | `asimd_two_regs_misc.cpp` |
| 34 | `v8_VRINTM` | VRINTM | `111100111D11zz10dddd01101QM0mmmm` | translated |  | `asimd_two_regs_misc.cpp` |
| 35 | `v8_VRINTP` | VRINTP | `111100111D11zz10dddd01111QM0mmmm` | translated |  | `asimd_two_regs_misc.cpp` |
| 36 | `asimd_VCVT_half` | VCVT (half-precision) | `111100111D11zz10dddd011o00M0mmmm` | translated | UndefinedInstruction | `asimd_two_regs_misc.cpp` |
| 37 | `arm_UDF` | UNALLOCATED | `111100111-11--10----011-01-0----` | architectural undefined | UndefinedInstruction | `a32_exception_generating.cpp` |
| 38 | `asimd_VPADDL` | VPADDL | `111100111D11zz00dddd0010oQM0mmmm` | translated |  | `asimd_two_regs_misc.cpp` |
| 39 | `asimd_VPADAL` | VPADAL | `111100111D11zz00dddd0110oQM0mmmm` | translated |  | `asimd_two_regs_misc.cpp` |
| 40 | `asimd_VCGT_zero` | VCGT (zero) | `111100111D11zz01dddd0F000QM0mmmm` | translated |  | `asimd_two_regs_misc.cpp` |
| 41 | `asimd_VCGE_zero` | VCGE (zero) | `111100111D11zz01dddd0F001QM0mmmm` | translated |  | `asimd_two_regs_misc.cpp` |
| 42 | `asimd_VCEQ_zero` | VCEQ (zero) | `111100111D11zz01dddd0F010QM0mmmm` | translated |  | `asimd_two_regs_misc.cpp` |
| 43 | `asimd_VCLE_zero` | VCLE (zero) | `111100111D11zz01dddd0F011QM0mmmm` | translated |  | `asimd_two_regs_misc.cpp` |
| 44 | `asimd_VCLT_zero` | VCLT (zero) | `111100111D11zz01dddd0F100QM0mmmm` | translated |  | `asimd_two_regs_misc.cpp` |
| 45 | `asimd_VABS` | VABS | `111100111D11zz01dddd0F110QM0mmmm` | translated | UndefinedInstruction | `asimd_two_regs_misc.cpp` |
| 46 | `asimd_VNEG` | VNEG | `111100111D11zz01dddd0F111QM0mmmm` | translated | UndefinedInstruction | `asimd_two_regs_misc.cpp` |
| 47 | `v8_VCVTA` | VCVTA | `111100111D11zz11dddd0000oQM0mmmm` | translated |  | `asimd_two_regs_misc.cpp` |
| 48 | `v8_VCVTN` | VCVTN | `111100111D11zz11dddd0001oQM0mmmm` | translated |  | `asimd_two_regs_misc.cpp` |
| 49 | `v8_VCVTP` | VCVTP | `111100111D11zz11dddd0010oQM0mmmm` | translated |  | `asimd_two_regs_misc.cpp` |
| 50 | `v8_VCVTM` | VCVTM | `111100111D11zz11dddd0011oQM0mmmm` | translated |  | `asimd_two_regs_misc.cpp` |
| 51 | `asimd_VRECPE` | VRECPE | `111100111D11zz11dddd010F0QM0mmmm` | translated | UndefinedInstruction | `asimd_two_regs_misc.cpp` |
| 52 | `asimd_VRSQRTE` | VRSQRTE | `111100111D11zz11dddd010F1QM0mmmm` | translated | UndefinedInstruction | `asimd_two_regs_misc.cpp` |
| 53 | `asimd_VREV` | VREV{16,32,64} | `111100111D11zz00dddd000ooQM0mmmm` | translated | UNREACHABLE, UndefinedInstruction | `asimd_two_regs_misc.cpp` |
| 54 | `asimd_VCVT_integer` | VCVT (integer) | `111100111D11zz11dddd011oUQM0mmmm` | translated | UndefinedInstruction | `asimd_two_regs_misc.cpp` |
| 55 | `asimd_VAND_reg` | VAND (register) | `111100100D00nnnndddd0001NQM1mmmm` | translated |  | `asimd_three_regs.cpp` |
| 56 | `asimd_VBIC_reg` | VBIC (register) | `111100100D01nnnndddd0001NQM1mmmm` | translated |  | `asimd_three_regs.cpp` |
| 57 | `asimd_VORR_reg` | VORR (register) | `111100100D10nnnndddd0001NQM1mmmm` | translated |  | `asimd_three_regs.cpp` |
| 58 | `asimd_VORN_reg` | VORN (register) | `111100100D11nnnndddd0001NQM1mmmm` | translated |  | `asimd_three_regs.cpp` |
| 59 | `asimd_VEOR_reg` | VEOR (register) | `111100110D00nnnndddd0001NQM1mmmm` | translated |  | `asimd_three_regs.cpp` |
| 60 | `asimd_VBSL` | VBSL | `111100110D01nnnndddd0001NQM1mmmm` | translated |  | `asimd_three_regs.cpp` |
| 61 | `asimd_VBIT` | VBIT | `111100110D10nnnndddd0001NQM1mmmm` | translated |  | `asimd_three_regs.cpp` |
| 62 | `asimd_VBIF` | VBIF | `111100110D11nnnndddd0001NQM1mmmm` | translated |  | `asimd_three_regs.cpp` |
| 63 | `v8_SHA256H` | SHA256H | `111100110D00nnnndddd1100NQM0mmmm` | translated | UndefinedInstruction | `asimd_three_regs.cpp` |
| 64 | `v8_SHA256H2` | SHA256H2 | `111100110D01nnnndddd1100NQM0mmmm` | translated | UndefinedInstruction | `asimd_three_regs.cpp` |
| 65 | `v8_SHA256SU1` | SHA256SU1 | `111100110D10nnnndddd1100NQM0mmmm` | translated | UndefinedInstruction | `asimd_three_regs.cpp` |
| 66 | `asimd_VSHRN` | VSHRN | `111100101Diiiiiidddd100000M1mmmm` | translated |  | `asimd_two_regs_shift.cpp` |
| 67 | `asimd_VRSHRN` | VRSHRN | `111100101Diiiiiidddd100001M1mmmm` | translated |  | `asimd_two_regs_shift.cpp` |
| 68 | `asimd_VQSHRUN` | VQSHRUN | `111100111Diiiiiidddd100000M1mmmm` | translated |  | `asimd_two_regs_shift.cpp` |
| 69 | `asimd_VQRSHRUN` | VQRSHRUN | `111100111Diiiiiidddd100001M1mmmm` | translated |  | `asimd_two_regs_shift.cpp` |
| 70 | `v8_VMAXNM` | VMAXNM | `111100110D0znnnndddd1111NQM1mmmm` | translated |  | `asimd_three_regs.cpp` |
| 71 | `v8_VMINNM` | VMINNM | `111100110D1znnnndddd1111NQM1mmmm` | translated |  | `asimd_three_regs.cpp` |
| 72 | `asimd_VFMA` | VFMA | `111100100D0znnnndddd1100NQM1mmmm` | translated |  | `asimd_three_regs.cpp` |
| 73 | `asimd_VFMS` | VFMS | `111100100D1znnnndddd1100NQM1mmmm` | translated |  | `asimd_three_regs.cpp` |
| 74 | `asimd_VADD_float` | VADD (floating-point) | `111100100D0znnnndddd1101NQM0mmmm` | translated |  | `asimd_three_regs.cpp` |
| 75 | `asimd_VSUB_float` | VSUB (floating-point) | `111100100D1znnnndddd1101NQM0mmmm` | translated |  | `asimd_three_regs.cpp` |
| 76 | `asimd_VPADD_float` | VPADD (floating-point) | `111100110D0znnnndddd1101NQM0mmmm` | translated | UndefinedInstruction | `asimd_three_regs.cpp` |
| 77 | `asimd_VABD_float` | VABD (floating-point) | `111100110D1znnnndddd1101NQM0mmmm` | translated |  | `asimd_three_regs.cpp` |
| 78 | `asimd_VMLA_float` | VMLA (floating-point) | `111100100D0znnnndddd1101NQM1mmmm` | translated |  | `asimd_three_regs.cpp` |
| 79 | `asimd_VMLS_float` | VMLS (floating-point) | `111100100D1znnnndddd1101NQM1mmmm` | translated |  | `asimd_three_regs.cpp` |
| 80 | `asimd_VMUL_float` | VMUL (floating-point) | `111100110D0znnnndddd1101NQM1mmmm` | translated |  | `asimd_three_regs.cpp` |
| 81 | `asimd_VCEQ_reg_float` | VCEQ (register) | `111100100D0znnnndddd1110NQM0mmmm` | translated |  | `asimd_three_regs.cpp` |
| 82 | `asimd_VCGE_reg_float` | VCGE (register) | `111100110D0znnnndddd1110NQM0mmmm` | translated |  | `asimd_three_regs.cpp` |
| 83 | `asimd_VCGT_reg_float` | VCGT (register) | `111100110D1znnnndddd1110NQM0mmmm` | translated |  | `asimd_three_regs.cpp` |
| 84 | `asimd_VMAX_float` | VMAX (floating-point) | `111100100D0znnnndddd1111NQM0mmmm` | translated |  | `asimd_three_regs.cpp` |
| 85 | `asimd_VMIN_float` | VMIN (floating-point) | `111100100D1znnnndddd1111NQM0mmmm` | translated |  | `asimd_three_regs.cpp` |
| 86 | `asimd_VPMAX_float` | VPMAX (floating-point) | `111100110D0znnnndddd1111NQM0mmmm` | translated | UndefinedInstruction | `asimd_three_regs.cpp` |
| 87 | `asimd_VPMIN_float` | VPMIN (floating-point) | `111100110D1znnnndddd1111NQM0mmmm` | translated | UndefinedInstruction | `asimd_three_regs.cpp` |
| 88 | `asimd_VRECPS` | VRECPS | `111100100D0znnnndddd1111NQM1mmmm` | translated |  | `asimd_three_regs.cpp` |
| 89 | `asimd_VRSQRTS` | VRSQRTS | `111100100D1znnnndddd1111NQM1mmmm` | translated |  | `asimd_three_regs.cpp` |
| 90 | `asimd_VQSHRN` | VQSHRN | `1111001U1Diiiiiidddd100100M1mmmm` | translated |  | `asimd_two_regs_shift.cpp` |
| 91 | `asimd_VQRSHRN` | VQRSHRN | `1111001U1Diiiiiidddd100101M1mmmm` | translated |  | `asimd_two_regs_shift.cpp` |
| 92 | `asimd_VSHLL` | VSHLL | `1111001U1Diiiiiidddd101000M1mmmm` | translated | DecodeError, UndefinedInstruction | `asimd_two_regs_shift.cpp` |
| 93 | `asimd_VADD_int` | VADD (integer) | `111100100Dzznnnndddd1000NQM0mmmm` | translated | UndefinedInstruction | `asimd_three_regs.cpp` |
| 94 | `asimd_VSUB_int` | VSUB (integer) | `111100110Dzznnnndddd1000NQM0mmmm` | translated | UndefinedInstruction | `asimd_three_regs.cpp` |
| 95 | `asimd_VTST` | VTST | `111100100Dzznnnndddd1000NQM1mmmm` | translated | UndefinedInstruction | `asimd_three_regs.cpp` |
| 96 | `asimd_VCEQ_reg` | VCEG (register) | `111100110Dzznnnndddd1000NQM1mmmm` | translated |  | `asimd_three_regs.cpp` |
| 97 | `asimd_VQDMULH` | VQDMULH | `111100100Dzznnnndddd1011NQM0mmmm` | translated | UndefinedInstruction | `asimd_three_regs.cpp` |
| 98 | `asimd_VQRDMULH` | VQRDMULH | `111100110Dzznnnndddd1011NQM0mmmm` | translated | UndefinedInstruction | `asimd_three_regs.cpp` |
| 99 | `asimd_VPADD` | VPADD | `111100100Dzznnnndddd1011NQM1mmmm` | translated | UndefinedInstruction | `asimd_three_regs.cpp` |
| 100 | `asimd_VACGE` | VACGE | `111100110Doznnnndddd1110NQM1mmmm` | translated |  | `asimd_three_regs.cpp` |
| 101 | `asimd_VABAL` | VABAL | `1111001U1Dzznnnndddd0101N0M0mmmm` | translated |  | `asimd_three_regs.cpp` |
| 102 | `asimd_VABDL` | VABDL | `1111001U1Dzznnnndddd0111N0M0mmmm` | translated |  | `asimd_three_regs.cpp` |
| 103 | `asimd_VSRI` | VSRI | `111100111Diiiiiidddd0100LQM1mmmm` | translated | DecodeError, UndefinedInstruction | `asimd_two_regs_shift.cpp` |
| 104 | `asimd_VSHL` | VSHL | `111100101Diiiiiidddd0101LQM1mmmm` | translated | DecodeError, UndefinedInstruction | `asimd_two_regs_shift.cpp` |
| 105 | `asimd_VSLI` | VSLI | `111100111Diiiiiidddd0101LQM1mmmm` | translated | DecodeError, UndefinedInstruction | `asimd_two_regs_shift.cpp` |
| 106 | `arm_UDF` | UNALLOCATED | `111100111-11--------11-----0----` | architectural undefined | UndefinedInstruction | `a32_exception_generating.cpp` |
| 107 | `arm_UDF` | UNALLOCATED | `111101000--0--------1011--------` | architectural undefined | UndefinedInstruction | `a32_exception_generating.cpp` |
| 108 | `asimd_VHADD` | VHADD | `1111001U0Dzznnnndddd0000NQM0mmmm` | translated | UndefinedInstruction | `asimd_three_regs.cpp` |
| 109 | `asimd_VQADD` | VQADD | `1111001U0Dzznnnndddd0000NQM1mmmm` | translated | UndefinedInstruction | `asimd_three_regs.cpp` |
| 110 | `asimd_VRHADD` | VRHADD | `1111001U0Dzznnnndddd0001NQM0mmmm` | translated | UndefinedInstruction | `asimd_three_regs.cpp` |
| 111 | `asimd_VHSUB` | VHSUB | `1111001U0Dzznnnndddd0010NQM0mmmm` | translated | UndefinedInstruction | `asimd_three_regs.cpp` |
| 112 | `asimd_VQSUB` | VQSUB | `1111001U0Dzznnnndddd0010NQM1mmmm` | translated | UndefinedInstruction | `asimd_three_regs.cpp` |
| 113 | `asimd_VCGT_reg` | VCGT (register) | `1111001U0Dzznnnndddd0011NQM0mmmm` | translated |  | `asimd_three_regs.cpp` |
| 114 | `asimd_VCGE_reg` | VCGE (register) | `1111001U0Dzznnnndddd0011NQM1mmmm` | translated |  | `asimd_three_regs.cpp` |
| 115 | `asimd_VSHL_reg` | VSHL (register) | `1111001U0Dzznnnndddd0100NQM0mmmm` | translated | UndefinedInstruction | `asimd_three_regs.cpp` |
| 116 | `asimd_VQSHL_reg` | VQSHL (register) | `1111001U0Dzznnnndddd0100NQM1mmmm` | translated | UndefinedInstruction | `asimd_three_regs.cpp` |
| 117 | `asimd_VRSHL` | VRSHL | `1111001U0Dzznnnndddd0101NQM0mmmm` | translated | UndefinedInstruction | `asimd_three_regs.cpp` |
| 118 | `asimd_VABD` | VABD | `1111001U0Dzznnnndddd0111NQM0mmmm` | translated |  | `asimd_three_regs.cpp` |
| 119 | `asimd_VABA` | VABA | `1111001U0Dzznnnndddd0111NQM1mmmm` | translated |  | `asimd_three_regs.cpp` |
| 120 | `asimd_VMLA` | VMLA/VMLS | `1111001o0Dzznnnndddd1001NQM0mmmm` | translated | UndefinedInstruction | `asimd_three_regs.cpp` |
| 121 | `asimd_VMUL` | VMUL | `1111001P0Dzznnnndddd1001NQM1mmmm` | translated | UndefinedInstruction | `asimd_three_regs.cpp` |
| 122 | `asimd_VADDL` | VADDL/VADDW | `1111001U1Dzznnnndddd000oN0M0mmmm` | translated |  | `asimd_three_regs.cpp` |
| 123 | `asimd_VSUBL` | VSUBL/VSUBW | `1111001U1Dzznnnndddd001oN0M0mmmm` | translated |  | `asimd_three_regs.cpp` |
| 124 | `asimd_VMLAL` | VMLAL/VMLSL | `1111001U1Dzznnnndddd10o0N0M0mmmm` | translated | DecodeError, UndefinedInstruction | `asimd_three_regs.cpp` |
| 125 | `asimd_VMULL` | VMULL | `1111001U1Dzznnnndddd11P0N0M0mmmm` | translated | DecodeError, UndefinedInstruction | `asimd_three_regs.cpp` |
| 126 | `asimd_SHR` | SHR | `1111001U1Diiiiiidddd0000LQM1mmmm` | translated |  | `asimd_two_regs_shift.cpp` |
| 127 | `asimd_SRA` | SRA | `1111001U1Diiiiiidddd0001LQM1mmmm` | translated |  | `asimd_two_regs_shift.cpp` |
| 128 | `asimd_VRSHR` | VRSHR | `1111001U1Diiiiiidddd0010LQM1mmmm` | translated |  | `asimd_two_regs_shift.cpp` |
| 129 | `asimd_VRSRA` | VRSRA | `1111001U1Diiiiiidddd0011LQM1mmmm` | translated |  | `asimd_two_regs_shift.cpp` |
| 130 | `asimd_VCVT_fixed` | VCVT (fixed-point) | `1111001U1Diiiiiidddd111o0QM1mmmm` | translated | DecodeError, UndefinedInstruction | `asimd_two_regs_shift.cpp` |
| 131 | `arm_UDF` | UNALLOCATED | `111101001-00--------11----------` | architectural undefined | UndefinedInstruction | `a32_exception_generating.cpp` |
| 132 | `v8_VLD_all_lanes` | VLD{1-4} (all lanes) | `111101001D10nnnndddd11nnzzTammmm` | translated | UndefinedInstruction, UnpredictableInstruction | `asimd_load_store_structures.cpp` |
| 133 | `asimd_VMAX` | VMAX/VMIN (integer) | `1111001U0Dzznnnnmmmm0110NQMommmm` | translated | UndefinedInstruction | `asimd_three_regs.cpp` |
| 134 | `asimd_VPMAX_int` | VPMAX/VPMIN (integer) | `1111001U0Dzznnnndddd1010NQMommmm` | translated | UndefinedInstruction | `asimd_three_regs.cpp` |
| 135 | `asimd_VQSHL` | VQSHL | `1111001U1Diiiiiidddd011oLQM1mmmm` | translated | DecodeError, UndefinedInstruction | `asimd_two_regs_shift.cpp` |
| 136 | `arm_UDF` | UNALLOCATED | `111101000--0--------11----------` | architectural undefined | UndefinedInstruction | `a32_exception_generating.cpp` |
| 137 | `v8_VST_multiple` | VST{1-4} (multiple) | `111101000D00nnnnddddxxxxzzaammmm` | translated | DecodeError, UndefinedInstruction, UnpredictableInstruction | `asimd_load_store_structures.cpp` |
| 138 | `v8_VLD_multiple` | VLD{1-4} (multiple) | `111101000D10nnnnddddxxxxzzaammmm` | translated | DecodeError, UndefinedInstruction, UnpredictableInstruction | `asimd_load_store_structures.cpp` |
| 139 | `v8_VST_single` | VST{1-4} (single) | `111101001D00nnnnddddzzNNaaaammmm` | translated | DecodeError, UndefinedInstruction, UnpredictableInstruction | `asimd_load_store_structures.cpp` |
| 140 | `v8_VLD_single` | VLD{1-4} (single) | `111101001D10nnnnddddzzNNaaaammmm` | translated | DecodeError, UndefinedInstruction, UnpredictableInstruction | `asimd_load_store_structures.cpp` |
| 141 | `asimd_VMLA_scalar` | VMLA (scalar) | `1111001Q1Dzznnnndddd0o0FN1M0mmmm` | translated |  | `asimd_two_regs_scalar.cpp` |
| 142 | `asimd_VMLAL_scalar` | VMLAL (scalar) | `1111001U1dzznnnndddd0o10N1M0mmmm` | translated |  | `asimd_two_regs_scalar.cpp` |
| 143 | `asimd_VMUL_scalar` | VMUL (scalar) | `1111001Q1Dzznnnndddd100FN1M0mmmm` | translated |  | `asimd_two_regs_scalar.cpp` |
| 144 | `asimd_VMULL_scalar` | VMULL (scalar) | `1111001U1Dzznnnndddd1010N1M0mmmm` | translated |  | `asimd_two_regs_scalar.cpp` |
| 145 | `asimd_VQDMULL_scalar` | VQDMULL (scalar) | `111100101Dzznnnndddd1011N1M0mmmm` | translated | DecodeError, UndefinedInstruction | `asimd_two_regs_scalar.cpp` |
| 146 | `asimd_VQDMULH_scalar` | VQDMULH (scalar) | `1111001Q1Dzznnnndddd1100N1M0mmmm` | translated |  | `asimd_two_regs_scalar.cpp` |
| 147 | `asimd_VQRDMULH_scalar` | VQRDMULH (scalar) | `1111001Q1Dzznnnndddd1101N1M0mmmm` | translated |  | `asimd_two_regs_scalar.cpp` |

### vfp

| Row | Handler | Name | Encoding | Status | Guards | Source |
|---:|---|---|---|---|---|---|
| 1 | `vfp_VMLA` | VMLA | `cccc11100D00nnnndddd101zN0M0mmmm` | translated |  | `vfp.cpp` |
| 2 | `vfp_VMLS` | VMLS | `cccc11100D00nnnndddd101zN1M0mmmm` | translated |  | `vfp.cpp` |
| 3 | `vfp_VNMLS` | VNMLS | `cccc11100D01nnnndddd101zN0M0mmmm` | translated |  | `vfp.cpp` |
| 4 | `vfp_VNMLA` | VNMLA | `cccc11100D01nnnndddd101zN1M0mmmm` | translated |  | `vfp.cpp` |
| 5 | `vfp_VMUL` | VMUL | `cccc11100D10nnnndddd101zN0M0mmmm` | translated |  | `vfp.cpp` |
| 6 | `vfp_VNMUL` | VNMUL | `cccc11100D10nnnndddd101zN1M0mmmm` | translated |  | `vfp.cpp` |
| 7 | `vfp_VADD` | VADD | `cccc11100D11nnnndddd101zN0M0mmmm` | translated |  | `vfp.cpp` |
| 8 | `vfp_VSUB` | VSUB | `cccc11100D11nnnndddd101zN1M0mmmm` | translated |  | `vfp.cpp` |
| 9 | `vfp_VDIV` | VDIV | `cccc11101D00nnnndddd101zN0M0mmmm` | translated |  | `vfp.cpp` |
| 10 | `vfp_VFNMS` | VFNMS | `cccc11101D01nnnndddd101zN0M0mmmm` | translated |  | `vfp.cpp` |
| 11 | `vfp_VFNMA` | VFNMA | `cccc11101D01nnnndddd101zN1M0mmmm` | translated |  | `vfp.cpp` |
| 12 | `vfp_VFMA` | VFMA | `cccc11101D10nnnndddd101zN0M0mmmm` | translated |  | `vfp.cpp` |
| 13 | `vfp_VFMS` | VFMS | `cccc11101D10nnnndddd101zN1M0mmmm` | translated |  | `vfp.cpp` |
| 14 | `vfp_VSEL` | VSEL | `111111100Dccnnnndddd101zN0M0mmmm` | translated |  | `vfp.cpp` |
| 15 | `vfp_VMAXNM` | VMAXNNM | `111111101D00nnnndddd101zN0M0mmmm` | translated |  | `vfp.cpp` |
| 16 | `vfp_VMINNM` | VMINNM | `111111101D00nnnndddd101zN1M0mmmm` | translated |  | `vfp.cpp` |
| 17 | `vfp_VMOV_imm` | VMOV (immediate) | `cccc11101D11vvvvdddd101z0000vvvv` | translated | UndefinedInstruction | `vfp.cpp` |
| 18 | `vfp_VMOV_reg` | VMOV (reg) | `cccc11101D110000dddd101z01M0mmmm` | translated |  | `vfp.cpp` |
| 19 | `vfp_VABS` | VABS | `cccc11101D110000dddd101z11M0mmmm` | translated |  | `vfp.cpp` |
| 20 | `vfp_VNEG` | VNEG | `cccc11101D110001dddd101z01M0mmmm` | translated |  | `vfp.cpp` |
| 21 | `vfp_VSQRT` | VSQRT | `cccc11101D110001dddd101z11M0mmmm` | translated |  | `vfp.cpp` |
| 22 | `vfp_VCVTB` | VCVTB | `cccc11101D11001odddd101z01M0mmmm` | translated |  | `vfp.cpp` |
| 23 | `vfp_VCVTT` | VCVTT | `cccc11101D11001odddd101z11M0mmmm` | translated |  | `vfp.cpp` |
| 24 | `vfp_VCMP` | VCMP | `cccc11101D110100dddd101zE1M0mmmm` | translated |  | `vfp.cpp` |
| 25 | `vfp_VCMP_zero` | VCMP (with zero) | `cccc11101D110101dddd101zE1000000` | translated |  | `vfp.cpp` |
| 26 | `vfp_VRINTR` | VRINTR | `cccc11101D110110dddd101z01M0mmmm` | translated |  | `vfp.cpp` |
| 27 | `vfp_VRINTZ` | VRINTZ | `cccc11101D110110dddd101z11M0mmmm` | translated |  | `vfp.cpp` |
| 28 | `vfp_VRINTX` | VRINTX | `cccc11101D110111dddd101z01M0mmmm` | translated |  | `vfp.cpp` |
| 29 | `vfp_VCVT_f_to_f` | VCVT (f32<->f64) | `cccc11101D110111dddd101z11M0mmmm` | translated |  | `vfp.cpp` |
| 30 | `vfp_VCVT_from_int` | VCVT (from int) | `cccc11101D111000dddd101zs1M0mmmm` | translated |  | `vfp.cpp` |
| 31 | `vfp_VCVT_from_fixed` | VCVT (from fixed) | `cccc11101D11101Udddd101zx1i0vvvv` | translated | UnpredictableInstruction | `vfp.cpp` |
| 32 | `vfp_VCVT_to_u32` | VCVT (to u32) | `cccc11101D111100dddd101zr1M0mmmm` | translated |  | `vfp.cpp` |
| 33 | `vfp_VCVT_to_s32` | VCVT (to s32) | `cccc11101D111101dddd101zr1M0mmmm` | translated |  | `vfp.cpp` |
| 34 | `vfp_VCVT_to_fixed` | VCVT (to fixed) | `cccc11101D11111Udddd101zx1i0vvvv` | translated | UnpredictableInstruction | `vfp.cpp` |
| 35 | `vfp_VRINT_rm` | VRINT{A,N,P,M} | `111111101D1110mmdddd101z01M0mmmm` | translated |  | `vfp.cpp` |
| 36 | `vfp_VCVT_rm` | VCVT{A,N,P,M} | `111111101D1111mmdddd101zU1M0mmmm` | translated |  | `vfp.cpp` |
| 37 | `vfp_VMOV_u32_f64` | VMOV (core to f64) | `cccc11100000ddddtttt1011D0010000` | translated | UnpredictableInstruction | `vfp.cpp` |
| 38 | `vfp_VMOV_f64_u32` | VMOV (f64 to core) | `cccc11100001nnnntttt1011N0010000` | translated | UnpredictableInstruction | `vfp.cpp` |
| 39 | `vfp_VMOV_u32_f32` | VMOV (core to f32) | `cccc11100000nnnntttt1010N0010000` | translated | UnpredictableInstruction | `vfp.cpp` |
| 40 | `vfp_VMOV_f32_u32` | VMOV (f32 to core) | `cccc11100001nnnntttt1010N0010000` | translated | UnpredictableInstruction | `vfp.cpp` |
| 41 | `vfp_VMOV_2u32_2f32` | VMOV (2xcore to 2xf32) | `cccc11000100uuuutttt101000M1mmmm` | translated | UnpredictableInstruction | `vfp.cpp` |
| 42 | `vfp_VMOV_2f32_2u32` | VMOV (2xf32 to 2xcore) | `cccc11000101uuuutttt101000M1mmmm` | translated | UnpredictableInstruction | `vfp.cpp` |
| 43 | `vfp_VMOV_2u32_f64` | VMOV (2xcore to f64) | `cccc11000100uuuutttt101100M1mmmm` | translated | UnpredictableInstruction | `vfp.cpp` |
| 44 | `vfp_VMOV_f64_2u32` | VMOV (f64 to 2xcore) | `cccc11000101uuuutttt101100M1mmmm` | translated | UnpredictableInstruction | `vfp.cpp` |
| 45 | `vfp_VMOV_from_i32` | VMOV (core to i32) | `cccc111000i0nnnntttt1011N0010000` | translated | UnpredictableInstruction | `vfp.cpp` |
| 46 | `vfp_VMOV_from_i16` | VMOV (core to i16) | `cccc111000i0nnnntttt1011Ni110000` | translated | UnpredictableInstruction | `vfp.cpp` |
| 47 | `vfp_VMOV_from_i8` | VMOV (core to i8) | `cccc111001i0nnnntttt1011Nii10000` | translated | UnpredictableInstruction | `vfp.cpp` |
| 48 | `vfp_VMOV_to_i32` | VMOV (i32 to core) | `cccc111000i1nnnntttt1011N0010000` | translated | UnpredictableInstruction | `vfp.cpp` |
| 49 | `vfp_VMOV_to_i16` | VMOV (i16 to core) | `cccc1110U0i1nnnntttt1011Ni110000` | translated | UnpredictableInstruction | `vfp.cpp` |
| 50 | `vfp_VMOV_to_i8` | VMOV (i8 to core) | `cccc1110U1i1nnnntttt1011Nii10000` | translated | UnpredictableInstruction | `vfp.cpp` |
| 51 | `vfp_VDUP` | VDUP (from core) | `cccc11101BQ0ddddtttt1011D0E10000` | translated | UndefinedInstruction, UnpredictableInstruction | `vfp.cpp` |
| 52 | `vfp_VMSR` | VMSR | `cccc111011100001tttt101000010000` | translated | UnpredictableInstruction | `vfp.cpp` |
| 53 | `vfp_VMRS` | VMRS | `cccc111011110001tttt101000010000` | translated |  | `vfp.cpp` |
| 54 | `vfp_VPUSH` | VPUSH | `cccc11010D101101dddd101zvvvvvvvv` | translated | UnpredictableInstruction | `vfp.cpp` |
| 55 | `vfp_VPOP` | VPOP | `cccc11001D111101dddd101zvvvvvvvv` | translated | UnpredictableInstruction | `vfp.cpp` |
| 56 | `vfp_VLDR` | VLDR | `cccc1101UD01nnnndddd101zvvvvvvvv` | translated |  | `vfp.cpp` |
| 57 | `vfp_VSTR` | VSTR | `cccc1101UD00nnnndddd101zvvvvvvvv` | translated |  | `vfp.cpp` |
| 58 | `arm_UDF` | Undefined VSTM/VLDM | `----11000-0---------101---------` | architectural undefined | UndefinedInstruction | `a32_exception_generating.cpp` |
| 59 | `vfp_VSTM_a1` | VSTM (A1) | `cccc110puDw0nnnndddd1011vvvvvvvv` | translated | UnpredictableInstruction | `vfp.cpp` |
| 60 | `vfp_VSTM_a2` | VSTM (A2) | `cccc110puDw0nnnndddd1010vvvvvvvv` | translated | UnpredictableInstruction | `vfp.cpp` |
| 61 | `vfp_VLDM_a1` | VLDM (A1) | `cccc110puDw1nnnndddd1011vvvvvvvv` | translated | UnpredictableInstruction | `vfp.cpp` |
| 62 | `vfp_VLDM_a2` | VLDM (A2) | `cccc110puDw1nnnndddd1010vvvvvvvv` | translated | UnpredictableInstruction | `vfp.cpp` |

### thumb16

| Row | Handler | Name | Encoding | Status | Guards | Source |
|---:|---|---|---|---|---|---|
| 1 | `thumb16_LSL_imm` | LSL (imm) | `00000vvvvvmmmddd` | translated | UnpredictableInstruction | `thumb16.cpp` |
| 2 | `thumb16_LSR_imm` | LSR (imm) | `00001vvvvvmmmddd` | translated |  | `thumb16.cpp` |
| 3 | `thumb16_ASR_imm` | ASR (imm) | `00010vvvvvmmmddd` | translated |  | `thumb16.cpp` |
| 4 | `thumb16_ADD_reg_t1` | ADD (reg, T1) | `0001100mmmnnnddd` | translated |  | `thumb16.cpp` |
| 5 | `thumb16_SUB_reg` | SUB (reg) | `0001101mmmnnnddd` | translated |  | `thumb16.cpp` |
| 6 | `thumb16_ADD_imm_t1` | ADD (imm, T1) | `0001110vvvnnnddd` | translated |  | `thumb16.cpp` |
| 7 | `thumb16_SUB_imm_t1` | SUB (imm, T1) | `0001111vvvnnnddd` | translated |  | `thumb16.cpp` |
| 8 | `thumb16_MOV_imm` | MOV (imm) | `00100dddvvvvvvvv` | translated |  | `thumb16.cpp` |
| 9 | `thumb16_CMP_imm` | CMP (imm) | `00101nnnvvvvvvvv` | translated |  | `thumb16.cpp` |
| 10 | `thumb16_ADD_imm_t2` | ADD (imm, T2) | `00110dddvvvvvvvv` | translated |  | `thumb16.cpp` |
| 11 | `thumb16_SUB_imm_t2` | SUB (imm, T2) | `00111dddvvvvvvvv` | translated |  | `thumb16.cpp` |
| 12 | `thumb16_AND_reg` | AND (reg) | `0100000000mmmddd` | translated |  | `thumb16.cpp` |
| 13 | `thumb16_EOR_reg` | EOR (reg) | `0100000001mmmddd` | translated |  | `thumb16.cpp` |
| 14 | `thumb16_LSL_reg` | LSL (reg) | `0100000010mmmddd` | translated |  | `thumb16.cpp` |
| 15 | `thumb16_LSR_reg` | LSR (reg) | `0100000011mmmddd` | translated |  | `thumb16.cpp` |
| 16 | `thumb16_ASR_reg` | ASR (reg) | `0100000100mmmddd` | translated |  | `thumb16.cpp` |
| 17 | `thumb16_ADC_reg` | ADC (reg) | `0100000101mmmddd` | translated |  | `thumb16.cpp` |
| 18 | `thumb16_SBC_reg` | SBC (reg) | `0100000110mmmddd` | translated |  | `thumb16.cpp` |
| 19 | `thumb16_ROR_reg` | ROR (reg) | `0100000111sssddd` | translated |  | `thumb16.cpp` |
| 20 | `thumb16_TST_reg` | TST (reg) | `0100001000mmmnnn` | translated |  | `thumb16.cpp` |
| 21 | `thumb16_RSB_imm` | RSB (imm) | `0100001001nnnddd` | translated |  | `thumb16.cpp` |
| 22 | `thumb16_CMP_reg_t1` | CMP (reg, T1) | `0100001010mmmnnn` | translated |  | `thumb16.cpp` |
| 23 | `thumb16_CMN_reg` | CMN (reg) | `0100001011mmmnnn` | translated |  | `thumb16.cpp` |
| 24 | `thumb16_ORR_reg` | ORR (reg) | `0100001100mmmddd` | translated |  | `thumb16.cpp` |
| 25 | `thumb16_MUL_reg` | MUL (reg) | `0100001101nnnddd` | translated |  | `thumb16.cpp` |
| 26 | `thumb16_BIC_reg` | BIC (reg) | `0100001110mmmddd` | translated |  | `thumb16.cpp` |
| 27 | `thumb16_MVN_reg` | MVN (reg) | `0100001111mmmddd` | translated |  | `thumb16.cpp` |
| 28 | `thumb16_ADD_reg_t2` | ADD (reg, T2) | `01000100Dmmmmddd` | translated | UnpredictableInstruction | `thumb16.cpp` |
| 29 | `thumb16_CMP_reg_t2` | CMP (reg, T2) | `01000101Nmmmmnnn` | translated | UnpredictableInstruction | `thumb16.cpp` |
| 30 | `thumb16_MOV_reg` | MOV (reg) | `01000110Dmmmmddd` | translated | UnpredictableInstruction | `thumb16.cpp` |
| 31 | `thumb16_LDR_literal` | LDR (literal) | `01001tttvvvvvvvv` | translated |  | `thumb16.cpp` |
| 32 | `thumb16_STR_reg` | STR (reg) | `0101000mmmnnnttt` | translated |  | `thumb16.cpp` |
| 33 | `thumb16_STRH_reg` | STRH (reg) | `0101001mmmnnnttt` | translated |  | `thumb16.cpp` |
| 34 | `thumb16_STRB_reg` | STRB (reg) | `0101010mmmnnnttt` | translated |  | `thumb16.cpp` |
| 35 | `thumb16_LDRSB_reg` | LDRSB (reg) | `0101011mmmnnnttt` | translated |  | `thumb16.cpp` |
| 36 | `thumb16_LDR_reg` | LDR (reg) | `0101100mmmnnnttt` | translated |  | `thumb16.cpp` |
| 37 | `thumb16_LDRH_reg` | LDRH (reg) | `0101101mmmnnnttt` | translated |  | `thumb16.cpp` |
| 38 | `thumb16_LDRB_reg` | LDRB (reg) | `0101110mmmnnnttt` | translated |  | `thumb16.cpp` |
| 39 | `thumb16_LDRSH_reg` | LDRSH (reg) | `0101111mmmnnnttt` | translated |  | `thumb16.cpp` |
| 40 | `thumb16_STR_imm_t1` | STR (imm, T1) | `01100vvvvvnnnttt` | translated |  | `thumb16.cpp` |
| 41 | `thumb16_LDR_imm_t1` | LDR (imm, T1) | `01101vvvvvnnnttt` | translated |  | `thumb16.cpp` |
| 42 | `thumb16_STRB_imm` | STRB (imm) | `01110vvvvvnnnttt` | translated |  | `thumb16.cpp` |
| 43 | `thumb16_LDRB_imm` | LDRB (imm) | `01111vvvvvnnnttt` | translated |  | `thumb16.cpp` |
| 44 | `thumb16_STRH_imm` | STRH (imm) | `10000vvvvvnnnttt` | translated |  | `thumb16.cpp` |
| 45 | `thumb16_LDRH_imm` | LDRH (imm) | `10001vvvvvnnnttt` | translated |  | `thumb16.cpp` |
| 46 | `thumb16_STR_imm_t2` | STR (imm, T2) | `10010tttvvvvvvvv` | translated |  | `thumb16.cpp` |
| 47 | `thumb16_LDR_imm_t2` | LDR (imm, T2) | `10011tttvvvvvvvv` | translated |  | `thumb16.cpp` |
| 48 | `thumb16_ADR` | ADR | `10100dddvvvvvvvv` | translated |  | `thumb16.cpp` |
| 49 | `thumb16_ADD_sp_t1` | ADD (SP plus imm, T1) | `10101dddvvvvvvvv` | translated |  | `thumb16.cpp` |
| 50 | `thumb16_ADD_sp_t2` | ADD (SP plus imm, T2) | `101100000vvvvvvv` | translated |  | `thumb16.cpp` |
| 51 | `thumb16_SUB_sp` | SUB (SP minus imm) | `101100001vvvvvvv` | translated |  | `thumb16.cpp` |
| 52 | `thumb16_SEV` | SEV | `1011111101000000` | translated | RaiseException | `thumb16.cpp` |
| 53 | `thumb16_SEVL` | SEVL | `1011111101010000` | translated | RaiseException | `thumb16.cpp` |
| 54 | `thumb16_WFE` | WFE | `1011111100100000` | translated | RaiseException | `thumb16.cpp` |
| 55 | `thumb16_WFI` | WFI | `1011111100110000` | translated | RaiseException | `thumb16.cpp` |
| 56 | `thumb16_YIELD` | YIELD | `1011111100010000` | translated | RaiseException | `thumb16.cpp` |
| 57 | `thumb16_NOP` | NOP | `10111111----0000` | translated |  | `thumb16.cpp` |
| 58 | `thumb16_IT` | IT | `10111111iiiiiiii` | translated | UnpredictableInstruction | `thumb16.cpp` |
| 59 | `thumb16_SXTH` | SXTH | `1011001000mmmddd` | translated |  | `thumb16.cpp` |
| 60 | `thumb16_SXTB` | SXTB | `1011001001mmmddd` | translated |  | `thumb16.cpp` |
| 61 | `thumb16_UXTH` | UXTH | `1011001010mmmddd` | translated |  | `thumb16.cpp` |
| 62 | `thumb16_UXTB` | UXTB | `1011001011mmmddd` | translated |  | `thumb16.cpp` |
| 63 | `thumb16_PUSH` | PUSH | `1011010Mxxxxxxxx` | translated | UnpredictableInstruction | `thumb16.cpp` |
| 64 | `thumb16_POP` | POP | `1011110Pxxxxxxxx` | translated | UnpredictableInstruction | `thumb16.cpp` |
| 65 | `thumb16_SETEND` | SETEND | `101101100101x000` | translated | UnpredictableInstruction | `thumb16.cpp` |
| 66 | `thumb16_CPS` | CPS | `10110110011m0aif` | translated |  | `thumb16.cpp` |
| 67 | `thumb16_REV` | REV | `1011101000mmmddd` | translated |  | `thumb16.cpp` |
| 68 | `thumb16_REV16` | REV16 | `1011101001mmmddd` | translated |  | `thumb16.cpp` |
| 69 | `thumb16_REVSH` | REVSH | `1011101011mmmddd` | translated |  | `thumb16.cpp` |
| 70 | `thumb16_BKPT` | BKPT | `10111110xxxxxxxx` | translated | RaiseException | `thumb16.cpp` |
| 71 | `thumb16_STMIA` | STMIA | `11000nnnxxxxxxxx` | translated | UnpredictableInstruction | `thumb16.cpp` |
| 72 | `thumb16_LDMIA` | LDMIA | `11001nnnxxxxxxxx` | translated | UnpredictableInstruction | `thumb16.cpp` |
| 73 | `thumb16_BX` | BX | `010001110mmmm000` | translated | UnpredictableInstruction | `thumb16.cpp` |
| 74 | `thumb16_BLX_reg` | BLX (reg) | `010001111mmmm000` | translated | UnpredictableInstruction | `thumb16.cpp` |
| 75 | `thumb16_CBZ_CBNZ` | CBZ/CBNZ | `1011o0i1iiiiinnn` | translated | UnpredictableInstruction | `thumb16.cpp` |
| 76 | `thumb16_UDF` | UDF | `11011110--------` | architectural undefined | UndefinedInstruction | `thumb16.cpp` |
| 77 | `thumb16_SVC` | SVC | `11011111xxxxxxxx` | translated |  | `thumb16.cpp` |
| 78 | `thumb16_B_t1` | B (T1) | `1101ccccvvvvvvvv` | translated | UnpredictableInstruction | `thumb16.cpp` |
| 79 | `thumb16_B_t2` | B (T2) | `11100vvvvvvvvvvv` | translated | UnpredictableInstruction | `thumb16.cpp` |

### thumb32

| Row | Handler | Name | Encoding | Status | Guards | Source |
|---:|---|---|---|---|---|---|
| 1 | `thumb32_STMIA` | STMIA/STMEA | `1110100010W0nnnn0iiiiiiiiiiiiiii` | translated | UnpredictableInstruction | `thumb32_load_store_multiple.cpp` |
| 2 | `thumb32_POP` | POP | `1110100010111101iiiiiiiiiiiiiiii` | translated |  | `thumb32_load_store_multiple.cpp` |
| 3 | `thumb32_LDMIA` | LDMIA/LDMFD | `1110100010W1nnnniiiiiiiiiiiiiiii` | translated | UnpredictableInstruction | `thumb32_load_store_multiple.cpp` |
| 4 | `thumb32_PUSH` | PUSH | `11101001001011010iiiiiiiiiiiiiii` | translated |  | `thumb32_load_store_multiple.cpp` |
| 5 | `thumb32_STMDB` | STMDB/STMFD | `1110100100W0nnnn0iiiiiiiiiiiiiii` | translated | UnpredictableInstruction | `thumb32_load_store_multiple.cpp` |
| 6 | `thumb32_LDMDB` | LDMDB/LDMEA | `1110100100W1nnnniiiiiiiiiiiiiiii` | translated | UnpredictableInstruction | `thumb32_load_store_multiple.cpp` |
| 7 | `thumb32_STREX` | STREX | `111010000100nnnnttttddddiiiiiiii` | translated | UnpredictableInstruction | `thumb32_load_store_dual.cpp` |
| 8 | `thumb32_LDREX` | LDREX | `111010000101nnnntttt1111iiiiiiii` | translated | UnpredictableInstruction | `thumb32_load_store_dual.cpp` |
| 9 | `thumb32_STRD_imm_1` | STRD (imm) | `11101000U110nnnnttttssssiiiiiiii` | translated |  | `thumb32_load_store_dual.cpp` |
| 10 | `thumb32_STRD_imm_2` | STRD (imm) | `11101001U1W0nnnnttttssssiiiiiiii` | translated |  | `thumb32_load_store_dual.cpp` |
| 11 | `thumb32_LDRD_lit_1` | LDRD (lit) | `11101000U1111111ttttssssiiiiiiii` | translated |  | `thumb32_load_store_dual.cpp` |
| 12 | `thumb32_LDRD_lit_2` | LDRD (lit) | `11101001U1W11111ttttssssiiiiiiii` | translated |  | `thumb32_load_store_dual.cpp` |
| 13 | `thumb32_LDRD_imm_1` | LDRD (imm) | `11101000U111nnnnttttssssiiiiiiii` | translated |  | `thumb32_load_store_dual.cpp` |
| 14 | `thumb32_LDRD_imm_2` | LDRD (imm) | `11101001U1W1nnnnttttssssiiiiiiii` | translated |  | `thumb32_load_store_dual.cpp` |
| 15 | `thumb32_STL` | STL | `111010001100nnnntttt111110101111` | translated | UnpredictableInstruction | `thumb32_load_store_dual.cpp` |
| 16 | `thumb32_LDA` | LDA | `111010001101nnnntttt111110101111` | translated | UnpredictableInstruction | `thumb32_load_store_dual.cpp` |
| 17 | `thumb32_STREXB` | STREXB | `111010001100nnnntttt11110100dddd` | translated | UnpredictableInstruction | `thumb32_load_store_dual.cpp` |
| 18 | `thumb32_STREXH` | STREXH | `111010001100nnnntttt11110101dddd` | translated | UnpredictableInstruction | `thumb32_load_store_dual.cpp` |
| 19 | `thumb32_STREXD` | STREXD | `111010001100nnnnttttuuuu0111dddd` | translated | UnpredictableInstruction | `thumb32_load_store_dual.cpp` |
| 20 | `thumb32_TBB` | TBB | `111010001101nnnn111100000000mmmm` | translated |  | `thumb32_load_store_dual.cpp` |
| 21 | `thumb32_TBH` | TBH | `111010001101nnnn111100000001mmmm` | translated |  | `thumb32_load_store_dual.cpp` |
| 22 | `thumb32_LDREXB` | LDREXB | `111010001101nnnntttt111101001111` | translated | UnpredictableInstruction | `thumb32_load_store_dual.cpp` |
| 23 | `thumb32_LDREXH` | LDREXH | `111010001101nnnntttt111101011111` | translated | UnpredictableInstruction | `thumb32_load_store_dual.cpp` |
| 24 | `thumb32_LDREXD` | LDREXD | `111010001101nnnnttttuuuu01111111` | translated | UnpredictableInstruction | `thumb32_load_store_dual.cpp` |
| 25 | `thumb32_TST_reg` | TST (reg) | `111010100001nnnn0vvv1111vvttmmmm` | translated | UnpredictableInstruction | `thumb32_data_processing_shifted_register.cpp` |
| 26 | `thumb32_AND_reg` | AND (reg) | `11101010000Snnnn0vvvddddvvttmmmm` | translated | UnpredictableInstruction | `thumb32_data_processing_shifted_register.cpp` |
| 27 | `thumb32_BIC_reg` | BIC (reg) | `11101010001Snnnn0vvvddddvvttmmmm` | translated | UnpredictableInstruction | `thumb32_data_processing_shifted_register.cpp` |
| 28 | `thumb32_MOV_reg` | MOV (reg) | `11101010010S11110vvvddddvvttmmmm` | translated | UnpredictableInstruction | `thumb32_data_processing_shifted_register.cpp` |
| 29 | `thumb32_ORR_reg` | ORR (reg) | `11101010010Snnnn0vvvddddvvttmmmm` | translated | UnpredictableInstruction | `thumb32_data_processing_shifted_register.cpp` |
| 30 | `thumb32_MVN_reg` | MVN (reg) | `11101010011S11110vvvddddvvttmmmm` | translated | UnpredictableInstruction | `thumb32_data_processing_shifted_register.cpp` |
| 31 | `thumb32_ORN_reg` | ORN (reg) | `11101010011Snnnn0vvvddddvvttmmmm` | translated | UnpredictableInstruction | `thumb32_data_processing_shifted_register.cpp` |
| 32 | `thumb32_TEQ_reg` | TEQ (reg) | `111010101001nnnn0vvv1111vvttmmmm` | translated | UnpredictableInstruction | `thumb32_data_processing_shifted_register.cpp` |
| 33 | `thumb32_EOR_reg` | EOR (reg) | `11101010100Snnnn0vvvddddvvttmmmm` | translated | UnpredictableInstruction | `thumb32_data_processing_shifted_register.cpp` |
| 34 | `thumb32_PKH` | PKH | `111010101100nnnn0vvvddddvvt0mmmm` | translated | UnpredictableInstruction | `thumb32_data_processing_shifted_register.cpp` |
| 35 | `thumb32_CMN_reg` | CMN (reg) | `111010110001nnnn0vvv1111vvttmmmm` | translated | UnpredictableInstruction | `thumb32_data_processing_shifted_register.cpp` |
| 36 | `thumb32_ADD_reg` | ADD (reg) | `11101011000Snnnn0vvvddddvvttmmmm` | translated | UnpredictableInstruction | `thumb32_data_processing_shifted_register.cpp` |
| 37 | `thumb32_ADC_reg` | ADC (reg) | `11101011010Snnnn0vvvddddvvttmmmm` | translated | UnpredictableInstruction | `thumb32_data_processing_shifted_register.cpp` |
| 38 | `thumb32_SBC_reg` | SBC (reg) | `11101011011Snnnn0vvvddddvvttmmmm` | translated | UnpredictableInstruction | `thumb32_data_processing_shifted_register.cpp` |
| 39 | `thumb32_CMP_reg` | CMP (reg) | `111010111011nnnn0vvv1111vvttmmmm` | translated | UnpredictableInstruction | `thumb32_data_processing_shifted_register.cpp` |
| 40 | `thumb32_SUB_reg` | SUB (reg) | `11101011101Snnnn0vvvddddvvttmmmm` | translated | UnpredictableInstruction | `thumb32_data_processing_shifted_register.cpp` |
| 41 | `thumb32_RSB_reg` | RSB (reg) | `11101011110Snnnn0vvvddddvvttmmmm` | translated | UnpredictableInstruction | `thumb32_data_processing_shifted_register.cpp` |
| 42 | `thumb32_TST_imm` | TST (imm) | `11110v000001nnnn0vvv1111vvvvvvvv` | translated | UnpredictableInstruction | `thumb32_data_processing_modified_immediate.cpp` |
| 43 | `thumb32_AND_imm` | AND (imm) | `11110v00000Snnnn0vvvddddvvvvvvvv` | translated | UnpredictableInstruction | `thumb32_data_processing_modified_immediate.cpp` |
| 44 | `thumb32_BIC_imm` | BIC (imm) | `11110v00001Snnnn0vvvddddvvvvvvvv` | translated | UnpredictableInstruction | `thumb32_data_processing_modified_immediate.cpp` |
| 45 | `thumb32_MOV_imm` | MOV (imm) | `11110v00010S11110vvvddddvvvvvvvv` | translated | UnpredictableInstruction | `thumb32_data_processing_modified_immediate.cpp` |
| 46 | `thumb32_ORR_imm` | ORR (imm) | `11110v00010Snnnn0vvvddddvvvvvvvv` | translated | UnpredictableInstruction | `thumb32_data_processing_modified_immediate.cpp` |
| 47 | `thumb32_MVN_imm` | MVN (imm) | `11110v00011S11110vvvddddvvvvvvvv` | translated | UnpredictableInstruction | `thumb32_data_processing_modified_immediate.cpp` |
| 48 | `thumb32_ORN_imm` | ORN (imm) | `11110v00011Snnnn0vvvddddvvvvvvvv` | translated | UnpredictableInstruction | `thumb32_data_processing_modified_immediate.cpp` |
| 49 | `thumb32_TEQ_imm` | TEQ (imm) | `11110v001001nnnn0vvv1111vvvvvvvv` | translated | UnpredictableInstruction | `thumb32_data_processing_modified_immediate.cpp` |
| 50 | `thumb32_EOR_imm` | EOR (imm) | `11110v00100Snnnn0vvvddddvvvvvvvv` | translated | UnpredictableInstruction | `thumb32_data_processing_modified_immediate.cpp` |
| 51 | `thumb32_CMN_imm` | CMN (imm) | `11110v010001nnnn0vvv1111vvvvvvvv` | translated | UnpredictableInstruction | `thumb32_data_processing_modified_immediate.cpp` |
| 52 | `thumb32_ADD_imm_1` | ADD (imm) | `11110v01000Snnnn0vvvddddvvvvvvvv` | translated | UnpredictableInstruction | `thumb32_data_processing_modified_immediate.cpp` |
| 53 | `thumb32_ADC_imm` | ADC (imm) | `11110v01010Snnnn0vvvddddvvvvvvvv` | translated | UnpredictableInstruction | `thumb32_data_processing_modified_immediate.cpp` |
| 54 | `thumb32_SBC_imm` | SBC (imm) | `11110v01011Snnnn0vvvddddvvvvvvvv` | translated | UnpredictableInstruction | `thumb32_data_processing_modified_immediate.cpp` |
| 55 | `thumb32_CMP_imm` | CMP (imm) | `11110v011011nnnn0vvv1111vvvvvvvv` | translated | UnpredictableInstruction | `thumb32_data_processing_modified_immediate.cpp` |
| 56 | `thumb32_SUB_imm_1` | SUB (imm) | `11110v01101Snnnn0vvvddddvvvvvvvv` | translated | UnpredictableInstruction | `thumb32_data_processing_modified_immediate.cpp` |
| 57 | `thumb32_RSB_imm` | RSB (imm) | `11110v01110Snnnn0vvvddddvvvvvvvv` | translated | UnpredictableInstruction | `thumb32_data_processing_modified_immediate.cpp` |
| 58 | `thumb32_ADR_t3` | ADR | `11110i10000011110iiiddddiiiiiiii` | translated | UnpredictableInstruction | `thumb32_data_processing_plain_binary_immediate.cpp` |
| 59 | `thumb32_ADD_imm_2` | ADD (imm) | `11110i100000nnnn0iiiddddiiiiiiii` | translated | UnpredictableInstruction | `thumb32_data_processing_plain_binary_immediate.cpp` |
| 60 | `thumb32_MOVW_imm` | MOVW (imm) | `11110i100100iiii0iiiddddiiiiiiii` | translated | UnpredictableInstruction | `thumb32_data_processing_plain_binary_immediate.cpp` |
| 61 | `thumb32_ADR_t2` | ADR | `11110i10101011110iiiddddiiiiiiii` | translated | UnpredictableInstruction | `thumb32_data_processing_plain_binary_immediate.cpp` |
| 62 | `thumb32_SUB_imm_2` | SUB (imm) | `11110i101010nnnn0iiiddddiiiiiiii` | translated | UnpredictableInstruction | `thumb32_data_processing_plain_binary_immediate.cpp` |
| 63 | `thumb32_MOVT` | MOVT | `11110i101100iiii0iiiddddiiiiiiii` | translated | UnpredictableInstruction | `thumb32_data_processing_plain_binary_immediate.cpp` |
| 64 | `thumb32_UDF` | Invalid decoding | `11110011-010----0000----0001----` | translated |  | `thumb32_control.cpp` |
| 65 | `thumb32_SSAT16` | SSAT16 | `111100110010nnnn0000dddd0000iiii` | translated |  | `thumb32_data_processing_plain_binary_immediate.cpp` |
| 66 | `thumb32_USAT16` | USAT16 | `111100111010nnnn0000dddd0000iiii` | translated |  | `thumb32_data_processing_plain_binary_immediate.cpp` |
| 67 | `thumb32_SSAT` | SSAT | `1111001100s0nnnn0iiiddddii0bbbbb` | translated |  | `thumb32_data_processing_plain_binary_immediate.cpp` |
| 68 | `thumb32_USAT` | USAT | `1111001110s0nnnn0iiiddddii0bbbbb` | translated |  | `thumb32_data_processing_plain_binary_immediate.cpp` |
| 69 | `thumb32_SBFX` | SBFX | `111100110100nnnn0iiiddddii0wwwww` | translated | UnpredictableInstruction | `thumb32_data_processing_plain_binary_immediate.cpp` |
| 70 | `thumb32_BFC` | BFC | `11110011011011110iiiddddii0bbbbb` | translated | UnpredictableInstruction | `thumb32_data_processing_plain_binary_immediate.cpp` |
| 71 | `thumb32_BFI` | BFI | `111100110110nnnn0iiiddddii0bbbbb` | translated | UnpredictableInstruction | `thumb32_data_processing_plain_binary_immediate.cpp` |
| 72 | `thumb32_UBFX` | UBFX | `111100111100nnnn0iiiddddii0wwwww` | translated | UnpredictableInstruction | `thumb32_data_processing_plain_binary_immediate.cpp` |
| 73 | `thumb32_MSR_reg` | MSR (reg) | `11110011100Rnnnn1000mmmm00000000` | translated | UndefinedInstruction, UnpredictableInstruction | `thumb32_control.cpp` |
| 74 | `thumb32_NOP` | NOP | `11110011101011111000000000000000` | translated |  | `thumb32_control.cpp` |
| 75 | `thumb32_YIELD` | YIELD | `11110011101011111000000000000001` | translated |  | `thumb32_control.cpp` |
| 76 | `thumb32_WFE` | WFE | `11110011101011111000000000000010` | translated |  | `thumb32_control.cpp` |
| 77 | `thumb32_WFI` | WFI | `11110011101011111000000000000011` | translated |  | `thumb32_control.cpp` |
| 78 | `thumb32_SEV` | SEV | `11110011101011111000000000000100` | translated |  | `thumb32_control.cpp` |
| 79 | `thumb32_SEVL` | SEVL | `11110011101011111000000000000101` | translated |  | `thumb32_control.cpp` |
| 80 | `thumb32_CLREX` | CLREX | `11110011101111111000111100101111` | translated |  | `thumb32_control.cpp` |
| 81 | `thumb32_DSB` | DSB | `1111001110111111100011110100oooo` | translated |  | `thumb32_control.cpp` |
| 82 | `thumb32_DMB` | DMB | `1111001110111111100011110101oooo` | translated |  | `thumb32_control.cpp` |
| 83 | `thumb32_ISB` | ISB | `1111001110111111100011110110oooo` | translated |  | `thumb32_control.cpp` |
| 84 | `thumb32_BXJ` | BXJ | `111100111100mmmm1000111100000000` | translated | UnpredictableInstruction | `thumb32_control.cpp` |
| 85 | `thumb32_MRS_reg` | MRS (reg) | `11110011111R11111000dddd00000000` | translated | UndefinedInstruction, UnpredictableInstruction | `thumb32_control.cpp` |
| 86 | `thumb32_UDF` | UDF | `111101111111----1010------------` | translated |  | `thumb32_control.cpp` |
| 87 | `thumb32_BL_imm` | BL (imm) | `11110Svvvvvvvvvv11j1jvvvvvvvvvvv` | translated | UnpredictableInstruction | `thumb32_branch.cpp` |
| 88 | `thumb32_BLX_imm` | BLX (imm) | `11110Svvvvvvvvvv11j0jvvvvvvvvvvv` | translated | UnpredictableInstruction | `thumb32_branch.cpp` |
| 89 | `thumb32_B` | B | `11110Svvvvvvvvvv10j1jvvvvvvvvvvv` | translated | UnpredictableInstruction | `thumb32_branch.cpp` |
| 90 | `thumb32_UDF` | Invalid decoding | `11110-111-------10-0------------` | translated |  | `thumb32_control.cpp` |
| 91 | `thumb32_B_cond` | B (cond) | `11110Sccccvvvvvv10i0ivvvvvvvvvvv` | translated | UnpredictableInstruction | `thumb32_branch.cpp` |
| 92 | `thumb32_STRB_imm_1` | STRB (imm) | `111110000000nnnntttt1PU1iiiiiiii` | translated | UndefinedInstruction, UnpredictableInstruction | `thumb32_store_single_data_item.cpp` |
| 93 | `thumb32_STRB_imm_2` | STRB (imm) | `111110000000nnnntttt1100iiiiiiii` | translated | UndefinedInstruction, UnpredictableInstruction | `thumb32_store_single_data_item.cpp` |
| 94 | `thumb32_STRB_imm_3` | STRB (imm) | `111110001000nnnnttttiiiiiiiiiiii` | translated | UndefinedInstruction, UnpredictableInstruction | `thumb32_store_single_data_item.cpp` |
| 95 | `thumb32_STRBT` | STRBT | `111110000000nnnntttt1110iiiiiiii` | translated | UndefinedInstruction, UnpredictableInstruction | `thumb32_store_single_data_item.cpp` |
| 96 | `thumb32_STRB` | STRB (reg) | `111110000000nnnntttt000000iimmmm` | translated |  | `thumb32_store_single_data_item.cpp` |
| 97 | `thumb32_STRH_imm_1` | STRH (imm) | `111110000010nnnntttt1PU1iiiiiiii` | translated | UndefinedInstruction, UnpredictableInstruction | `thumb32_store_single_data_item.cpp` |
| 98 | `thumb32_STRH_imm_2` | STRH (imm) | `111110000010nnnntttt1100iiiiiiii` | translated | UndefinedInstruction, UnpredictableInstruction | `thumb32_store_single_data_item.cpp` |
| 99 | `thumb32_STRH_imm_3` | STRH (imm) | `111110001010nnnnttttiiiiiiiiiiii` | translated | UndefinedInstruction, UnpredictableInstruction | `thumb32_store_single_data_item.cpp` |
| 100 | `thumb32_STRHT` | STRHT | `111110000010nnnntttt1110iiiiiiii` | translated | UndefinedInstruction, UnpredictableInstruction | `thumb32_store_single_data_item.cpp` |
| 101 | `thumb32_STRH` | STRH (reg) | `111110000010nnnntttt000000iimmmm` | translated |  | `thumb32_store_single_data_item.cpp` |
| 102 | `thumb32_STR_imm_1` | STR (imm) | `111110000100nnnntttt1PU1iiiiiiii` | translated | UndefinedInstruction, UnpredictableInstruction | `thumb32_store_single_data_item.cpp` |
| 103 | `thumb32_STR_imm_2` | STR (imm) | `111110000100nnnntttt1100iiiiiiii` | translated | UndefinedInstruction, UnpredictableInstruction | `thumb32_store_single_data_item.cpp` |
| 104 | `thumb32_STR_imm_3` | STR (imm) | `111110001100nnnnttttiiiiiiiiiiii` | translated | UndefinedInstruction, UnpredictableInstruction | `thumb32_store_single_data_item.cpp` |
| 105 | `thumb32_STRT` | STRT | `111110000100nnnntttt1110iiiiiiii` | translated | UndefinedInstruction, UnpredictableInstruction | `thumb32_store_single_data_item.cpp` |
| 106 | `thumb32_STR_reg` | STR (reg) | `111110000100nnnntttt000000iimmmm` | translated |  | `thumb32_store_single_data_item.cpp` |
| 107 | `thumb32_PLD_lit` | PLD (lit) | `11111000U00111111111iiiiiiiiiiii` | translated |  | `thumb32_load_byte.cpp` |
| 108 | `thumb32_PLD_lit` | PLD (lit) | `11111000U01111111111iiiiiiiiiiii` | translated |  | `thumb32_load_byte.cpp` |
| 109 | `thumb32_PLD_reg` | PLD (reg) | `1111100000W1nnnn1111000000iimmmm` | translated | UnpredictableInstruction | `thumb32_load_byte.cpp` |
| 110 | `thumb32_PLD_imm8` | PLD (imm8) | `1111100000W1nnnn11111100iiiiiiii` | translated |  | `thumb32_load_byte.cpp` |
| 111 | `thumb32_PLD_imm12` | PLD (imm12) | `1111100010W1nnnn1111iiiiiiiiiiii` | translated |  | `thumb32_load_byte.cpp` |
| 112 | `thumb32_PLI_lit` | PLI (lit) | `11111001U00111111111iiiiiiiiiiii` | translated |  | `thumb32_load_byte.cpp` |
| 113 | `thumb32_PLI_reg` | PLI (reg) | `111110010001nnnn1111000000iimmmm` | translated | UnpredictableInstruction | `thumb32_load_byte.cpp` |
| 114 | `thumb32_PLI_imm8` | PLI (imm8) | `111110010001nnnn11111100iiiiiiii` | translated |  | `thumb32_load_byte.cpp` |
| 115 | `thumb32_PLI_imm12` | PLI (imm12) | `111110011001nnnn1111iiiiiiiiiiii` | translated |  | `thumb32_load_byte.cpp` |
| 116 | `thumb32_LDRB_lit` | LDRB (lit) | `11111000U0011111ttttiiiiiiiiiiii` | translated |  | `thumb32_load_byte.cpp` |
| 117 | `thumb32_LDRB_reg` | LDRB (reg) | `111110000001nnnntttt000000iimmmm` | translated |  | `thumb32_load_byte.cpp` |
| 118 | `thumb32_LDRBT` | LDRBT | `111110000001nnnntttt1110iiiiiiii` | translated | UnpredictableInstruction | `thumb32_load_byte.cpp` |
| 119 | `thumb32_LDRB_imm8` | LDRB (imm8) | `111110000001nnnntttt1PUWiiiiiiii` | translated | UndefinedInstruction, UnpredictableInstruction | `thumb32_load_byte.cpp` |
| 120 | `thumb32_LDRB_imm12` | LDRB (imm12) | `111110001001nnnnttttiiiiiiiiiiii` | translated |  | `thumb32_load_byte.cpp` |
| 121 | `thumb32_LDRSB_lit` | LDRSB (lit) | `11111001U0011111ttttiiiiiiiiiiii` | translated |  | `thumb32_load_byte.cpp` |
| 122 | `thumb32_LDRSB_reg` | LDRSB (reg) | `111110010001nnnntttt000000iimmmm` | translated |  | `thumb32_load_byte.cpp` |
| 123 | `thumb32_LDRSBT` | LDRSBT | `111110010001nnnntttt1110iiiiiiii` | translated | UnpredictableInstruction | `thumb32_load_byte.cpp` |
| 124 | `thumb32_LDRSB_imm8` | LDRSB (imm8) | `111110010001nnnntttt1PUWiiiiiiii` | translated | UndefinedInstruction, UnpredictableInstruction | `thumb32_load_byte.cpp` |
| 125 | `thumb32_LDRSB_imm12` | LDRSB (imm12) | `111110011001nnnnttttiiiiiiiiiiii` | translated |  | `thumb32_load_byte.cpp` |
| 126 | `thumb32_LDRH_lit` | LDRH (lit) | `11111000U0111111ttttiiiiiiiiiiii` | translated |  | `thumb32_load_halfword.cpp` |
| 127 | `thumb32_LDRH_reg` | LDRH (reg) | `111110000011nnnntttt000000iimmmm` | translated |  | `thumb32_load_halfword.cpp` |
| 128 | `thumb32_LDRHT` | LDRHT | `111110000011nnnntttt1110iiiiiiii` | translated | UnpredictableInstruction | `thumb32_load_halfword.cpp` |
| 129 | `thumb32_LDRH_imm8` | LDRH (imm8) | `111110000011nnnntttt1PUWiiiiiiii` | translated | UndefinedInstruction, UnpredictableInstruction | `thumb32_load_halfword.cpp` |
| 130 | `thumb32_LDRH_imm12` | LDRH (imm12) | `111110001011nnnnttttiiiiiiiiiiii` | translated |  | `thumb32_load_halfword.cpp` |
| 131 | `thumb32_NOP` | NOP | `11111001-01111111111------------` | translated |  | `thumb32_control.cpp` |
| 132 | `thumb32_LDRSH_lit` | LDRSH (lit) | `11111001U0111111ttttiiiiiiiiiiii` | translated |  | `thumb32_load_halfword.cpp` |
| 133 | `thumb32_NOP` | NOP | `111110010011----1111000000------` | translated |  | `thumb32_control.cpp` |
| 134 | `thumb32_LDRSH_reg` | LDRSH (reg) | `111110010011nnnntttt000000iimmmm` | translated |  | `thumb32_load_halfword.cpp` |
| 135 | `thumb32_LDRSHT` | LDRSHT | `111110010011nnnntttt1110iiiiiiii` | translated | UnpredictableInstruction | `thumb32_load_halfword.cpp` |
| 136 | `thumb32_NOP` | NOP | `111110010011----11111100--------` | translated |  | `thumb32_control.cpp` |
| 137 | `thumb32_NOP` | NOP | `111110011011----1111------------` | translated |  | `thumb32_control.cpp` |
| 138 | `thumb32_LDRSH_imm8` | LDRSH (imm8) | `111110010011nnnntttt1PUWiiiiiiii` | translated | UndefinedInstruction, UnpredictableInstruction | `thumb32_load_halfword.cpp` |
| 139 | `thumb32_LDRSH_imm12` | LDRSH (imm12) | `111110011011nnnnttttiiiiiiiiiiii` | translated |  | `thumb32_load_halfword.cpp` |
| 140 | `thumb32_LDR_lit` | LDR (lit) | `11111000U1011111ttttiiiiiiiiiiii` | translated | UnpredictableInstruction | `thumb32_load_word.cpp` |
| 141 | `thumb32_LDRT` | LDRT | `111110000101nnnntttt1110iiiiiiii` | translated | UnpredictableInstruction | `thumb32_load_word.cpp` |
| 142 | `thumb32_LDR_reg` | LDR (reg) | `111110000101nnnntttt000000iimmmm` | translated | UnpredictableInstruction | `thumb32_load_word.cpp` |
| 143 | `thumb32_LDR_imm8` | LDR (imm8) | `111110000101nnnntttt1PUWiiiiiiii` | translated | UndefinedInstruction, UnpredictableInstruction | `thumb32_load_word.cpp` |
| 144 | `thumb32_LDR_imm12` | LDR (imm12) | `111110001101nnnnttttiiiiiiiiiiii` | translated | UnpredictableInstruction | `thumb32_load_word.cpp` |
| 145 | `thumb32_LSL_reg` | LSL (reg) | `11111010000Smmmm1111dddd0000ssss` | translated |  | `thumb32_data_processing_register.cpp` |
| 146 | `thumb32_LSR_reg` | LSR (reg) | `11111010001Smmmm1111dddd0000ssss` | translated |  | `thumb32_data_processing_register.cpp` |
| 147 | `thumb32_ASR_reg` | ASR (reg) | `11111010010Smmmm1111dddd0000ssss` | translated |  | `thumb32_data_processing_register.cpp` |
| 148 | `thumb32_ROR_reg` | ROR (reg) | `11111010011Smmmm1111dddd0000ssss` | translated |  | `thumb32_data_processing_register.cpp` |
| 149 | `thumb32_SXTH` | SXTH | `11111010000011111111dddd10rrmmmm` | translated | UnpredictableInstruction | `thumb32_data_processing_register.cpp` |
| 150 | `thumb32_SXTAH` | SXTAH | `111110100000nnnn1111dddd10rrmmmm` | translated | UnpredictableInstruction | `thumb32_data_processing_register.cpp` |
| 151 | `thumb32_UXTH` | UXTH | `11111010000111111111dddd10rrmmmm` | translated | UnpredictableInstruction | `thumb32_data_processing_register.cpp` |
| 152 | `thumb32_UXTAH` | UXTAH | `111110100001nnnn1111dddd10rrmmmm` | translated | UnpredictableInstruction | `thumb32_data_processing_register.cpp` |
| 153 | `thumb32_SXTB16` | SXTB16 | `11111010001011111111dddd10rrmmmm` | translated | UnpredictableInstruction | `thumb32_data_processing_register.cpp` |
| 154 | `thumb32_SXTAB16` | SXTAB16 | `111110100010nnnn1111dddd10rrmmmm` | translated | UnpredictableInstruction | `thumb32_data_processing_register.cpp` |
| 155 | `thumb32_UXTB16` | UXTB16 | `11111010001111111111dddd10rrmmmm` | translated | UnpredictableInstruction | `thumb32_data_processing_register.cpp` |
| 156 | `thumb32_UXTAB16` | UXTAB16 | `111110100011nnnn1111dddd10rrmmmm` | translated | UnpredictableInstruction | `thumb32_data_processing_register.cpp` |
| 157 | `thumb32_SXTB` | SXTB | `11111010010011111111dddd10rrmmmm` | translated | UnpredictableInstruction | `thumb32_data_processing_register.cpp` |
| 158 | `thumb32_SXTAB` | SXTAB | `111110100100nnnn1111dddd10rrmmmm` | translated | UnpredictableInstruction | `thumb32_data_processing_register.cpp` |
| 159 | `thumb32_UXTB` | UXTB | `11111010010111111111dddd10rrmmmm` | translated | UnpredictableInstruction | `thumb32_data_processing_register.cpp` |
| 160 | `thumb32_UXTAB` | UXTAB | `111110100101nnnn1111dddd10rrmmmm` | translated | UnpredictableInstruction | `thumb32_data_processing_register.cpp` |
| 161 | `thumb32_SADD16` | SADD16 | `111110101001nnnn1111dddd0000mmmm` | translated | UnpredictableInstruction | `thumb32_parallel.cpp` |
| 162 | `thumb32_SASX` | SASX | `111110101010nnnn1111dddd0000mmmm` | translated | UnpredictableInstruction | `thumb32_parallel.cpp` |
| 163 | `thumb32_SSAX` | SSAX | `111110101110nnnn1111dddd0000mmmm` | translated | UnpredictableInstruction | `thumb32_parallel.cpp` |
| 164 | `thumb32_SSUB16` | SSUB16 | `111110101101nnnn1111dddd0000mmmm` | translated | UnpredictableInstruction | `thumb32_parallel.cpp` |
| 165 | `thumb32_SADD8` | SADD8 | `111110101000nnnn1111dddd0000mmmm` | translated | UnpredictableInstruction | `thumb32_parallel.cpp` |
| 166 | `thumb32_SSUB8` | SSUB8 | `111110101100nnnn1111dddd0000mmmm` | translated | UnpredictableInstruction | `thumb32_parallel.cpp` |
| 167 | `thumb32_QADD16` | QADD16 | `111110101001nnnn1111dddd0001mmmm` | translated | UnpredictableInstruction | `thumb32_parallel.cpp` |
| 168 | `thumb32_QASX` | QASX | `111110101010nnnn1111dddd0001mmmm` | translated | UnpredictableInstruction | `thumb32_parallel.cpp` |
| 169 | `thumb32_QSAX` | QSAX | `111110101110nnnn1111dddd0001mmmm` | translated | UnpredictableInstruction | `thumb32_parallel.cpp` |
| 170 | `thumb32_QSUB16` | QSUB16 | `111110101101nnnn1111dddd0001mmmm` | translated | UnpredictableInstruction | `thumb32_parallel.cpp` |
| 171 | `thumb32_QADD8` | QADD8 | `111110101000nnnn1111dddd0001mmmm` | translated | UnpredictableInstruction | `thumb32_parallel.cpp` |
| 172 | `thumb32_QSUB8` | QSUB8 | `111110101100nnnn1111dddd0001mmmm` | translated | UnpredictableInstruction | `thumb32_parallel.cpp` |
| 173 | `thumb32_SHADD16` | SHADD16 | `111110101001nnnn1111dddd0010mmmm` | translated | UnpredictableInstruction | `thumb32_parallel.cpp` |
| 174 | `thumb32_SHASX` | SHASX | `111110101010nnnn1111dddd0010mmmm` | translated | UnpredictableInstruction | `thumb32_parallel.cpp` |
| 175 | `thumb32_SHSAX` | SHSAX | `111110101110nnnn1111dddd0010mmmm` | translated | UnpredictableInstruction | `thumb32_parallel.cpp` |
| 176 | `thumb32_SHSUB16` | SHSUB16 | `111110101101nnnn1111dddd0010mmmm` | translated | UnpredictableInstruction | `thumb32_parallel.cpp` |
| 177 | `thumb32_SHADD8` | SHADD8 | `111110101000nnnn1111dddd0010mmmm` | translated | UnpredictableInstruction | `thumb32_parallel.cpp` |
| 178 | `thumb32_SHSUB8` | SHSUB8 | `111110101100nnnn1111dddd0010mmmm` | translated | UnpredictableInstruction | `thumb32_parallel.cpp` |
| 179 | `thumb32_UADD16` | UADD16 | `111110101001nnnn1111dddd0100mmmm` | translated | UnpredictableInstruction | `thumb32_parallel.cpp` |
| 180 | `thumb32_UASX` | UASX | `111110101010nnnn1111dddd0100mmmm` | translated | UnpredictableInstruction | `thumb32_parallel.cpp` |
| 181 | `thumb32_USAX` | USAX | `111110101110nnnn1111dddd0100mmmm` | translated | UnpredictableInstruction | `thumb32_parallel.cpp` |
| 182 | `thumb32_USUB16` | USUB16 | `111110101101nnnn1111dddd0100mmmm` | translated | UnpredictableInstruction | `thumb32_parallel.cpp` |
| 183 | `thumb32_UADD8` | UADD8 | `111110101000nnnn1111dddd0100mmmm` | translated | UnpredictableInstruction | `thumb32_parallel.cpp` |
| 184 | `thumb32_USUB8` | USUB8 | `111110101100nnnn1111dddd0100mmmm` | translated | UnpredictableInstruction | `thumb32_parallel.cpp` |
| 185 | `thumb32_UQADD16` | UQADD16 | `111110101001nnnn1111dddd0101mmmm` | translated | UnpredictableInstruction | `thumb32_parallel.cpp` |
| 186 | `thumb32_UQASX` | UQASX | `111110101010nnnn1111dddd0101mmmm` | translated | UnpredictableInstruction | `thumb32_parallel.cpp` |
| 187 | `thumb32_UQSAX` | UQSAX | `111110101110nnnn1111dddd0101mmmm` | translated | UnpredictableInstruction | `thumb32_parallel.cpp` |
| 188 | `thumb32_UQSUB16` | UQSUB16 | `111110101101nnnn1111dddd0101mmmm` | translated | UnpredictableInstruction | `thumb32_parallel.cpp` |
| 189 | `thumb32_UQADD8` | UQADD8 | `111110101000nnnn1111dddd0101mmmm` | translated | UnpredictableInstruction | `thumb32_parallel.cpp` |
| 190 | `thumb32_UQSUB8` | UQSUB8 | `111110101100nnnn1111dddd0101mmmm` | translated | UnpredictableInstruction | `thumb32_parallel.cpp` |
| 191 | `thumb32_UHADD16` | UHADD16 | `111110101001nnnn1111dddd0110mmmm` | translated | UnpredictableInstruction | `thumb32_parallel.cpp` |
| 192 | `thumb32_UHASX` | UHASX | `111110101010nnnn1111dddd0110mmmm` | translated | UnpredictableInstruction | `thumb32_parallel.cpp` |
| 193 | `thumb32_UHSAX` | UHSAX | `111110101110nnnn1111dddd0110mmmm` | translated | UnpredictableInstruction | `thumb32_parallel.cpp` |
| 194 | `thumb32_UHSUB16` | UHSUB16 | `111110101101nnnn1111dddd0110mmmm` | translated | UnpredictableInstruction | `thumb32_parallel.cpp` |
| 195 | `thumb32_UHADD8` | UHADD8 | `111110101000nnnn1111dddd0110mmmm` | translated | UnpredictableInstruction | `thumb32_parallel.cpp` |
| 196 | `thumb32_UHSUB8` | UHSUB8 | `111110101100nnnn1111dddd0110mmmm` | translated | UnpredictableInstruction | `thumb32_parallel.cpp` |
| 197 | `thumb32_QADD` | QADD | `111110101000nnnn1111dddd1000mmmm` | translated | UnpredictableInstruction | `thumb32_misc.cpp` |
| 198 | `thumb32_QDADD` | QDADD | `111110101000nnnn1111dddd1001mmmm` | translated | UnpredictableInstruction | `thumb32_misc.cpp` |
| 199 | `thumb32_QSUB` | QSUB | `111110101000nnnn1111dddd1010mmmm` | translated | UnpredictableInstruction | `thumb32_misc.cpp` |
| 200 | `thumb32_QDSUB` | QDSUB | `111110101000nnnn1111dddd1011mmmm` | translated | UnpredictableInstruction | `thumb32_misc.cpp` |
| 201 | `thumb32_REV` | REV | `111110101001nnnn1111dddd1000mmmm` | translated | UnpredictableInstruction | `thumb32_misc.cpp` |
| 202 | `thumb32_REV16` | REV16 | `111110101001nnnn1111dddd1001mmmm` | translated | UnpredictableInstruction | `thumb32_misc.cpp` |
| 203 | `thumb32_RBIT` | RBIT | `111110101001nnnn1111dddd1010mmmm` | translated | UnpredictableInstruction | `thumb32_misc.cpp` |
| 204 | `thumb32_REVSH` | REVSH | `111110101001nnnn1111dddd1011mmmm` | translated | UnpredictableInstruction | `thumb32_misc.cpp` |
| 205 | `thumb32_SEL` | SEL | `111110101010nnnn1111dddd1000mmmm` | translated | UnpredictableInstruction | `thumb32_misc.cpp` |
| 206 | `thumb32_CLZ` | CLZ | `111110101011nnnn1111dddd1000mmmm` | translated | UnpredictableInstruction | `thumb32_misc.cpp` |
| 207 | `thumb32_MUL` | MUL | `111110110000nnnn1111dddd0000mmmm` | translated | UnpredictableInstruction | `thumb32_multiply.cpp` |
| 208 | `thumb32_MLA` | MLA | `111110110000nnnnaaaadddd0000mmmm` | translated | UnpredictableInstruction | `thumb32_multiply.cpp` |
| 209 | `thumb32_MLS` | MLS | `111110110000nnnnaaaadddd0001mmmm` | translated | UnpredictableInstruction | `thumb32_multiply.cpp` |
| 210 | `thumb32_SMULXY` | SMULXY | `111110110001nnnn1111dddd00NMmmmm` | translated | UnpredictableInstruction | `thumb32_multiply.cpp` |
| 211 | `thumb32_SMLAXY` | SMLAXY | `111110110001nnnnaaaadddd00NMmmmm` | translated | UnpredictableInstruction | `thumb32_multiply.cpp` |
| 212 | `thumb32_SMUAD` | SMUAD | `111110110010nnnn1111dddd000Mmmmm` | translated | UnpredictableInstruction | `thumb32_multiply.cpp` |
| 213 | `thumb32_SMLAD` | SMLAD | `111110110010nnnnaaaadddd000Xmmmm` | translated | UnpredictableInstruction | `thumb32_multiply.cpp` |
| 214 | `thumb32_SMULWY` | SMULWY | `111110110011nnnn1111dddd000Mmmmm` | translated | UnpredictableInstruction | `thumb32_multiply.cpp` |
| 215 | `thumb32_SMLAWY` | SMLAWY | `111110110011nnnnaaaadddd000Mmmmm` | translated | UnpredictableInstruction | `thumb32_multiply.cpp` |
| 216 | `thumb32_SMUSD` | SMUSD | `111110110100nnnn1111dddd000Mmmmm` | translated | UnpredictableInstruction | `thumb32_multiply.cpp` |
| 217 | `thumb32_SMLSD` | SMLSD | `111110110100nnnnaaaadddd000Xmmmm` | translated | UnpredictableInstruction | `thumb32_multiply.cpp` |
| 218 | `thumb32_SMMUL` | SMMUL | `111110110101nnnn1111dddd000Rmmmm` | translated | UnpredictableInstruction | `thumb32_multiply.cpp` |
| 219 | `thumb32_SMMLA` | SMMLA | `111110110101nnnnaaaadddd000Rmmmm` | translated | UnpredictableInstruction | `thumb32_multiply.cpp` |
| 220 | `thumb32_SMMLS` | SMMLS | `111110110110nnnnaaaadddd000Rmmmm` | translated | UnpredictableInstruction | `thumb32_multiply.cpp` |
| 221 | `thumb32_USAD8` | USAD8 | `111110110111nnnn1111dddd0000mmmm` | translated | UnpredictableInstruction | `thumb32_multiply.cpp` |
| 222 | `thumb32_USADA8` | USADA8 | `111110110111nnnnaaaadddd0000mmmm` | translated | UnpredictableInstruction | `thumb32_multiply.cpp` |
| 223 | `thumb32_SMULL` | SMULL | `111110111000nnnnllllhhhh0000mmmm` | translated | UnpredictableInstruction | `thumb32_long_multiply.cpp` |
| 224 | `thumb32_SDIV` | SDIV | `111110111001nnnn1111dddd1111mmmm` | translated |  | `thumb32_long_multiply.cpp` |
| 225 | `thumb32_UMULL` | UMULL | `111110111010nnnnllllhhhh0000mmmm` | translated | UnpredictableInstruction | `thumb32_long_multiply.cpp` |
| 226 | `thumb32_UDIV` | UDIV | `111110111011nnnn1111dddd1111mmmm` | translated |  | `thumb32_long_multiply.cpp` |
| 227 | `thumb32_SMLAL` | SMLAL | `111110111100nnnnllllhhhh0000mmmm` | translated | UnpredictableInstruction | `thumb32_long_multiply.cpp` |
| 228 | `thumb32_SMLALXY` | SMLALXY | `111110111100nnnnllllhhhh10NMmmmm` | translated | UnpredictableInstruction | `thumb32_long_multiply.cpp` |
| 229 | `thumb32_SMLALD` | SMLALD | `111110111100nnnnllllhhhh110Mmmmm` | translated | UnpredictableInstruction | `thumb32_long_multiply.cpp` |
| 230 | `thumb32_SMLSLD` | SMLSLD | `111110111101nnnnllllhhhh110Mmmmm` | translated | UnpredictableInstruction | `thumb32_long_multiply.cpp` |
| 231 | `thumb32_UMLAL` | UMLAL | `111110111110nnnnllllhhhh0000mmmm` | translated | UnpredictableInstruction | `thumb32_long_multiply.cpp` |
| 232 | `thumb32_UMAAL` | UMAAL | `111110111110nnnnllllhhhh0110mmmm` | translated | UnpredictableInstruction | `thumb32_long_multiply.cpp` |
| 233 | `thumb32_MCRR` | MCRR | `111o11000100uuuuttttppppooooMMMM` | translated |  | `thumb32_coprocessor.cpp` |
| 234 | `thumb32_MRRC` | MRRC | `111o11000101uuuuttttppppooooMMMM` | translated |  | `thumb32_coprocessor.cpp` |
| 235 | `thumb32_STC` | STC | `111o110pudw0nnnnDDDDppppvvvvvvvv` | translated |  | `thumb32_coprocessor.cpp` |
| 236 | `thumb32_LDC` | LDC | `111o110pudw1nnnnDDDDppppvvvvvvvv` | translated |  | `thumb32_coprocessor.cpp` |
| 237 | `thumb32_CDP` | CDP | `111o1110ooooNNNNDDDDppppooo0MMMM` | translated |  | `thumb32_coprocessor.cpp` |
| 238 | `thumb32_MCR` | MCR | `111o1110ooo0NNNNttttppppooo1MMMM` | translated |  | `thumb32_coprocessor.cpp` |
| 239 | `thumb32_MRC` | MRC | `111o1110ooo1NNNNttttppppooo1MMMM` | translated |  | `thumb32_coprocessor.cpp` |
