#ifndef __X86_FRAME_H
#define __X86_FRAME_H

#include "jit/stack-slot.h"
#include <stdbool.h>

struct vm_method;
struct expression;
struct compilation_unit;

struct native_stack_frame {
	void *prev; /* previous stack frame link */
	unsigned long return_address;
	unsigned long args[0];
} __attribute__((packed));

#ifdef CONFIG_X86_32

#define NR_TRAMPOLINE_LOCALS	0

struct jit_stack_frame {
	void *prev; /* previous stack frame link */
	unsigned long old_ebx;
	unsigned long old_esi;
	unsigned long old_edi;
	unsigned long return_address;
	unsigned long args[0];
} __attribute__((packed));

#else

#define NR_TRAMPOLINE_LOCALS	14

/*
 * FIXME: This no longer reflects the structure
 * of the stack frame and should be fixed, although
 * it provides the correct answer in most cases. It is a bug!
 */
struct jit_stack_frame {
	void *prev; /* previous stack frame link */
	unsigned long old_xmm15;
	unsigned long old_xmm14;
	unsigned long old_xmm13;
	unsigned long old_xmm12;
	unsigned long old_xmm11;
	unsigned long old_xmm10;
	unsigned long old_xmm9;
	unsigned long old_xmm8;
	unsigned long old_r15;
	unsigned long old_r14;
	unsigned long old_r13;
	unsigned long old_r12;
	unsigned long old_rbx;
	unsigned long this;
	unsigned long return_address;
	unsigned long args[0];
} __attribute__((packed));

#endif /* CONFIG_X86_32 */

unsigned long frame_local_offset(struct vm_method *, struct expression *);
unsigned long slot_offset(struct stack_slot *slot);
unsigned long slot_offset_64(struct stack_slot *slot);
unsigned long frame_locals_size(struct stack_frame *frame);
unsigned long cu_frame_locals_offset(struct compilation_unit *cu);

#endif
