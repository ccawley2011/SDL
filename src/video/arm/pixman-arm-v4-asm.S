/*
 * Copyright (c) 2016 RISC OS Open Ltd
 *
 * This software is provided 'as-is', without any express or implied
 * warranty.  In no event will the authors be held liable for any damages
 * arising from the use of this software.
 *
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 *
 * 1. The origin of this software must not be misrepresented; you must not
 *    claim that you wrote the original software. If you use this software
 *    in a product, an acknowledgment in the product documentation would be
 *    appreciated but is not required.
 * 2. Altered source versions must be plainly marked as such, and must not be
 *    misrepresented as being the original software.
 * 3. This notice may not be removed or altered from any source distribution.
 */

/* Prevent the stack from becoming executable */
#if defined(__linux__) && defined(__ELF__)
.section .note.GNU-stack,"",%progbits
#endif

	.text
	.arch armv4
	.object_arch armv4
	.arm
	.altmacro
	.p2align 2

#include "pixman-arm-asm.h"

/*
 * Offset into stack where mask and source pointer/stride can be accessed.
 */
.set ARGS_STACK_OFFSET,        (9*4)

.macro generate_fillrect_function name, dst_w_bpp, dst_bpp_shift
pixman_asm_function name

    X           .req    r0  /* pixels to go on this line */
    Y           .req    r1  /* lines to go */
    DST         .req    r2  /* destination pixel pointer */
    STRIDE_D    .req    r3  /* destination stride (bytes, minus width) */
    FILL1       .req    r4  /* colour to fill with */
    FILL2       .req    r5
    FILL3       .req    r6
    FILL4       .req    r7
    WK0         .req    r8  /* pixel data registers */
    SCRATCH     .req    r12
    ORIG_W      .req    r14 /* width (pixels) */

	push	{r4-r11, lr}        /* save all registers */
	subs    Y, Y, #1
	blo	294f

	/* Initialise the fill registers */
.if dst_w_bpp==32
	ldr	FILL1, [sp, #ARGS_STACK_OFFSET]
.elseif dst_w_bpp==16
	ldrh    FILL1, [sp, #ARGS_STACK_OFFSET]
	orr     FILL1, FILL1, lsl #16
.elseif dst_w_bpp==8
	ldrb    FILL1, [sp, #ARGS_STACK_OFFSET]
	orr     FILL1, FILL1, lsl #8
	orr     FILL1, FILL1, lsl #16
.endif
	mov	FILL2, FILL1
	mov	FILL3, FILL1
	mov	FILL4, FILL1

	lsl     STRIDE_D, #dst_bpp_shift /* stride in bytes */
	sub     STRIDE_D, STRIDE_D, X, lsl #dst_bpp_shift

	/* Are we not even wide enough to have one 16-byte aligned 16-byte block write? */
	cmp     X, #2*16*8/dst_w_bpp - 1
	blo     244f

	/* Medium case */
	mov     ORIG_W, X

200:	/* New line */
        sub     X, X, #128/dst_w_bpp     /* simplifies inner loop termination */
        ands    WK0, DST, #15
        beq     204f
        rsb     WK0, WK0, #16 /* number of leading bytes until destination aligned */


	/* Leading 15 bytes */
.if dst_w_bpp==16
        movs    SCRATCH, WK0, lsl #32-1  /* C,N = bits 1,0 of DST */
        subcs   X, X, #16/dst_w_bpp
	strcsh	FILL1, [DST], #2
.elseif dst_w_bpp==8
        movs    SCRATCH, WK0, lsl #32-1  /* C,N = bits 1,0 of DST */
        submi   X, X, #8/dst_w_bpp
	strmib	FILL1, [DST], #1
        subcs   X, X, #16/dst_w_bpp
	strcsh	FILL1, [DST], #2
.endif
        movs    SCRATCH, WK0, lsl #32-3  /* C,N = bits 3, 2 of DST */
        submi   X, X, #32/dst_w_bpp
	strmi	FILL1, [DST], #4
        subcs   X, X, #64/dst_w_bpp
	stmcsia	DST!, {FILL1, FILL2}

204:	/* Destination now 16-byte aligned; we have at least one 16-byte output block */
	stmia	DST!, {FILL1, FILL2, FILL3, FILL4}
        subs    X, X, #128/dst_w_bpp
        bhs     204b

        /* Trailing pixels */
        tst     X, #128/dst_w_bpp - 1
        beq     230f
        movs    SCRATCH, X, lsl #dst_bpp_shift+32-3
	stmcsia	DST!, {FILL1, FILL2}
	strmi	FILL1, [DST], #4
.if dst_w_bpp == 16
        movs    SCRATCH, X, lsr #1
	strcsh	FILL1, [DST], #2
.elseif dst_w_bpp == 8
        movs    SCRATCH, X, lsl #dst_bpp_shift+32-1
	strcsh	FILL1, [DST], #2
	strmib	FILL1, [DST], #1
.endif

230:	/* Check for another line */
	subs	Y, Y, #1
	add	DST, DST, STRIDE_D
	mov	X, ORIG_W
	bhs	200b
	b	294f

244:    /* Narrow case, less than 31 bytes, so no guarantee of at least one 16-byte block */
 .if dst_w_bpp < 32
        mov     ORIG_W, X
 .endif

248:    /* New line */
.if dst_w_bpp == 16
        tst     DST, #2
        beq     264f
        subs    X, X, #1
        blo     284f
	strh	FILL1, [DST], #2
.elseif dst_w_bpp == 8
	tst     DST, #3
        beq     264f
250:    subs    X, X, #1
        blo     284f
	strb	FILL1, [DST], #1
        tst     DST, #3
        bne     250b
.endif

264:	/* Destination now 4-byte aligned; we have 0 or more output bytes to go */
        tst     X, #16*8/dst_w_bpp
	stmneia	DST!, {FILL1, FILL2, FILL3, FILL4}

        /* Trailing pixels */
        movs    SCRATCH, X, lsl #dst_bpp_shift+32-3
	stmcsia	DST!, {FILL1, FILL2}
	strmi	FILL1, [DST], #4
.if dst_w_bpp == 16
        movs    SCRATCH, X, lsr #1
	strcsh	FILL1, [DST], #2
.elseif dst_w_bpp == 8
        movs    SCRATCH, X, lsl #dst_bpp_shift+32-1
	strcsh	FILL1, [DST], #2
	strmib	FILL1, [DST], #1
.endif

284:	/* Check for another line */
	subs	Y, Y, #1
	add	DST, DST, STRIDE_D
 .if dst_w_bpp < 32
	mov	X, ORIG_W
 .endif
	bhs	248b
294:	pop	{r4-r11, pc}  /* exit */

    .unreq  Y
    .unreq  DST
    .unreq  STRIDE_D
    .unreq  FILL1
    .unreq  FILL2
    .unreq  FILL3
    .unreq  FILL4
    .unreq  WK0
    .unreq  SCRATCH
    .unreq  ORIG_W
.endfunc
.endm

generate_fillrect_function FillRect32ARMv4Asm, 32, 2
generate_fillrect_function FillRect16ARMv4Asm, 16, 1
generate_fillrect_function FillRect8ARMv4Asm,  8,  0
