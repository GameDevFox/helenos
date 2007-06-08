/*
 * Copyright (c) 2007 Petr Stepan, Pavel Jancik
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * - Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 * - Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in the
 *   documentation and/or other materials provided with the distribution.
 * - The name of the author may not be used to endorse or promote products
 *   derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/** @addtogroup arm32
 * @{
 */
/** @file
 *  @brief Userspace switch.
 */

#include <userspace.h>

/** Struct for holding all general purpose registers.
 *  
 *  Used to set registers when going to userspace.
 */
typedef struct {
	uint32_t r0;
	uint32_t r1;
	uint32_t r2;
	uint32_t r3;
	uint32_t r4;
	uint32_t r5;
	uint32_t r6;
	uint32_t r7;
	uint32_t r8;
	uint32_t r9;
	uint32_t r10;
	uint32_t r11;
	uint32_t r12;
	uint32_t sp;
	uint32_t lr;
	uint32_t pc;
} ustate_t;

/** Changes processor mode and jumps to the address specified in the first
 * parameter.
 *
 *  @param kernel_uarg	 Userspace settings (entry point, stack, ...).
 */
void userspace(uspace_arg_t *kernel_uarg)
{
	volatile ustate_t ustate;

	/* set first parameter */
	ustate.r0 = (uintptr_t) kernel_uarg->uspace_uarg;

	/* clear other registers */
	ustate.r1 = ustate.r2  = ustate.r3  = ustate.r4  = ustate.r5 =
	    ustate.r6  = ustate.r7  = ustate.r8  = ustate.r9 = ustate.r10 = 
	    ustate.r11 = ustate.r12 = ustate.lr = 0;

	/* set user stack */
	ustate.sp = ((uint32_t)kernel_uarg->uspace_stack) + PAGE_SIZE;

	/* set where uspace execution starts */
	ustate.pc = (uintptr_t) kernel_uarg->uspace_entry;

	/* status register in user mode */
	ipl_t user_mode = current_status_reg_read() &
	    (~STATUS_REG_MODE_MASK | USER_MODE);

	/* set user mode, set registers, jump */
	asm volatile (
		"mov r0, %0			\n"
		"msr spsr_c, %1			\n"
		"ldmfd r0!, {r0-r12, sp, lr}^	\n"
		"ldmfd r0!, {pc}^\n"
		:
		: "r" (&ustate), "r" (user_mode)
		: "r0", "r1"
	);

	/* unreachable */
	while(1)
		;
}

/** @}
 */
