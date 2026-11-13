@ Metrowerks ARM runtime helpers that the decomp's assembly still calls, in the soft-float
@ calling convention it was written for (floats in r0, doubles in r0:r1 / r2:r3), done with the
@ 3DS's VFP unit. The compare helpers return their result in the CPU flags (as an unsigned
@ compare would), which is exactly what vcmp + vmrs produce, and keep r0-r3 intact.
	.syntax unified
	.arch armv6k
	.fpu vfpv2
	.arm
	.text

.macro func name
	.global \name
	.type \name, %function
	.balign 4
\name:
.endm

@ ---- single precision ----
.macro fop2 name, op
func \name
	vmov s0, r0
	vmov s1, r1
	\op s0, s0, s1
	vmov r0, s0
	bx lr
.endm
	fop2 _fadd, vadd.f32
	fop2 _fsub, vsub.f32
	fop2 _fmul, vmul.f32
	fop2 _fdiv, vdiv.f32

.macro fcmp name
func \name
	vmov s0, r0
	vmov s1, r1
	vcmp.f32 s0, s1
	vmrs APSR_nzcv, fpscr
	bx lr
.endm
	fcmp _feq
	fcmp _fneq
	fcmp _fgr
	fcmp _fls
	fcmp _fleq
	fcmp _fgeq

func _fflt
	vmov s0, r0
	vcvt.f32.s32 s0, s0
	vmov r0, s0
	bx lr
func _ffltu
	vmov s0, r0
	vcvt.f32.u32 s0, s0
	vmov r0, s0
	bx lr
func _ffix
	vmov s0, r0
	vcvt.s32.f32 s0, s0
	vmov r0, s0
	bx lr
func _ffixu
	vmov s0, r0
	vcvt.u32.f32 s0, s0
	vmov r0, s0
	bx lr
func _f2d
	vmov s0, r0
	vcvt.f64.f32 d1, s0
	vmov r0, r1, d1
	bx lr

@ ---- double precision ----
.macro dop2 name, op
func \name
	vmov d0, r0, r1
	vmov d1, r2, r3
	\op d0, d0, d1
	vmov r0, r1, d0
	bx lr
.endm
	dop2 _dadd, vadd.f64
	dop2 _dsub, vsub.f64
	dop2 _dmul, vmul.f64
	dop2 _ddiv, vdiv.f64

.macro dcmp name
func \name
	vmov d0, r0, r1
	vmov d1, r2, r3
	vcmp.f64 d0, d1
	vmrs APSR_nzcv, fpscr
	bx lr
.endm
	dcmp _deq
	dcmp _dneq
	dcmp _dgr
	dcmp _dls
	dcmp _dleq
	dcmp _dgeq

func _dflt
	vmov s0, r0
	vcvt.f64.s32 d1, s0
	vmov r0, r1, d1
	bx lr
func _dfltu
	vmov s0, r0
	vcvt.f64.u32 d1, s0
	vmov r0, r1, d1
	bx lr
func _dfix
	vmov d0, r0, r1
	vcvt.s32.f64 s2, d0
	vmov r0, s2
	bx lr
func _dfixu
	vmov d0, r0, r1
	vcvt.u32.f64 s2, d0
	vmov r0, s2
	bx lr
func _d2f
	vmov d0, r0, r1
	vcvt.f32.f64 s2, d0
	vmov r0, s2
	bx lr

@ ---- 64-bit integers and division ----
func _f_lltof
	b __aeabi_l2f
func _f_ulltof
	b __aeabi_ul2f
func _ll_mul
	b __aeabi_lmul
func _ll_sdiv
	b __aeabi_ldivmod
func _ll_udiv
	b __aeabi_uldivmod
func _ll_mod
	push {r4, lr}
	bl __aeabi_ldivmod
	mov r0, r2
	mov r1, r3
	pop {r4, pc}
func _ull_mod
	push {r4, lr}
	bl __aeabi_uldivmod
	mov r0, r2
	mov r1, r3
	pop {r4, pc}
@ quotient in r0, remainder in r1, as __aeabi_*divmod return them
func _s32_div_f
	b __aeabi_idivmod
func _u32_div_f
	b __aeabi_uidivmod
