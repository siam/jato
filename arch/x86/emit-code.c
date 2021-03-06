/*
 * x86-32/x86-64 code emitter.
 *
 * Copyright (C) 2006-2009 Pekka Enberg
 * Copyright (C) 2008-2009 Arthur Huillet
 * Copyright (C) 2009 Eduard - Gabriel Munteanu
 *
 * This file is released under the GPL version 2 with the following
 * clarification and special exception:
 *
 *     Linking this library statically or dynamically with other modules is
 *     making a combined work based on this library. Thus, the terms and
 *     conditions of the GNU General Public License cover the whole
 *     combination.
 *
 *     As a special exception, the copyright holders of this library give you
 *     permission to link this library with independent modules to produce an
 *     executable, regardless of the license terms of these independent
 *     modules, and to copy and distribute the resulting executable under terms
 *     of your choice, provided that you also meet, for each linked independent
 *     module, the terms and conditions of the license of that module. An
 *     independent module is a module which is not derived from or based on
 *     this library. If you modify this library, you may extend this exception
 *     to your version of the library, but you are not obligated to do so. If
 *     you do not wish to do so, delete this exception statement from your
 *     version.
 *
 * Please refer to the file LICENSE for details.
 */

#include "arch/instruction.h"
#include "arch/stack-frame.h"
#include "arch/itable.h"
#include "arch/memory.h"
#include "arch/thread.h"
#include "arch/init.h"

#include "cafebabe/method_info.h"

#include "jit/compilation-unit.h"
#include "jit/basic-block.h"
#include "jit/stack-slot.h"
#include "jit/statement.h"
#include "jit/compiler.h"
#include "jit/exception.h"
#include "jit/emit-code.h"
#include "jit/text.h"

#include "lib/buffer.h"
#include "lib/list.h"

#include "vm/backtrace.h"
#include "vm/method.h"
#include "vm/object.h"

#include <stdbool.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>

#define	REX		0x40
#define REX_W		(REX | 8)	/* 64-bit operands */
#define REX_R		(REX | 4)	/* ModRM reg extension */
#define REX_X		(REX | 2)	/* SIB index extension */
#define REX_B		(REX | 1)	/* ModRM r/m extension */

/* Aliases and prototypes to make common emitters work as expected. */
#ifdef CONFIG_X86_64
# define __emit_add_imm_reg		__emit64_add_imm_reg
# define __emit_pop_reg			__emit64_pop_reg
# define __emit_push_imm		__emit64_push_imm
# define __emit_push_membase		__emit64_push_membase
# define __emit_push_reg		__emit64_push_reg
# define __emit_mov_reg_reg		__emit64_mov_reg_reg
#endif

typedef void (*emit_fn_t)(struct insn *insn, struct buffer *, struct basic_block *bb);

struct emitter {
	emit_fn_t emit_fn;
};

extern struct emitter emitters[];

#define DECL_EMITTER(_insn_type, _fn) \
	[_insn_type] = { .emit_fn = _fn }

static void __emit_add_imm_reg(struct buffer *buf,
			       long imm,
			       enum machine_reg reg);
static void __emit_pop_reg(struct buffer *buf, enum machine_reg reg);
static void __emit_push_imm(struct buffer *buf, long imm);
#ifdef CONFIG_X86_32
static void __emit_push_membase(struct buffer *buf,
				enum machine_reg src_reg,
				unsigned long disp);
#endif
static void __emit_push_reg(struct buffer *buf, enum machine_reg reg);
static void __emit_mov_reg_reg(struct buffer *buf,
			       enum machine_reg src,
			       enum machine_reg dst);
static void emit_indirect_jump_reg(struct buffer *buf, enum machine_reg reg);
static void emit_exception_test(struct buffer *buf, enum machine_reg reg);
static void emit_restore_regs(struct buffer *buf);

/*
 * Common code emitters
 */

#define PREFIX_SIZE 1
#define BRANCH_INSN_SIZE 5
#define BRANCH_TARGET_OFFSET 1

#define CALL_INSN_SIZE 5

#define PTR_SIZE	sizeof(long)

#define GENERIC_X86_EMITTERS \
	DECL_EMITTER(INSN_CALL_REL, emit_call),		\
	DECL_EMITTER(INSN_JE_BRANCH, emit_je_branch),		\
	DECL_EMITTER(INSN_JNE_BRANCH, emit_jne_branch),		\
	DECL_EMITTER(INSN_JGE_BRANCH, emit_jge_branch),		\
	DECL_EMITTER(INSN_JG_BRANCH, emit_jg_branch),		\
	DECL_EMITTER(INSN_JLE_BRANCH, emit_jle_branch),		\
	DECL_EMITTER(INSN_JL_BRANCH, emit_jl_branch),		\
	DECL_EMITTER(INSN_JMP_BRANCH, emit_jmp_branch),		\
	DECL_EMITTER(INSN_RET, emit_ret)

/*
 *	encode_mach_reg: Encode register to be used in x86 instruction.
 *	@reg: Register to encode.
 *
 *	Returns register in r/m or reg/opcode field format of the ModR/M byte.
 */
static unsigned char encode_mach_reg(enum machine_reg reg)
{
	static unsigned char register_numbers[] = {
		[MACH_REG_xAX]		= 0x00,
		[MACH_REG_xCX]		= 0x01,
		[MACH_REG_xDX]		= 0x02,
		[MACH_REG_xBX]		= 0x03,
		[MACH_REG_xSP]		= 0x04,
		[MACH_REG_xBP]		= 0x05,
		[MACH_REG_xSI]		= 0x06,
		[MACH_REG_xDI]		= 0x07,
#ifdef CONFIG_X86_64
		[MACH_REG_R8]		= 0x08,
		[MACH_REG_R9]		= 0x09,
		[MACH_REG_R10]		= 0x0A,
		[MACH_REG_R11]		= 0x0B,
		[MACH_REG_R12]		= 0x0C,
		[MACH_REG_R13]		= 0x0D,
		[MACH_REG_R14]		= 0x0E,
		[MACH_REG_R15]		= 0x0F,
#endif
		/* XMM registers */
		[MACH_REG_XMM0] 	= 0x00,
		[MACH_REG_XMM1] 	= 0x01,
		[MACH_REG_XMM2] 	= 0x02,
		[MACH_REG_XMM3] 	= 0x03,
		[MACH_REG_XMM4] 	= 0x04,
		[MACH_REG_XMM5] 	= 0x05,
		[MACH_REG_XMM6] 	= 0x06,
		[MACH_REG_XMM7] 	= 0x07,
#ifdef CONFIG_X86_64
		[MACH_REG_XMM8] 	= 0x08,
		[MACH_REG_XMM9] 	= 0x09,
		[MACH_REG_XMM10] 	= 0x0A,
		[MACH_REG_XMM11] 	= 0x0B,
		[MACH_REG_XMM12]	= 0x0C,
		[MACH_REG_XMM13] 	= 0x0D,
		[MACH_REG_XMM14]	= 0x0E,
		[MACH_REG_XMM15] 	= 0x0F,
#endif
	};

	if (reg == MACH_REG_UNASSIGNED)
		die("unassigned register during code emission");

	if (reg < 0 || reg >= ARRAY_SIZE(register_numbers))
		die("unknown register %d", reg);

	return register_numbers[reg];
}

#ifdef CONFIG_X86_32
static unsigned char encode_reg(struct use_position *reg)
{
	return encode_mach_reg(mach_reg(reg));
}
#endif

static inline bool is_imm_8(long imm)
{
	return (imm >= -128) && (imm <= 127);
}

/**
 *	encode_modrm:	Encode a ModR/M byte of an IA-32 instruction.
 *	@mod: The mod field of the byte.
 *	@reg_opcode: The reg/opcode field of the byte.
 *	@rm: The r/m field of the byte.
 */
static inline unsigned char encode_modrm(unsigned char mod,
					 unsigned char reg_opcode,
					 unsigned char rm)
{
	return ((mod & 0x3) << 6) | ((reg_opcode & 0x7) << 3) | (rm & 0x7);
}

static inline unsigned char encode_sib(unsigned char scale,
				       unsigned char index, unsigned char base)
{
	return ((scale & 0x3) << 6) | ((index & 0x7) << 3) | (base & 0x7);
}

static inline void emit(struct buffer *buf, unsigned char c)
{
	int err;

	err = append_buffer(buf, c);
	assert(!err);
}

static void write_imm32(struct buffer *buf, unsigned long offset, long imm32)
{
	unsigned char *buffer;
	union {
		int val;
		unsigned char b[4];
	} imm_buf;

	buffer = buf->buf;
	imm_buf.val = imm32;

	buffer[offset] = imm_buf.b[0];
	buffer[offset + 1] = imm_buf.b[1];
	buffer[offset + 2] = imm_buf.b[2];
	buffer[offset + 3] = imm_buf.b[3];
}

static void emit_imm32(struct buffer *buf, int imm)
{
	union {
		int val;
		unsigned char b[4];
	} imm_buf;

	imm_buf.val = imm;
	emit(buf, imm_buf.b[0]);
	emit(buf, imm_buf.b[1]);
	emit(buf, imm_buf.b[2]);
	emit(buf, imm_buf.b[3]);
}

static void emit_imm(struct buffer *buf, long imm)
{
	if (is_imm_8(imm))
		emit(buf, imm);
	else
		emit_imm32(buf, imm);
}

static void __emit_call(struct buffer *buf, void *call_target)
{
	int disp = call_target - buffer_current(buf) - CALL_INSN_SIZE;

	emit(buf, 0xe8);
	emit_imm32(buf, disp);
}

static void emit_call(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	__emit_call(buf, (void *)insn->operand.rel);
}

static void encode_ret(struct buffer *buf)
{
	emit(buf, 0xc3);
}

static void emit_ret(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	encode_ret(buf);
}

static void emit_leave(struct buffer *b)
{
	emit(b, 0xc9);
}

static void emit_branch_rel(struct buffer *buf, unsigned char prefix,
			    unsigned char opc, long rel32)
{
	if (prefix)
		emit(buf, prefix);
	emit(buf, opc);
	emit_imm32(buf, rel32);
}

static long branch_rel_addr(struct insn *insn, unsigned long target_offset)
{
	long ret;

	ret = target_offset - insn->mach_offset - BRANCH_INSN_SIZE;
	if (insn->flags & INSN_FLAG_ESCAPED)
		ret -= PREFIX_SIZE;

	return ret;
}

static void __emit_branch(struct buffer *buf, struct basic_block *bb,
		unsigned char prefix, unsigned char opc, struct insn *insn)
{
	struct basic_block *target_bb;
	long addr = 0;
	int idx;

	if (prefix)
		insn->flags |= INSN_FLAG_ESCAPED;

	target_bb = insn->operand.branch_target;

	if (!bb)
		idx = -1;
	else
		idx = bb_lookup_successor_index(bb, target_bb);

	if (idx >= 0 && branch_needs_resolution_block(bb, idx)) {
		list_add(&insn->branch_list_node,
			 &bb->resolution_blocks[idx].backpatch_insns);
	} else if (target_bb->is_emitted) {
		struct insn *target_insn =
		    list_first_entry(&target_bb->insn_list, struct insn,
			       insn_list_node);

		addr = branch_rel_addr(insn, target_insn->mach_offset);
	} else
		list_add(&insn->branch_list_node, &target_bb->backpatch_insns);

	emit_branch_rel(buf, prefix, opc, addr);
}

static void emit_je_branch(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	__emit_branch(buf, bb, 0x0f, 0x84, insn);
}

static void emit_jne_branch(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	__emit_branch(buf, bb, 0x0f, 0x85, insn);
}

static void emit_jge_branch(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	__emit_branch(buf, bb, 0x0f, 0x8d, insn);
}

static void emit_jg_branch(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	__emit_branch(buf, bb, 0x0f, 0x8f, insn);
}

static void emit_jle_branch(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	__emit_branch(buf, bb, 0x0f, 0x8e, insn);
}

static void emit_jl_branch(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	__emit_branch(buf, bb, 0x0f, 0x8c, insn);
}

static void emit_jmp_branch(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	__emit_branch(buf, bb, 0x00, 0xe9, insn);
}

void backpatch_branch_target(struct buffer *buf,
			     struct insn *insn,
			     unsigned long target_offset)
{
	unsigned long backpatch_offset;
	long relative_addr;

	backpatch_offset = insn->mach_offset + BRANCH_TARGET_OFFSET;
	if (insn->flags & INSN_FLAG_ESCAPED)
		backpatch_offset += PREFIX_SIZE;

	relative_addr = branch_rel_addr(insn, target_offset);

	write_imm32(buf, backpatch_offset, relative_addr);
}

static void __emit_jmp(struct buffer *buf, unsigned long addr)
{
	unsigned long current = (unsigned long)buffer_current(buf);
	emit(buf, 0xE9);
	emit_imm32(buf, addr - current - BRANCH_INSN_SIZE);
}

void emit_unwind(struct buffer *buf)
{
	emit_leave(buf);
	emit_restore_regs(buf);
	__emit_jmp(buf, (unsigned long)&unwind);
}

void emit_trace_invoke(struct buffer *buf, struct compilation_unit *cu)
{
	__emit_push_imm(buf, (unsigned long) cu);
	__emit_call(buf, &trace_invoke);
	__emit_add_imm_reg(buf, PTR_SIZE, MACH_REG_xSP);
}

/*
 * This fixes relative calls generated by EXPR_INVOKE.
 *
 * Please note that this code does not care about icache flushing in SMP
 * environment. This may lead to a GPF when one CPU modifies code already
 * prefetched by another CPU on some bogus Intel CPUs (see section 7.1.3 of
 * "Intel 64 and IA-32 Architectures Software Developers Manual Volume 3A").
 *
 * It is required for other CPUs to execute a serializing instruction (to flush
 * instruction cache) between modification and execution of new instruction. To
 * achieve this, we could suspend all threads before patching, and force them
 * to execute flush_icache() on resume.
 */
void fixup_direct_calls(struct jit_trampoline *t, unsigned long target)
{
	struct fixup_site *this, *next;

	pthread_mutex_lock(&t->mutex);

	list_for_each_entry_safe(this, next, &t->fixup_site_list, trampoline_node) {
		unsigned char *site_addr;
		uint32_t new_target;

		/*
		 * It is possible that we're fixing calls to method X() and
		 * another thread is compiling method Y() which calls X(). Call
		 * sites from Y might be added to X's trampoline but Y's
		 * ->objcode might not be set yet. We should skip fixing
		 *  callsites coming from not yet compiled methods.
		 */
		if (!fixup_site_is_ready(this))
			continue;

		site_addr = fixup_site_addr(this);
		new_target = target - ((unsigned long) site_addr + CALL_INSN_SIZE);
		cpu_write_u32(site_addr+1, new_target);

		list_del(&this->trampoline_node);
		free_fixup_site(this);
	}

	pthread_mutex_unlock(&t->mutex);
}

static void do_fixup_static(void *site_addr, int skip_count, void *new_target)
{
	void *p = site_addr + skip_count;

#ifdef CONFIG_X86_64
	/* We need RIP-relative addressing. */
	cpu_write_u32(p, new_target - site_addr - (skip_count + 4));
#else
	cpu_write_u32(p, (unsigned long) new_target);
#endif
}

static inline bool is_rex_prefix(unsigned char opc)
{
#ifdef CONFIG_X86_64
	return (opc & 0xf0) == REX;
#else
	return false;
#endif
}

static inline bool is_sse_insn(unsigned char *opc)
{
	return (opc[0] & 0xfe) == 0xf2 && opc[1] == 0x0f;
}

void fixup_static(struct vm_class *vmc)
{
	struct static_fixup_site *this, *next;

	pthread_mutex_lock(&vmc->mutex);

	list_for_each_entry_safe(this, next, &vmc->static_fixup_site_list, vmc_node) {
		struct vm_field *vmf = this->vmf;
		unsigned char *mach_insn;
		int skip_count = 0;
		void *new_target;

		new_target	= vmc->static_values + vmf->offset;
		mach_insn	= buffer_ptr(this->cu->objcode) + this->mach_offset;

		/* Does the instruction begin with a REX prefix? */
		if (is_rex_prefix(*mach_insn))
			skip_count += 1;

		/* Is it an SSE instruction? */
		if (is_sse_insn(mach_insn + skip_count))
			skip_count += 4;
		else
			skip_count += 2;

		do_fixup_static(mach_insn, skip_count, new_target);

		list_del(&this->vmc_node);

		pthread_mutex_lock(&this->cu->mutex);
		list_del(&this->cu_node);
		pthread_mutex_unlock(&this->cu->mutex);

		free(this);
	}

	pthread_mutex_unlock(&vmc->mutex);
}

int fixup_static_at(unsigned long addr)
{
	struct compilation_unit *cu;
	struct static_fixup_site *this, *t;

	cu = jit_lookup_cu(addr);
	assert(cu);

	pthread_mutex_lock(&cu->mutex);

	list_for_each_entry_safe(this, t, &cu->static_fixup_site_list, cu_node) {
		void *site_addr;

		site_addr = buffer_ptr(cu->objcode) + this->mach_offset;

		if ((unsigned long) site_addr == addr) {
			struct vm_class *vmc = this->vmf->class;
			int ret;

			pthread_mutex_unlock(&cu->mutex);

			/*
			 * Note: After this call, we can no longer access
			 * "this" because it may have been deleted already
			 * (from insite the class initializer of "vmc".
			 */
			ret = vm_class_ensure_init(vmc);
			if (ret)
				return ret;

			fixup_static(vmc);

			return 0;
		}
	}

	pthread_mutex_unlock(&cu->mutex);

	return 0;
}

#ifdef CONFIG_X86_32

/*
 * x86-32 code emitters
 */

static void emit_mov_reg_membase(struct insn *insn, struct buffer *buf, struct basic_block *bb);
static void emit_mov_imm_membase(struct insn *insn, struct buffer *buf, struct basic_block *bb);

static void __emit_mov_imm_membase(struct buffer *buf, long imm,
				   enum machine_reg base, long disp);

static void
__emit_reg_reg(struct buffer *buf, unsigned char opc,
	       enum machine_reg direct_reg, enum machine_reg rm_reg)
{
	unsigned char mod_rm;

	mod_rm = encode_modrm(0x03, encode_mach_reg(direct_reg), encode_mach_reg(rm_reg));

	emit(buf, opc);
	emit(buf, mod_rm);
}

static void
emit_reg_reg(struct buffer *buf, unsigned char opc,
	     struct operand *direct, struct operand *rm)
{
	enum machine_reg direct_reg, rm_reg;

	direct_reg = mach_reg(&direct->reg);
	rm_reg = mach_reg(&rm->reg);

	__emit_reg_reg(buf, opc, direct_reg, rm_reg);
}

static void
__emit_memdisp(struct buffer *buf, unsigned char opc, unsigned long disp,
	       unsigned char reg_opcode)
{
	unsigned char mod_rm;

	mod_rm = encode_modrm(0, reg_opcode, 5);

	emit(buf, opc);
	emit(buf, mod_rm);
	emit_imm32(buf, disp);
}

static void
__emit_memdisp_reg(struct buffer *buf, unsigned char opc, unsigned long disp,
		   enum machine_reg reg)
{
	__emit_memdisp(buf, opc, disp, encode_mach_reg(reg));
}

static void
__emit_reg_memdisp(struct buffer *buf, unsigned char opc, enum machine_reg reg,
		   unsigned long disp)
{
	__emit_memdisp(buf, opc, disp, encode_mach_reg(reg));
}

static void
__emit_membase(struct buffer *buf, unsigned char opc,
	       enum machine_reg base_reg, unsigned long disp,
	       unsigned char reg_opcode)
{
	unsigned char mod, rm, mod_rm;
	int needs_sib;

	needs_sib = (base_reg == MACH_REG_ESP);

	emit(buf, opc);

	if (needs_sib)
		rm = 0x04;
	else
		rm = encode_mach_reg(base_reg);

	if (disp == 0 && base_reg != MACH_REG_EBP)
		mod = 0x00;
	else if (is_imm_8(disp))
		mod = 0x01;
	else
		mod = 0x02;

	mod_rm = encode_modrm(mod, reg_opcode, rm);
	emit(buf, mod_rm);

	if (needs_sib)
		emit(buf, encode_sib(0x00, 0x04, encode_mach_reg(base_reg)));

	if (disp)
		emit_imm(buf, disp);
}

static void
__emit_membase_reg(struct buffer *buf, unsigned char opc,
		   enum machine_reg base_reg, unsigned long disp,
		   enum machine_reg dest_reg)
{
	__emit_membase(buf, opc, base_reg, disp, encode_mach_reg(dest_reg));
}

static void
emit_membase_reg(struct buffer *buf, unsigned char opc, struct operand *src,
		 struct operand *dest)
{
	enum machine_reg base_reg, dest_reg;
	unsigned long disp;

	base_reg = mach_reg(&src->base_reg);
	disp = src->disp;
	dest_reg = mach_reg(&dest->reg);

	__emit_membase_reg(buf, opc, base_reg, disp, dest_reg);
}

static void __emit_push_reg(struct buffer *buf, enum machine_reg reg)
{
	emit(buf, 0x50 + encode_mach_reg(reg));
}

static void __emit_push_membase(struct buffer *buf, enum machine_reg src_reg,
				unsigned long disp)
{
	__emit_membase(buf, 0xff, src_reg, disp, 6);
}

static void __emit_pop_reg(struct buffer *buf, enum machine_reg reg)
{
	emit(buf, 0x58 + encode_mach_reg(reg));
}

static void emit_push_reg(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	__emit_push_reg(buf, mach_reg(&insn->operand.reg));
}

static void __emit_mov_reg_reg(struct buffer *buf, enum machine_reg src_reg,
			       enum machine_reg dest_reg)
{
	__emit_reg_reg(buf, 0x89, src_reg, dest_reg);
}

static void emit_mov_reg_reg(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	__emit_mov_reg_reg(buf, mach_reg(&insn->src.reg), mach_reg(&insn->dest.reg));
}

static void emit_movsx_8_reg_reg(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	enum machine_reg src_reg = mach_reg(&insn->src.reg);

	emit(buf, 0x0f);

	/* We probably don't want %dh and %bh here. */
	assert(src_reg != MACH_REG_ESI && src_reg != MACH_REG_EDI);

	__emit_reg_reg(buf, 0xbe, mach_reg(&insn->dest.reg), src_reg);
}

static void emit_movsx_8_membase_reg(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	enum machine_reg base_reg, dest_reg;
	unsigned long disp;

	base_reg = mach_reg(&insn->src.reg);
	disp = insn->src.disp;
	dest_reg = mach_reg(&insn->dest.reg);

	emit(buf, 0x0f);
	__emit_membase_reg(buf, 0xbe, base_reg, disp, encode_mach_reg(dest_reg));
}

static void emit_movsx_16_reg_reg(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	emit(buf, 0x0f);
	__emit_reg_reg(buf, 0xbf, mach_reg(&insn->dest.reg), mach_reg(&insn->src.reg));
}

static void emit_movsx_16_membase_reg(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	enum machine_reg base_reg, dest_reg;
	unsigned long disp;

	base_reg = mach_reg(&insn->src.reg);
	disp = insn->src.disp;
	dest_reg = mach_reg(&insn->dest.reg);

	emit(buf, 0x0f);
	__emit_membase_reg(buf, 0xbf, base_reg, disp, dest_reg);
}

static void emit_movzx_16_reg_reg(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	emit(buf, 0x0f);
	__emit_reg_reg(buf, 0xb7, mach_reg(&insn->dest.reg), mach_reg(&insn->src.reg));
}

static void emit_mov_memlocal_reg(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	enum machine_reg dest_reg;
	unsigned long disp;

	dest_reg = mach_reg(&insn->dest.reg);
	disp = slot_offset(insn->src.slot);

	__emit_membase_reg(buf, 0x8b, MACH_REG_EBP, disp, dest_reg);
}

static void emit_mov_memlocal_xmm(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	enum machine_reg dest_reg;
	unsigned long disp;

	dest_reg = mach_reg(&insn->dest.reg);
	disp = slot_offset(insn->src.slot);

	emit(buf, 0xf3);
	emit(buf, 0x0f);
	__emit_membase_reg(buf, 0x10, MACH_REG_EBP, disp, dest_reg);
}

static void emit_mov_64_memlocal_xmm(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	enum machine_reg dest_reg;
	unsigned long disp;

	dest_reg = mach_reg(&insn->dest.reg);
	disp = slot_offset_64(insn->src.slot);

	emit(buf, 0xf2);
	emit(buf, 0x0f);
	__emit_membase_reg(buf, 0x10, MACH_REG_EBP, disp, dest_reg);
}

static void emit_mov_membase_reg(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	emit_membase_reg(buf, 0x8b, &insn->src, &insn->dest);
}

static void emit_mov_thread_local_memdisp_reg(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	emit(buf, 0x65); /* GS segment override prefix */
	__emit_memdisp_reg(buf, 0x8b, insn->src.imm, mach_reg(&insn->dest.reg));
}

static void emit_mov_reg_thread_local_memdisp(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	emit(buf, 0x65); /* GS segment override prefix */
	__emit_reg_memdisp(buf, 0x89, mach_reg(&insn->src.reg), insn->dest.imm);
}

static void emit_mov_reg_thread_local_membase(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	emit(buf, 0x65); /* GS segment override prefix */
	emit_mov_reg_membase(insn, buf, bb);
}

static void emit_mov_imm_thread_local_membase(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	emit(buf, 0x65); /* GS segment override prefix */
	emit_mov_imm_membase(insn, buf, bb);
}

static void emit_mov_memdisp_reg(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	__emit_memdisp_reg(buf, 0x8b, insn->src.imm, mach_reg(&insn->dest.reg));
}

static void emit_mov_reg_memdisp(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	__emit_reg_memdisp(buf, 0x89, mach_reg(&insn->src.reg), insn->dest.imm);
}

static void emit_mov_memindex_reg(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	emit(buf, 0x8b);
	emit(buf, encode_modrm(0x00, encode_reg(&insn->dest.reg), 0x04));
	emit(buf, encode_sib(insn->src.shift, encode_reg(&insn->src.index_reg), encode_reg(&insn->src.base_reg)));
}

static void __emit_mov_imm_reg(struct buffer *buf, long imm, enum machine_reg reg)
{
	emit(buf, 0xb8 + encode_mach_reg(reg));
	emit_imm32(buf, imm);
}

static void emit_mov_imm_reg(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	__emit_mov_imm_reg(buf, insn->src.imm, mach_reg(&insn->dest.reg));
}

static void __emit_mov_imm_membase(struct buffer *buf, long imm,
				   enum machine_reg base, long disp)
{
	__emit_membase(buf, 0xc7, base, disp, 0);
	emit_imm32(buf, imm);
}

static void emit_mov_imm_membase(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	__emit_mov_imm_membase(buf, insn->src.imm, mach_reg(&insn->dest.base_reg), insn->dest.disp);
}

static void emit_mov_imm_memlocal(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	__emit_mov_imm_membase(buf, insn->src.imm, MACH_REG_EBP, slot_offset(insn->dest.slot));
}

static void __emit_mov_reg_membase(struct buffer *buf, enum machine_reg src,
				   enum machine_reg base, unsigned long disp)
{
	__emit_membase(buf, 0x89, base, disp, encode_mach_reg(src));
}

static void emit_mov_reg_membase(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	__emit_mov_reg_membase(buf, mach_reg(&insn->src.reg), mach_reg(&insn->dest.base_reg), insn->dest.disp);
}

static void emit_mov_reg_memlocal(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	__emit_mov_reg_membase(buf, mach_reg(&insn->src.reg), MACH_REG_EBP, slot_offset(insn->dest.slot));
}

static void emit_mov_xmm_memlocal(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	unsigned long disp;
	int mod;

	disp = slot_offset(insn->dest.slot);

	if (is_imm_8(disp))
		mod = 0x01;
	else
		mod = 0x02;

	emit(buf, 0xf3);
	emit(buf, 0x0f);
	emit(buf, 0x11);
	emit(buf, encode_modrm(mod, encode_reg(&insn->src.reg),
			       encode_mach_reg(MACH_REG_EBP)));

	emit_imm(buf, disp);
}

static void emit_mov_64_xmm_memlocal(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	unsigned long disp;
	int mod;

	disp = slot_offset_64(insn->dest.slot);

	if (is_imm_8(disp))
		mod = 0x01;
	else
		mod = 0x02;

	emit(buf, 0xf2);
	emit(buf, 0x0f);
	emit(buf, 0x11);
	emit(buf, encode_modrm(mod, encode_reg(&insn->src.reg),
			       encode_mach_reg(MACH_REG_EBP)));

	emit_imm(buf, disp);
}

static void emit_mov_reg_memindex(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	emit(buf, 0x89);
	emit(buf, encode_modrm(0x00, encode_reg(&insn->src.reg), 0x04));
	emit(buf, encode_sib(insn->dest.shift, encode_reg(&insn->dest.index_reg), encode_reg(&insn->dest.base_reg)));
}

static void emit_alu_imm_reg(struct buffer *buf, unsigned char opc_ext,
			     long imm, enum machine_reg reg)
{
	int opc;

	if (is_imm_8(imm))
		opc = 0x83;
	else
		opc = 0x81;

	emit(buf, opc);
	emit(buf, encode_modrm(0x3, opc_ext, encode_mach_reg(reg)));
	emit_imm(buf, imm);
}

static void __emit_sub_imm_reg(struct buffer *buf, unsigned long imm,
			       enum machine_reg reg)
{
	emit_alu_imm_reg(buf, 0x05, imm, reg);
}

static void emit_sub_imm_reg(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	__emit_sub_imm_reg(buf, insn->src.imm, mach_reg(&insn->dest.reg));
}

static void emit_sub_reg_reg(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	emit_reg_reg(buf, 0x29, &insn->src, &insn->dest);
}

static void __emit_sbb_imm_reg(struct buffer *buf, unsigned long imm,
			       enum machine_reg reg)
{
	emit_alu_imm_reg(buf, 0x03, imm, reg);
}

static void emit_sbb_imm_reg(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	__emit_sbb_imm_reg(buf, insn->src.imm, mach_reg(&insn->dest.reg));
}

static void emit_sbb_reg_reg(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	emit_reg_reg(buf, 0x19, &insn->src, &insn->dest);
}

static void __emit_test_imm_memdisp(struct buffer *buf,
	long imm, long disp)
{
	/* XXX: Supports only byte or long imms */

	if (is_imm_8(imm))
		emit(buf, 0xf6);
	else
		emit(buf, 0xf7);

	emit(buf, 0x04);
	emit(buf, 0x25);
	emit_imm32(buf, disp);
	emit_imm(buf, imm);
}

static void emit_test_imm_memdisp(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	__emit_test_imm_memdisp(buf, insn->src.imm, insn->dest.disp);
}

void emit_prolog(struct buffer *buf, unsigned long nr_locals)
{
	/* Unconditionally push callee-saved registers */
	__emit_push_reg(buf, MACH_REG_EDI);
	__emit_push_reg(buf, MACH_REG_ESI);
	__emit_push_reg(buf, MACH_REG_EBX);

	__emit_push_reg(buf, MACH_REG_EBP);
	__emit_mov_reg_reg(buf, MACH_REG_ESP, MACH_REG_EBP);

	if (nr_locals)
		__emit_sub_imm_reg(buf, nr_locals * sizeof(unsigned long), MACH_REG_ESP);
}

void emit_epilog(struct buffer *buf)
{
	emit_leave(buf);
	emit_restore_regs(buf);
	encode_ret(buf);
}

static void emit_pop_memlocal(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	unsigned long disp = slot_offset(insn->operand.slot);

	__emit_membase(buf, 0x8f, MACH_REG_EBP, disp, 0);
}

static void emit_push_memlocal(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	unsigned long disp = slot_offset(insn->operand.slot);

	__emit_membase(buf, 0xff, MACH_REG_EBP, disp, 6);
}

static void emit_pop_reg(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	__emit_pop_reg(buf, mach_reg(&insn->operand.reg));
}

static void __emit_push_imm(struct buffer *buf, long imm)
{
	unsigned char opc;

	if (is_imm_8(imm))
		opc = 0x6a;
	else
		opc = 0x68;

	emit(buf, opc);
	emit_imm(buf, imm);
}

static void emit_push_imm(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	__emit_push_imm(buf, insn->operand.imm);
}

static void emit_restore_regs(struct buffer *buf)
{
	__emit_pop_reg(buf, MACH_REG_EBX);
	__emit_pop_reg(buf, MACH_REG_ESI);
	__emit_pop_reg(buf, MACH_REG_EDI);
}

static void emit_adc_reg_reg(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	emit_reg_reg(buf, 0x13, &insn->dest, &insn->src);
}

static void emit_adc_membase_reg(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	emit_membase_reg(buf, 0x13, &insn->src, &insn->dest);
}

static void emit_add_reg_reg(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	emit_reg_reg(buf, 0x03, &insn->dest, &insn->src);
}

static void emit_fadd_reg_reg(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	emit(buf, 0xf3);
	emit(buf, 0x0f);
	emit_reg_reg(buf, 0x58, &insn->dest, &insn->src);
}

static void emit_fadd_64_reg_reg(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	emit(buf, 0xf2);
	emit(buf, 0x0f);
	emit_reg_reg(buf, 0x58, &insn->dest, &insn->src);
}

static void emit_fadd_64_memdisp_reg(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	emit(buf, 0xf2);
	emit(buf, 0x0f);
	__emit_memdisp_reg(buf, 0x58, insn->src.imm, mach_reg(&insn->dest.reg));
}

static void emit_fsub_reg_reg(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	emit(buf, 0xf3);
	emit(buf, 0x0f);
	emit_reg_reg(buf, 0x5c, &insn->dest, &insn->src);
}

static void emit_fsub_64_reg_reg(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	emit(buf, 0xf2);
	emit(buf, 0x0f);
	emit_reg_reg(buf, 0x5c, &insn->dest, &insn->src);
}

static void emit_fmul_reg_reg(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	emit(buf, 0xf3);
	emit(buf, 0x0f);
	emit_reg_reg(buf, 0x59, &insn->dest, &insn->src);
}

static void emit_fmul_64_reg_reg(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	emit(buf, 0xf2);
	emit(buf, 0x0f);
	emit_reg_reg(buf, 0x59, &insn->dest, &insn->src);
}

static void emit_fmul_64_memdisp_reg(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	emit(buf, 0xf2);
	emit(buf, 0x0f);
	__emit_memdisp_reg(buf, 0x59, insn->src.imm, mach_reg(&insn->dest.reg));
}

static void emit_fdiv_reg_reg(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	emit(buf, 0xf3);
	emit(buf, 0x0f);
	emit_reg_reg(buf, 0x5e, &insn->dest, &insn->src);
}

static void emit_fdiv_64_reg_reg(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	emit(buf, 0xf2);
	emit(buf, 0x0f);
	emit_reg_reg(buf, 0x5e, &insn->dest, &insn->src);
}

static void emit_fld_membase(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	__emit_membase(buf, 0xd9, mach_reg(&insn->operand.base_reg), insn->operand.disp, 0);
}

static void emit_fld_64_membase(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	__emit_membase(buf, 0xdd, mach_reg(&insn->operand.base_reg), insn->operand.disp, 0);
}

static void emit_fld_memlocal(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	__emit_membase(buf, 0xd9, MACH_REG_EBP, slot_offset(insn->operand.slot), 0);
}

static void emit_fld_64_memlocal(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	__emit_membase(buf, 0xdd, MACH_REG_EBP, slot_offset_64(insn->operand.slot), 0);
}

static void emit_fild_64_membase(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	__emit_membase(buf, 0xdf, mach_reg(&insn->operand.base_reg), insn->operand.disp, 5);
}

static void emit_fldcw_membase(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	__emit_membase(buf, 0xd9, mach_reg(&insn->operand.base_reg), insn->operand.disp, 5);
}

static void emit_fnstcw_membase(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	__emit_membase(buf, 0xd9, mach_reg(&insn->operand.base_reg), insn->operand.disp, 7);
}

static void emit_fistp_64_membase(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	__emit_membase(buf, 0xdf, mach_reg(&insn->operand.base_reg), insn->operand.disp, 7);
}

static void emit_fstp_membase(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	__emit_membase(buf, 0xd9, mach_reg(&insn->operand.base_reg), insn->operand.disp, 3);
}

static void emit_fstp_memlocal(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	__emit_membase(buf, 0xd9, MACH_REG_EBP, slot_offset(insn->operand.slot), 3);
}

static void emit_fstp_64_membase(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	__emit_membase(buf, 0xdd, mach_reg(&insn->operand.base_reg), insn->operand.disp, 3);
}

static void emit_fstp_64_memlocal(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	__emit_membase(buf, 0xdd, MACH_REG_EBP, slot_offset_64(insn->operand.slot), 3);
}

static void emit_add_membase_reg(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	emit_membase_reg(buf, 0x03, &insn->src, &insn->dest);
}

static void emit_and_reg_reg(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	emit_reg_reg(buf, 0x23, &insn->dest, &insn->src);
}

static void emit_and_membase_reg(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	emit_membase_reg(buf, 0x23, &insn->src, &insn->dest);
}

static void emit_sbb_membase_reg(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	emit_membase_reg(buf, 0x1b, &insn->src, &insn->dest);
}

static void emit_sub_membase_reg(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	emit_membase_reg(buf, 0x2b, &insn->src, &insn->dest);
}

static void __emit_div_mul_membase_eax(struct buffer *buf,
				       struct operand *src,
				       struct operand *dest,
				       unsigned char opc_ext)
{
	long disp;
	int mod;

	assert(mach_reg(&dest->reg) == MACH_REG_EAX);

	disp = src->disp;

	if (is_imm_8(disp))
		mod = 0x01;
	else
		mod = 0x02;

	emit(buf, 0xf7);
	emit(buf, encode_modrm(mod, opc_ext, encode_reg(&src->base_reg)));
	emit_imm(buf, disp);
}

static void __emit_div_mul_reg_eax(struct buffer *buf,
				       struct operand *src,
				       struct operand *dest,
				       unsigned char opc_ext)
{
	assert(mach_reg(&dest->reg) == MACH_REG_EAX);

	emit(buf, 0xf7);
	emit(buf, encode_modrm(0x03, opc_ext, encode_reg(&src->base_reg)));
}

static void emit_mul_membase_eax(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	__emit_div_mul_membase_eax(buf, &insn->src, &insn->dest, 0x04);
}

static void emit_mul_reg_eax(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	__emit_div_mul_reg_eax(buf, &insn->src, &insn->dest, 0x04);
}

static void emit_mul_reg_reg(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	emit(buf, 0x0f);
	__emit_reg_reg(buf, 0xaf, mach_reg(&insn->dest.reg), mach_reg(&insn->src.reg));
}

static void emit_neg_reg(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	emit(buf, 0xf7);
	emit(buf, encode_modrm(0x3, 0x3, encode_reg(&insn->operand.reg)));
}

static void emit_cltd_reg_reg(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	assert(mach_reg(&insn->src.reg) == MACH_REG_EAX);
	assert(mach_reg(&insn->dest.reg) == MACH_REG_EDX);

	emit(buf, 0x99);
}

static void emit_div_membase_reg(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	__emit_div_mul_membase_eax(buf, &insn->src, &insn->dest, 0x07);
}

static void emit_div_reg_reg(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	__emit_div_mul_reg_eax(buf, &insn->src, &insn->dest, 0x07);
}

static void __emit_shift_reg_reg(struct buffer *buf,
				 struct operand *src,
				 struct operand *dest, unsigned char opc_ext)
{
	assert(mach_reg(&src->reg) == MACH_REG_ECX);

	emit(buf, 0xd3);
	emit(buf, encode_modrm(0x03, opc_ext, encode_reg(&dest->reg)));
}

static void emit_shl_reg_reg(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	__emit_shift_reg_reg(buf, &insn->src, &insn->dest, 0x04);
}

static void emit_sar_imm_reg(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	emit(buf, 0xc1);
	emit(buf, encode_modrm(0x03, 0x07, encode_reg(&insn->dest.reg)));
	emit(buf, insn->src.imm);
}

static void emit_sar_reg_reg(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	__emit_shift_reg_reg(buf, &insn->src, &insn->dest, 0x07);
}

static void emit_shr_reg_reg(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	__emit_shift_reg_reg(buf, &insn->src, &insn->dest, 0x05);
}

static void emit_or_membase_reg(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	emit_membase_reg(buf, 0x0b, &insn->src, &insn->dest);
}

static void emit_or_imm_membase(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	__emit_membase(buf, 0x81, mach_reg(&insn->dest.base_reg), insn->dest.disp, 1);
	emit_imm32(buf, insn->src.disp);
}

static void emit_or_reg_reg(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	emit_reg_reg(buf, 0x0b, &insn->dest, &insn->src);
}

static void emit_xor_reg_reg(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	emit_reg_reg(buf, 0x33, &insn->dest, &insn->src);
}

static void emit_xor_xmm_reg_reg(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	emit(buf, 0x0f);
	emit_reg_reg(buf, 0x57, &insn->dest, &insn->src);
}

static void emit_xor_64_xmm_reg_reg(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	emit(buf, 0x66);
	emit(buf, 0x0f);
	emit_reg_reg(buf, 0x57, &insn->dest, &insn->src);
}

static void __emit_add_imm_reg(struct buffer *buf, long imm, enum machine_reg reg)
{
	emit_alu_imm_reg(buf, 0x00, imm, reg);
}

static void emit_add_imm_reg(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	__emit_add_imm_reg(buf, insn->src.imm, mach_reg(&insn->dest.reg));
}

static void __emit_adc_imm_reg(struct buffer *buf, long imm, enum machine_reg reg)
{
	emit_alu_imm_reg(buf, 0x02, imm, reg);
}

static void emit_adc_imm_reg(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	__emit_adc_imm_reg(buf, insn->src.imm, mach_reg(&insn->dest.reg));
}

static void __emit_cmp_imm_reg(struct buffer *buf, long imm, enum machine_reg reg)
{
	emit_alu_imm_reg(buf, 0x07, imm, reg);
}

static void emit_cmp_imm_reg(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	__emit_cmp_imm_reg(buf, insn->src.imm, mach_reg(&insn->dest.reg));
}

static void emit_cmp_membase_reg(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	emit_membase_reg(buf, 0x3b, &insn->src, &insn->dest);
}

static void emit_cmp_reg_reg(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	emit_reg_reg(buf, 0x39, &insn->src, &insn->dest);
}

static void emit_indirect_jump_reg(struct buffer *buf, enum machine_reg reg)
{
	emit(buf, 0xff);
	emit(buf, encode_modrm(0x3, 0x04, encode_mach_reg(reg)));
}

static void emit_really_indirect_jump_reg(struct buffer *buf, enum machine_reg reg)
{
	emit(buf, 0xff);
	emit(buf, encode_modrm(0x0, 0x04, encode_mach_reg(reg)));
}

static void emit_indirect_call(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	emit(buf, 0xff);
	emit(buf, encode_modrm(0x0, 0x2, encode_reg(&insn->operand.reg)));
}

static void emit_xor_membase_reg(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	emit_membase_reg(buf, 0x33, &insn->src, &insn->dest);
}

static void __emit_test_membase_reg(struct buffer *buf, enum machine_reg src,
				    unsigned long disp, enum machine_reg dest)
{
	__emit_membase_reg(buf, 0x85, src, disp, dest);
}

static void emit_test_membase_reg(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	emit_membase_reg(buf, 0x85, &insn->src, &insn->dest);
}

/* Emits exception test using given register. */
static void emit_exception_test(struct buffer *buf, enum machine_reg reg)
{
	/* mov gs:(0xXXX), %reg */
	emit(buf, 0x65);
	__emit_memdisp_reg(buf, 0x8b,
		get_thread_local_offset(&exception_guard), reg);

	/* test (%reg), %reg */
	__emit_test_membase_reg(buf, reg, 0, reg);
}

static void emit_conv_xmm_to_xmm64(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	emit(buf, 0xf3);
	emit(buf, 0x0f);
	emit_reg_reg(buf, 0x5a, &insn->dest, &insn->src);
}

static void emit_conv_xmm64_to_xmm(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	emit(buf, 0xf2);
	emit(buf, 0x0f);
	emit_reg_reg(buf, 0x5a, &insn->dest, &insn->src);
}

static void emit_conv_gpr_to_fpu(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	emit(buf, 0xf3);
	emit(buf, 0x0f);
	emit_reg_reg(buf, 0x2a, &insn->dest, &insn->src);
}

static void emit_conv_gpr_to_fpu64(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	emit(buf, 0xf2);
	emit(buf, 0x0f);
	emit_reg_reg(buf, 0x2a, &insn->dest, &insn->src);
}

static void emit_conv_fpu_to_gpr(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	emit(buf, 0xf3);
	emit(buf, 0x0f);
	emit_reg_reg(buf, 0x2d, &insn->dest, &insn->src);
}

static void emit_conv_fpu64_to_gpr(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	emit(buf, 0xf2);
	emit(buf, 0x0f);
	emit_reg_reg(buf, 0x2d, &insn->dest, &insn->src);
}

static void emit_mov_xmm_xmm(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	emit(buf, 0xf3);
	emit(buf, 0x0f);
	emit_reg_reg(buf, 0x10, &insn->dest, &insn->src);
}

static void emit_mov_64_xmm_xmm(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	emit(buf, 0xf2);
	emit(buf, 0x0f);
	emit_reg_reg(buf, 0x10, &insn->dest, &insn->src);
}

static void emit_mov_membase_xmm(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	emit(buf, 0xf3);
	emit(buf, 0x0f);
	emit_membase_reg(buf, 0x10, &insn->src, &insn->dest);
}

static void emit_mov_64_membase_xmm(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	emit(buf, 0xf2);
	emit(buf, 0x0f);
	emit_membase_reg(buf, 0x10, &insn->src, &insn->dest);
}

static void emit_mov_memdisp_xmm(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	emit(buf, 0xf3);
	emit(buf, 0x0f);
	__emit_memdisp_reg(buf, 0x10, insn->src.imm, mach_reg(&insn->dest.reg));
}

static void emit_mov_64_memdisp_xmm(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	emit(buf, 0xf2);
	emit(buf, 0x0f);
	__emit_memdisp_reg(buf, 0x10, insn->src.imm, mach_reg(&insn->dest.reg));
}

static void emit_mov_memindex_xmm(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	emit(buf, 0xf3);
	emit(buf, 0x0f);
	emit(buf, 0x10);
	emit(buf, encode_modrm(0x00, encode_reg(&insn->dest.reg), 0x04));
	emit(buf, encode_sib(insn->src.shift, encode_reg(&insn->src.index_reg), encode_reg(&insn->src.base_reg)));
}

static void emit_mov_64_memindex_xmm(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	emit(buf, 0xf2);
	emit(buf, 0x0f);
	emit(buf, 0x10);
	emit(buf, encode_modrm(0x00, encode_reg(&insn->dest.reg), 0x04));
	emit(buf, encode_sib(insn->src.shift, encode_reg(&insn->src.index_reg), encode_reg(&insn->src.base_reg)));
}

static void emit_mov_xmm_membase(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	emit(buf, 0xf3);
	emit(buf, 0x0f);
	emit_membase_reg(buf, 0x11, &insn->dest, &insn->src);
}

static void emit_mov_64_xmm_membase(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	emit(buf, 0xf2);
	emit(buf, 0x0f);
	emit_membase_reg(buf, 0x11, &insn->dest, &insn->src);
}

static void emit_mov_xmm_memindex(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	emit(buf, 0xf3);
	emit(buf, 0x0f);
	emit(buf, 0x11);
	emit(buf, encode_modrm(0x00, encode_reg(&insn->src.reg), 0x04));
	emit(buf, encode_sib(insn->dest.shift, encode_reg(&insn->dest.index_reg), encode_reg(&insn->dest.base_reg)));
}

static void emit_mov_64_xmm_memindex(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	emit(buf, 0xf2);
	emit(buf, 0x0f);
	emit(buf, 0x11);
	emit(buf, encode_modrm(0x00, encode_reg(&insn->src.reg), 0x04));
	emit(buf, encode_sib(insn->dest.shift, encode_reg(&insn->dest.index_reg), encode_reg(&insn->dest.base_reg)));
}

static void emit_mov_xmm_memdisp(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	emit(buf, 0xf3);
	emit(buf, 0x0f);
	__emit_reg_memdisp(buf, 0x11, mach_reg(&insn->src.reg), insn->dest.imm);
}

static void emit_mov_64_xmm_memdisp(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	emit(buf, 0xf2);
	emit(buf, 0x0f);
	__emit_reg_memdisp(buf, 0x11, mach_reg(&insn->src.reg), insn->dest.imm);
}

static void emit_jmp_memindex(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	emit(buf, 0xff);
	emit(buf, encode_modrm(0x00, 0x04, 0x04));
	emit(buf, encode_sib(insn->operand.shift, encode_reg(&insn->operand.index_reg),
			     encode_reg(&insn->operand.base_reg)));
}

static void emit_jmp_membase(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	__emit_membase(buf, 0xff, mach_reg(&insn->operand.base_reg), insn->operand.disp, 0x04);
}

struct emitter emitters[] = {
	GENERIC_X86_EMITTERS,
	DECL_EMITTER(INSN_ADC_IMM_REG, emit_adc_imm_reg),
	DECL_EMITTER(INSN_ADC_REG_REG, emit_adc_reg_reg),
	DECL_EMITTER(INSN_ADC_MEMBASE_REG, emit_adc_membase_reg),
	DECL_EMITTER(INSN_ADD_IMM_REG, emit_add_imm_reg),
	DECL_EMITTER(INSN_ADD_MEMBASE_REG, emit_add_membase_reg),
	DECL_EMITTER(INSN_ADD_REG_REG, emit_add_reg_reg),
	DECL_EMITTER(INSN_AND_MEMBASE_REG, emit_and_membase_reg),
	DECL_EMITTER(INSN_AND_REG_REG, emit_and_reg_reg),
	DECL_EMITTER(INSN_CALL_REG, emit_indirect_call),
	DECL_EMITTER(INSN_CLTD_REG_REG, emit_cltd_reg_reg),
	DECL_EMITTER(INSN_CMP_IMM_REG, emit_cmp_imm_reg),
	DECL_EMITTER(INSN_CMP_MEMBASE_REG, emit_cmp_membase_reg),
	DECL_EMITTER(INSN_CMP_REG_REG, emit_cmp_reg_reg),
	DECL_EMITTER(INSN_DIV_MEMBASE_REG, emit_div_membase_reg),
	DECL_EMITTER(INSN_DIV_REG_REG, emit_div_reg_reg),
	DECL_EMITTER(INSN_FADD_REG_REG, emit_fadd_reg_reg),
	DECL_EMITTER(INSN_FADD_64_REG_REG, emit_fadd_64_reg_reg),
	DECL_EMITTER(INSN_FADD_64_MEMDISP_REG, emit_fadd_64_memdisp_reg),
	DECL_EMITTER(INSN_FSUB_REG_REG, emit_fsub_reg_reg),
	DECL_EMITTER(INSN_FSUB_64_REG_REG, emit_fsub_64_reg_reg),
	DECL_EMITTER(INSN_FMUL_REG_REG, emit_fmul_reg_reg),
	DECL_EMITTER(INSN_FMUL_64_REG_REG, emit_fmul_64_reg_reg),
	DECL_EMITTER(INSN_FMUL_64_MEMDISP_REG, emit_fmul_64_memdisp_reg),
	DECL_EMITTER(INSN_FDIV_REG_REG, emit_fdiv_reg_reg),
	DECL_EMITTER(INSN_FDIV_64_REG_REG, emit_fdiv_64_reg_reg),
	DECL_EMITTER(INSN_FLD_MEMBASE, emit_fld_membase),
	DECL_EMITTER(INSN_FLD_MEMLOCAL, emit_fld_memlocal),
	DECL_EMITTER(INSN_FLD_64_MEMBASE, emit_fld_64_membase),
	DECL_EMITTER(INSN_FLD_64_MEMLOCAL, emit_fld_64_memlocal),
	DECL_EMITTER(INSN_FLDCW_MEMBASE, emit_fldcw_membase),
	DECL_EMITTER(INSN_FILD_64_MEMBASE, emit_fild_64_membase),
	DECL_EMITTER(INSN_FISTP_64_MEMBASE, emit_fistp_64_membase),
	DECL_EMITTER(INSN_FNSTCW_MEMBASE, emit_fnstcw_membase),
	DECL_EMITTER(INSN_FSTP_MEMBASE, emit_fstp_membase),
	DECL_EMITTER(INSN_FSTP_MEMLOCAL, emit_fstp_memlocal),
	DECL_EMITTER(INSN_FSTP_64_MEMBASE, emit_fstp_64_membase),
	DECL_EMITTER(INSN_FSTP_64_MEMLOCAL, emit_fstp_64_memlocal),
	DECL_EMITTER(INSN_CONV_GPR_TO_FPU, emit_conv_gpr_to_fpu),
	DECL_EMITTER(INSN_CONV_GPR_TO_FPU64, emit_conv_gpr_to_fpu64),
	DECL_EMITTER(INSN_CONV_FPU_TO_GPR, emit_conv_fpu_to_gpr),
	DECL_EMITTER(INSN_CONV_FPU64_TO_GPR, emit_conv_fpu64_to_gpr),
	DECL_EMITTER(INSN_CONV_XMM_TO_XMM64, emit_conv_xmm_to_xmm64),
	DECL_EMITTER(INSN_CONV_XMM64_TO_XMM, emit_conv_xmm64_to_xmm),
	DECL_EMITTER(INSN_JMP_MEMBASE, emit_jmp_membase),
	DECL_EMITTER(INSN_JMP_MEMINDEX, emit_jmp_memindex),
	DECL_EMITTER(INSN_MOV_MEMBASE_XMM, emit_mov_membase_xmm),
	DECL_EMITTER(INSN_MOV_64_MEMBASE_XMM, emit_mov_64_membase_xmm),
	DECL_EMITTER(INSN_MOV_XMM_MEMBASE, emit_mov_xmm_membase),
	DECL_EMITTER(INSN_MOV_64_XMM_MEMBASE, emit_mov_64_xmm_membase),
	DECL_EMITTER(INSN_MOV_IMM_MEMBASE, emit_mov_imm_membase),
	DECL_EMITTER(INSN_MOV_IMM_MEMLOCAL, emit_mov_imm_memlocal),
	DECL_EMITTER(INSN_MOV_IMM_REG, emit_mov_imm_reg),
	DECL_EMITTER(INSN_MOV_IMM_THREAD_LOCAL_MEMBASE, emit_mov_imm_thread_local_membase),
	DECL_EMITTER(INSN_MOV_MEMLOCAL_REG, emit_mov_memlocal_reg),
	DECL_EMITTER(INSN_MOV_MEMLOCAL_XMM, emit_mov_memlocal_xmm),
	DECL_EMITTER(INSN_MOV_64_MEMLOCAL_XMM, emit_mov_64_memlocal_xmm),
	DECL_EMITTER(INSN_MOV_MEMBASE_REG, emit_mov_membase_reg),
	DECL_EMITTER(INSN_MOV_MEMDISP_REG, emit_mov_memdisp_reg),
	DECL_EMITTER(INSN_MOV_MEMDISP_XMM, emit_mov_memdisp_xmm),
	DECL_EMITTER(INSN_MOV_64_MEMDISP_XMM, emit_mov_64_memdisp_xmm),
	DECL_EMITTER(INSN_MOV_REG_MEMDISP, emit_mov_reg_memdisp),
	DECL_EMITTER(INSN_MOV_THREAD_LOCAL_MEMDISP_REG, emit_mov_thread_local_memdisp_reg),
	DECL_EMITTER(INSN_MOV_MEMINDEX_REG, emit_mov_memindex_reg),
	DECL_EMITTER(INSN_MOV_MEMINDEX_XMM, emit_mov_memindex_xmm),
	DECL_EMITTER(INSN_MOV_64_MEMINDEX_XMM, emit_mov_64_memindex_xmm),
	DECL_EMITTER(INSN_MOV_REG_MEMBASE, emit_mov_reg_membase),
	DECL_EMITTER(INSN_MOV_REG_MEMINDEX, emit_mov_reg_memindex),
	DECL_EMITTER(INSN_MOV_REG_MEMLOCAL, emit_mov_reg_memlocal),
	DECL_EMITTER(INSN_MOV_REG_THREAD_LOCAL_MEMBASE, emit_mov_reg_thread_local_membase),
	DECL_EMITTER(INSN_MOV_REG_THREAD_LOCAL_MEMDISP, emit_mov_reg_thread_local_memdisp),
	DECL_EMITTER(INSN_MOV_XMM_MEMLOCAL, emit_mov_xmm_memlocal),
	DECL_EMITTER(INSN_MOV_64_XMM_MEMLOCAL, emit_mov_64_xmm_memlocal),
	DECL_EMITTER(INSN_MOV_REG_REG, emit_mov_reg_reg),
	DECL_EMITTER(INSN_MOV_XMM_MEMDISP, emit_mov_xmm_memdisp),
	DECL_EMITTER(INSN_MOV_64_XMM_MEMDISP, emit_mov_64_xmm_memdisp),
	DECL_EMITTER(INSN_MOV_XMM_MEMINDEX, emit_mov_xmm_memindex),
	DECL_EMITTER(INSN_MOV_64_XMM_MEMINDEX, emit_mov_64_xmm_memindex),
	DECL_EMITTER(INSN_MOV_XMM_XMM, emit_mov_xmm_xmm),
	DECL_EMITTER(INSN_MOV_64_XMM_XMM, emit_mov_64_xmm_xmm),
	DECL_EMITTER(INSN_MOVSX_8_REG_REG, emit_movsx_8_reg_reg),
	DECL_EMITTER(INSN_MOVSX_8_MEMBASE_REG, emit_movsx_8_membase_reg),
	DECL_EMITTER(INSN_MOVSX_16_REG_REG, emit_movsx_16_reg_reg),
	DECL_EMITTER(INSN_MOVSX_16_MEMBASE_REG, emit_movsx_16_membase_reg),
	DECL_EMITTER(INSN_MOVZX_16_REG_REG, emit_movzx_16_reg_reg),
	DECL_EMITTER(INSN_MUL_MEMBASE_EAX, emit_mul_membase_eax),
	DECL_EMITTER(INSN_MUL_REG_EAX, emit_mul_reg_eax),
	DECL_EMITTER(INSN_MUL_REG_REG, emit_mul_reg_reg),
	DECL_EMITTER(INSN_NEG_REG, emit_neg_reg),
	DECL_EMITTER(INSN_OR_IMM_MEMBASE, emit_or_imm_membase),
	DECL_EMITTER(INSN_OR_MEMBASE_REG, emit_or_membase_reg),
	DECL_EMITTER(INSN_OR_REG_REG, emit_or_reg_reg),
	DECL_EMITTER(INSN_PUSH_IMM, emit_push_imm),
	DECL_EMITTER(INSN_PUSH_REG, emit_push_reg),
	DECL_EMITTER(INSN_PUSH_MEMLOCAL, emit_push_memlocal),
	DECL_EMITTER(INSN_POP_MEMLOCAL, emit_pop_memlocal),
	DECL_EMITTER(INSN_POP_REG, emit_pop_reg),
	DECL_EMITTER(INSN_SAR_IMM_REG, emit_sar_imm_reg),
	DECL_EMITTER(INSN_SAR_REG_REG, emit_sar_reg_reg),
	DECL_EMITTER(INSN_SBB_IMM_REG, emit_sbb_imm_reg),
	DECL_EMITTER(INSN_SBB_MEMBASE_REG, emit_sbb_membase_reg),
	DECL_EMITTER(INSN_SBB_REG_REG, emit_sbb_reg_reg),
	DECL_EMITTER(INSN_SHL_REG_REG, emit_shl_reg_reg),
	DECL_EMITTER(INSN_SHR_REG_REG, emit_shr_reg_reg),
	DECL_EMITTER(INSN_SUB_IMM_REG, emit_sub_imm_reg),
	DECL_EMITTER(INSN_SUB_MEMBASE_REG, emit_sub_membase_reg),
	DECL_EMITTER(INSN_SUB_REG_REG, emit_sub_reg_reg),
	DECL_EMITTER(INSN_TEST_IMM_MEMDISP, emit_test_imm_memdisp),
	DECL_EMITTER(INSN_TEST_MEMBASE_REG, emit_test_membase_reg),
	DECL_EMITTER(INSN_XOR_MEMBASE_REG, emit_xor_membase_reg),
	DECL_EMITTER(INSN_XOR_REG_REG, emit_xor_reg_reg),
	DECL_EMITTER(INSN_XOR_XMM_REG_REG, emit_xor_xmm_reg_reg),
	DECL_EMITTER(INSN_XOR_64_XMM_REG_REG, emit_xor_64_xmm_reg_reg),
};

void emit_trampoline(struct compilation_unit *cu,
		     void *call_target,
		     struct jit_trampoline *trampoline)
{
	struct buffer *buf = trampoline->objcode;

	jit_text_lock();

	buf->buf = jit_text_ptr();

	/* This is for __builtin_return_address() to work and to access
	   call arguments in correct manner. */
	__emit_push_reg(buf, MACH_REG_EBP);
	__emit_mov_reg_reg(buf, MACH_REG_ESP, MACH_REG_EBP);

	__emit_push_imm(buf, (unsigned long)cu);
	__emit_call(buf, call_target);
	__emit_add_imm_reg(buf, 0x04, MACH_REG_ESP);

	/*
	 * Test for exeption occurance.
	 * We do this by polling a dedicated thread-specific pointer,
	 * which triggers SIGSEGV when exception is set.
	 *
	 * mov gs:(0xXXX), %ecx
	 * test (%ecx), %ecx
	 */
	emit(buf, 0x65);
	__emit_memdisp_reg(buf, 0x8b,
			   get_thread_local_offset(&trampoline_exception_guard),
			   MACH_REG_ECX);
	__emit_test_membase_reg(buf, MACH_REG_ECX, 0, MACH_REG_ECX);

	__emit_pop_reg(buf, MACH_REG_EBP);
	emit_indirect_jump_reg(buf, MACH_REG_EAX);

	jit_text_reserve(buffer_offset(buf));
	jit_text_unlock();
}

void emit_lock(struct buffer *buf, struct vm_object *obj)
{
	__emit_push_imm(buf, (unsigned long)obj);
	__emit_call(buf, vm_object_lock);
	__emit_add_imm_reg(buf, PTR_SIZE, MACH_REG_xSP);

	__emit_push_reg(buf, MACH_REG_EAX);
	emit_exception_test(buf, MACH_REG_EAX);
	__emit_pop_reg(buf, MACH_REG_EAX);
}

void emit_unlock(struct buffer *buf, struct vm_object *obj)
{
	/* Save caller-saved registers which contain method's return value */
	__emit_push_reg(buf, MACH_REG_EAX);
	__emit_push_reg(buf, MACH_REG_EDX);

	__emit_push_imm(buf, (unsigned long)obj);
	__emit_call(buf, vm_object_unlock);
	__emit_add_imm_reg(buf, PTR_SIZE, MACH_REG_ESP);

	emit_exception_test(buf, MACH_REG_EAX);

	__emit_pop_reg(buf, MACH_REG_EDX);
	__emit_pop_reg(buf, MACH_REG_EAX);
}

void emit_lock_this(struct buffer *buf)
{
	unsigned long this_arg_offset;

	this_arg_offset = offsetof(struct jit_stack_frame, args);

	__emit_push_membase(buf, MACH_REG_EBP, this_arg_offset);
	__emit_call(buf, vm_object_lock);
	__emit_add_imm_reg(buf, PTR_SIZE, MACH_REG_ESP);

	__emit_push_reg(buf, MACH_REG_EAX);
	emit_exception_test(buf, MACH_REG_EAX);
	__emit_pop_reg(buf, MACH_REG_EAX);
}

void emit_unlock_this(struct buffer *buf)
{
	unsigned long this_arg_offset;

	this_arg_offset = offsetof(struct jit_stack_frame, args);

	/* Save caller-saved registers which contain method's return value */
	__emit_push_reg(buf, MACH_REG_EAX);
	__emit_push_reg(buf, MACH_REG_EDX);

	__emit_push_membase(buf, MACH_REG_EBP, this_arg_offset);
	__emit_call(buf, vm_object_unlock);
	__emit_add_imm_reg(buf, PTR_SIZE, MACH_REG_ESP);

	emit_exception_test(buf, MACH_REG_EAX);

	__emit_pop_reg(buf, MACH_REG_EDX);
	__emit_pop_reg(buf, MACH_REG_EAX);
}

static void fixup_branch_target(uint8_t *target_p, void *target)
{
	long cur = (long) (target - (void *) target_p) - 4;
	target_p[3] = cur >> 24;
	target_p[2] = cur >> 16;
	target_p[1] = cur >> 8;
	target_p[0] = cur;
}

void emit_jni_trampoline(struct buffer *buf, struct vm_method *vmm,
			 void *target)
{
	uint8_t *jne_target;

	jit_text_lock();

	buf->buf = jit_text_ptr();

	/* return address is passed implicitly as last argument */
	__emit_push_imm(buf, (unsigned long) vmm);
	__emit_push_reg(buf, MACH_REG_EBP);

	/* If this returns non-zero then StackOverflowError occurred. */
	__emit_call(buf, vm_enter_jni);

	/* test %eax, %eax */
	__emit_reg_reg(buf, 0x33, MACH_REG_EAX, MACH_REG_EAX);

	/* jne */
	emit(buf, 0x0f);
	emit(buf, 0x85);
	jne_target = buffer_current(buf);
	emit_imm32(buf, 0);

	/* Cleanup call arguments and return address. */
	__emit_add_imm_reg(buf, 3 * sizeof(long), MACH_REG_ESP);

	if (vm_method_is_static(vmm))
		__emit_push_imm(buf, (unsigned long) vmm->class->object);

	__emit_push_imm(buf, (unsigned long) vm_jni_get_jni_env());

	__emit_call(buf, target);

	/* Cleanup args. Leave one slot for return address. */
	if (vm_method_is_static(vmm))
		__emit_add_imm_reg(buf, sizeof(long), MACH_REG_ESP);

	__emit_push_reg(buf, MACH_REG_EAX);
	__emit_call(buf, vm_leave_jni);
	__emit_mov_reg_membase(buf, MACH_REG_EAX, MACH_REG_ESP, sizeof(long));
	__emit_pop_reg(buf, MACH_REG_EAX);

	encode_ret(buf);

	/* We will jump here if StackOverflowError occurred. */
	fixup_branch_target(jne_target, buffer_current(buf));

	/* cleanup vm_enter_jni() call arguments. */
	__emit_add_imm_reg(buf, 2 * sizeof(long), MACH_REG_ESP);
	encode_ret(buf);

	jit_text_reserve(buffer_offset(buf));
	jit_text_unlock();
}

/* The regparm(1) makes GCC get the first argument from %ecx and the rest
 * from the stack. This is convenient, because we use %ecx for passing the
 * hidden "method" parameter. Interfaces are invoked on objects, so we also
 * always get the object in the first stack parameter. */
void __attribute__((regparm(1)))
itable_resolver_stub_error(struct vm_method *method, struct vm_object *obj)
{
	fprintf(stderr, "itable resolver stub error!\n");
	fprintf(stderr, "invokeinterface called on method %s.%s%s "
		"(itable index %d)\n",
		method->class->name, method->name, method->type,
		method->itable_index);
	fprintf(stderr, "object class %s\n", obj->class->name);

	print_trace();
	abort();
}

/* Note: a < b, always */
static void emit_itable_bsearch(struct buffer *buf,
	struct itable_entry **table, unsigned int a, unsigned int b)
{
	uint8_t *jb_addr = NULL;
	uint8_t *ja_addr = NULL;
	unsigned int m;

	/* Find middle (safe from overflows) */
	m = a + (b - a) / 2;

	/* No point in emitting the "cmp" if we're not going to test
	 * anything */
	if (b - a >= 1) {
		__emit_cmp_imm_reg(buf, (long) table[m]->i_method, MACH_REG_EAX);

		if (m - a > 0) {
			/* open-coded "jb" */
			emit(buf, 0x0f);
			emit(buf, 0x82);

			/* placeholder address */
			jb_addr = buffer_current(buf);
			emit_imm32(buf, 0);
		}

		if (b - m > 0) {
			/* open-coded "ja" */
			emit(buf, 0x0f);
			emit(buf, 0x87);

			/* placeholder address */
			ja_addr = buffer_current(buf);
			emit_imm32(buf, 0);
		}
	}

#ifndef NDEBUG
	/* Make sure what we wanted is what we got;
	 *
	 *     cmp i_method, %eax
	 *     je .okay
	 *     jmp itable_resolver_stub_error
	 * .okay:
	 *
	 */
	__emit_cmp_imm_reg(buf, (long) table[m]->i_method, MACH_REG_EAX);

	/* open-coded "je" */
	emit(buf, 0x0f);
	emit(buf, 0x84);

	uint8_t *je_addr = buffer_current(buf);
	emit_imm32(buf, 0);

	__emit_jmp(buf, (unsigned long) &itable_resolver_stub_error);

	fixup_branch_target(je_addr, buffer_current(buf));
#endif

	__emit_add_imm_reg(buf, 4 * table[m]->c_method->virtual_index, MACH_REG_ECX);
	emit_really_indirect_jump_reg(buf, MACH_REG_ECX);

	/* This emits the code for checking the interval [a, m> */
	if (jb_addr) {
		fixup_branch_target(jb_addr, buffer_current(buf));
		emit_itable_bsearch(buf, table, a, m - 1);
	}

	/* This emits the code for checking the interval <m, b] */
	if (ja_addr) {
		fixup_branch_target(ja_addr, buffer_current(buf));
		emit_itable_bsearch(buf, table, m + 1, b);
	}
}

/* Note: table is always sorted on entry->method address */
/* Note: nr_entries is always >= 2 */
void *emit_itable_resolver_stub(struct vm_class *vmc,
	struct itable_entry **table, unsigned int nr_entries)
{
	static struct buffer_operations exec_buf_ops = {
		.expand = NULL,
		.free   = NULL,
	};

	struct buffer *buf = __alloc_buffer(&exec_buf_ops);

	jit_text_lock();

	buf->buf = jit_text_ptr();

	/* Note: When the stub is called, %eax contains the signature hash that
	 * we look up in the stub. 0(%esp) contains the object reference. %ecx
	 * and %edx are available here because they are already saved by the
	 * caller (guaranteed by ABI). */

	/* Load the start of the vtable into %ecx. Later we just add the
	 * right offset to %ecx and jump to *(%ecx). */
	__emit_mov_imm_reg(buf, (long) vmc->vtable.native_ptr, MACH_REG_ECX);

	emit_itable_bsearch(buf, table, 0, nr_entries - 1);

	jit_text_reserve(buffer_offset(buf));
	jit_text_unlock();

	return buffer_ptr(buf);
}

#else /* CONFIG_X86_32 */

/*
 * x86-64 code emitters
 */

static inline void emit_str(struct buffer *buf, unsigned char *str, size_t len)
{
	int err;

	err = append_buffer_str(buf, str, len);
	assert(!err);
}

static int has_legacy_prefix(unsigned char *str, size_t len)
{
	if (len <= 1)
		return 0;

	switch (str[0]) {
		case 0xF2:
		case 0xF3:
			return 1;
		default:
			return 0;
	}
}

static void emit_lopc(struct buffer *buf, int rex, unsigned char *str, size_t len)
{
	if (rex && has_legacy_prefix(str, len)) {
		emit(buf, str[0]);
		emit(buf, rex);
		emit_str(buf, str + 1, len - 1);
	} else {
		if (rex)
			emit(buf, rex);
		emit_str(buf, str, len);
	}
}

static inline unsigned char reg_low(unsigned char reg)
{
	return reg & 0x7;
}

static inline unsigned char reg_high(unsigned char reg)
{
	return reg & 0x8;
}

static inline unsigned long rip_relative(struct buffer *buf,
					 unsigned long addr,
					 unsigned long insn_size)
{
	return addr - (unsigned long) buffer_current(buf) - insn_size;
}

static inline int is_64bit_reg(struct operand *reg)
{
	switch (reg->reg.interval->var_info->vm_type) {
		case J_LONG:
		case J_REFERENCE:
		case J_DOUBLE:
			return 1;
		default:
			return 0;
	}
}

static int is_64bit_bin_reg_op(struct operand *a, struct operand *b)
{
	return is_64bit_reg(a) || is_64bit_reg(b);
}

static inline int is_xmm_reg(struct operand *reg)
{
	return (reg->reg.interval->var_info->vm_type == J_FLOAT ||
		reg->reg.interval->var_info->vm_type == J_DOUBLE);
}

static void __emit_reg(struct buffer *buf,
		       int rex_w,
		       unsigned char opc,
		       enum machine_reg reg)
{
	unsigned char reg_num = encode_mach_reg(reg);
	unsigned char rex_pfx = 0;

	if (rex_w)
		rex_pfx |= REX_W;
	if (reg_high(reg_num))
		rex_pfx |= REX_B;

	if (rex_pfx)
		emit(buf, rex_pfx);
	emit(buf, opc + reg_low(reg_num));
}

static void __emit_push_reg(struct buffer *buf, enum machine_reg reg)
{
	__emit_reg(buf, 0, 0x50, reg);
}

static void emit_push_reg(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	__emit_push_reg(buf, mach_reg(&insn->operand.reg));
}

static void __emit_pop_reg(struct buffer *buf, enum machine_reg reg)
{
	__emit_reg(buf, 0, 0x58, reg);
}

static void emit_pop_reg(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	__emit_pop_reg(buf, mach_reg(&insn->operand.reg));
}

static void __emit_lopc_reg_reg(struct buffer *buf,
				int rex_w,
				unsigned char *lopc,
				size_t lopc_size,
				enum machine_reg direct_reg,
				enum machine_reg rm_reg)
{
	unsigned char rex_pfx = 0, mod_rm;
	unsigned char direct, rm;

	direct = encode_mach_reg(direct_reg);
	rm = encode_mach_reg(rm_reg);

	if (rex_w)
		rex_pfx |= REX_W;
	if (reg_high(direct))
		rex_pfx |= REX_R;
	if (reg_high(rm))
		rex_pfx |= REX_B;

	mod_rm = encode_modrm(0x03, direct, rm);

	emit_lopc(buf, rex_pfx, lopc, lopc_size);
	emit(buf, mod_rm);
}

static inline void __emit_reg_reg(struct buffer *buf,
				  int rex_w,
				  unsigned char opc,
				  enum machine_reg direct_reg,
				  enum machine_reg rm_reg)
{
	__emit_lopc_reg_reg(buf, rex_w, &opc, 1, direct_reg, rm_reg);
}

static void emit_reg_reg(struct buffer *buf,
			 int rex_w,
			 unsigned char opc,
			 struct operand *direct,
			 struct operand *rm)
{
	enum machine_reg direct_reg, rm_reg;

	direct_reg = mach_reg(&direct->reg);
	rm_reg = mach_reg(&rm->reg);

	__emit_reg_reg(buf, rex_w, opc, direct_reg, rm_reg);
}

static void __emit64_mov_reg_reg(struct buffer *buf,
				 enum machine_reg src,
				 enum machine_reg dst)
{
	__emit_reg_reg(buf, 1, 0x89, src, dst);
}

static void __emit32_mov_reg_reg(struct buffer *buf,
				 enum machine_reg src,
				 enum machine_reg dst)
{
	__emit_reg_reg(buf, 0, 0x89, src, dst);
}

static void emit_mov_xmm_xmm(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	unsigned char opc[3];

	if (!is_64bit_reg(&insn->src))
		/* MOVSS */
		opc[0] = 0xF3;
	else
		/* MOVSD */
		opc[0] = 0xF2;
	opc[1] = 0x0F;
	opc[2] = 0x10;

	__emit_lopc_reg_reg(buf, 0, opc, 3,
			    mach_reg(&insn->dest.reg), mach_reg(&insn->src.reg));
}

static void emit_mov_reg_reg(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	int fp;

	fp = is_xmm_reg(&insn->src);
	if (fp != is_xmm_reg(&insn->dest))
		assert(!"Can't do 'mov' between XMM and GPR!");

	if (fp) {
		emit_mov_xmm_xmm(insn, buf, bb);
		return;
	}

	if (is_64bit_bin_reg_op(&insn->src, &insn->dest))
		__emit64_mov_reg_reg(buf,
				     mach_reg(&insn->src.reg), mach_reg(&insn->dest.reg));
	else
		__emit32_mov_reg_reg(buf,
				     mach_reg(&insn->src.reg), mach_reg(&insn->dest.reg));
}

static void emit_add_reg_reg(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	if (is_64bit_bin_reg_op(&insn->src, &insn->dest))
		emit_reg_reg(buf, 1, 0x03, &insn->dest, &insn->src);
	else
		emit_reg_reg(buf, 0, 0x03, &insn->dest, &insn->src);
}

static void emit_sub_reg_reg(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	if (is_64bit_bin_reg_op(&insn->src, &insn->dest))
		emit_reg_reg(buf, 1, 0x29, &insn->src, &insn->dest);
	else
		emit_reg_reg(buf, 0, 0x29, &insn->src, &insn->dest);
}

static void emit_mul_gpr_gpr(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	unsigned char opc[2];
	int rex_w = is_64bit_bin_reg_op(&insn->src, &insn->dest);

	opc[0] = 0x0F;
	opc[1] = 0xAF;
	__emit_lopc_reg_reg(buf, rex_w, opc, 2, mach_reg(&insn->dest.reg), mach_reg(&insn->src.reg));
}

static void emit_mul_xmm_xmm(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	unsigned char opc[3];
	enum machine_reg src, dest;

	if (!is_64bit_reg(&insn->src))
		/* MULSS */
		opc[0] = 0xF3;
	else
		/* MULSD */
		opc[0] = 0xF2;
	opc[1] = 0x0F;
	opc[2] = 0x59;

	src = mach_reg(&insn->src.reg);
	dest = mach_reg(&insn->dest.reg);

	__emit_lopc_reg_reg(buf, 0, opc, 3, dest, src);
}

static void emit_mul_reg_reg(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	int fp;

	fp = is_xmm_reg(&insn->src);
	if (fp != is_xmm_reg(&insn->dest))
		assert(!"Can't do 'mul' between XMM and GPR!");

	if (fp)
		emit_mul_xmm_xmm(insn, buf, bb);
	else
		emit_mul_gpr_gpr(insn, buf, bb);
}

static void emit_alu_imm_reg(struct buffer *buf,
			     int rex_w,
			     unsigned char opc_ext,
			     long imm,
			     enum machine_reg reg)
{
	unsigned char rex_pfx = 0, opc, reg_num;

	reg_num = encode_mach_reg(reg);

	if (rex_w)
		rex_pfx |= REX_W;
	if (reg_high(reg_num))
		rex_pfx |= REX_B;

	if (is_imm_8(imm))
		opc = 0x83;
	else
		opc = 0x81;

	if (rex_pfx)
		emit(buf, rex_pfx);
	emit(buf, opc);
	emit(buf, encode_modrm(0x3, opc_ext, reg_num));
	emit_imm(buf, imm);
}

static void __emit64_sub_imm_reg(struct buffer *buf,
				 unsigned long imm,
				 enum machine_reg reg)
{
	emit_alu_imm_reg(buf, 1, 0x05, imm, reg);
}

static void __emit32_sub_imm_reg(struct buffer *buf,
				 unsigned long imm,
				 enum machine_reg reg)
{
	emit_alu_imm_reg(buf, 0, 0x05, imm, reg);
}

static void emit_sub_imm_reg(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	if (is_64bit_reg(&insn->dest))
		__emit64_sub_imm_reg(buf, insn->src.imm, mach_reg(&insn->dest.reg));
	else
		__emit32_sub_imm_reg(buf, insn->src.imm, mach_reg(&insn->dest.reg));
}

static void __emit64_add_imm_reg(struct buffer *buf,
				 long imm,
				 enum machine_reg reg)
{
	emit_alu_imm_reg(buf, 1, 0x00, imm, reg);
}

#ifdef CONFIG_X86_32
static void __emit32_add_imm_reg(struct buffer *buf,
				 long imm,
				 enum machine_reg reg)
{
	emit_alu_imm_reg(buf, 0, 0x00, imm, reg);
}
#endif

static void emit_add_imm_reg(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	if (is_64bit_reg(&insn->dest))
		__emit64_add_imm_reg(buf, insn->src.imm, mach_reg(&insn->dest.reg));
	else
		__emit64_add_imm_reg(buf, insn->src.imm, mach_reg(&insn->dest.reg));
}

static void emit_imm64(struct buffer *buf, unsigned long imm)
{
	union {
		unsigned long val;
		unsigned char b[8];
	} imm_buf;

	imm_buf.val = imm;
	emit(buf, imm_buf.b[0]);
	emit(buf, imm_buf.b[1]);
	emit(buf, imm_buf.b[2]);
	emit(buf, imm_buf.b[3]);
	emit(buf, imm_buf.b[4]);
	emit(buf, imm_buf.b[5]);
	emit(buf, imm_buf.b[6]);
	emit(buf, imm_buf.b[7]);
}

#ifdef CONFIG_X86_32
static void emit64_imm(struct buffer *buf, long imm)
{
	if (is_imm_8(imm))
		emit(buf, imm);
	else
		emit_imm64(buf, imm);
}
#endif

static void __emit_push_imm(struct buffer *buf, long imm)
{
	unsigned char opc;

	if (is_imm_8(imm))
		opc = 0x6a;
	else
		opc = 0x68;

	emit(buf, opc);
	emit_imm(buf, imm);
}

static void emit_push_imm(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	__emit_push_imm(buf, insn->operand.imm);
}

static void __emit_lopc_membase(struct buffer *buf,
				int rex_w,
				unsigned char *lopc,
				size_t lopc_size,
				enum machine_reg base_reg,
				unsigned long disp,
				unsigned char reg_opcode)
{
	unsigned char rex_pfx = 0, mod, rm, mod_rm;
	unsigned char __base_reg = encode_mach_reg(base_reg);
	int needs_sib, needs_disp;

	needs_sib = (base_reg == MACH_REG_RSP || base_reg == MACH_REG_R12);
	needs_disp = (disp || base_reg == MACH_REG_R13);

	if (needs_sib)
		rm = 0x04;
	else
		rm = __base_reg;

	if (!needs_disp)
		mod = 0x00;
	else if (is_imm_8(disp))
		mod = 0x01;
	else
		mod = 0x02;

	if (rex_w)
		rex_pfx |= REX_W;
	if (reg_high(reg_opcode))
		rex_pfx |= REX_R;
	if (reg_high(__base_reg))
		rex_pfx |= REX_B;

	emit_lopc(buf, rex_pfx, lopc, lopc_size);

	mod_rm = encode_modrm(mod, reg_opcode, rm);
	emit(buf, mod_rm);

	if (needs_sib)
		emit(buf, encode_sib(0x00, 0x04, __base_reg));

	if (needs_disp)
		emit_imm(buf, disp);
}

static void __emit_membase(struct buffer *buf,
			   int rex_w,
			   unsigned char opc,
			   enum machine_reg base_reg,
			   unsigned long disp,
			   unsigned char reg_opcode)
{
	__emit_lopc_membase(buf, rex_w, &opc, 1, base_reg, disp, reg_opcode);
}

static void __emit_lopc_membase_reg(struct buffer *buf,
				    int rex_w,
				    unsigned char *lopc,
				    size_t lopc_size,
				    enum machine_reg base_reg,
				    unsigned long disp,
				    enum machine_reg dest_reg)
{
	__emit_lopc_membase(buf, rex_w, lopc, lopc_size,
			    base_reg, disp, encode_mach_reg(dest_reg));
}

static void __emit_membase_reg(struct buffer *buf,
			       int rex_w,
			       unsigned char opc,
			       enum machine_reg base_reg,
			       unsigned long disp,
			       enum machine_reg dest_reg)
{
	__emit_membase(buf, rex_w, opc, base_reg, disp, encode_mach_reg(dest_reg));
}

static void __emit_lopc_reg_membase(struct buffer *buf,
				    int rex_w,
				    unsigned char *lopc,
				    size_t lopc_size,
				    enum machine_reg src_reg,
				    enum machine_reg base_reg,
				    unsigned long disp)
{
	__emit_lopc_membase(buf, rex_w, lopc, lopc_size,
			    base_reg, disp, encode_mach_reg(src_reg));
}

static void __emit_reg_membase(struct buffer *buf,
			       int rex_w,
			       unsigned char opc,
			       enum machine_reg src_reg,
			       enum machine_reg base_reg,
			       unsigned long disp)
{
	__emit_lopc_reg_membase(buf, rex_w, &opc, 1, src_reg, base_reg, disp);
}

static void emit_membase_reg(struct buffer *buf,
			     int rex_w,
			     unsigned char opc,
			     struct operand *src,
			     struct operand *dest)
{
	enum machine_reg base_reg, dest_reg;
	unsigned long disp;

	base_reg = mach_reg(&src->base_reg);
	disp = src->disp;
	dest_reg = mach_reg(&dest->reg);

	__emit_membase_reg(buf, rex_w, opc, base_reg, disp, dest_reg);
}

#ifdef CONFIG_X86_32
static void emit_reg_membase(struct buffer *buf,
			     int rex_w,
			     unsigned char opc,
			     struct operand *src,
			     struct operand *dest)
{
	enum machine_reg src_reg, base_reg;
	unsigned long disp;

	base_reg = mach_reg(&dest->base_reg);
	disp = dest->disp;
	src_reg = mach_reg(&src->reg);

	__emit_reg_membase(buf, rex_w, opc, src_reg, base_reg, disp);
}

static void __emit64_push_membase(struct buffer *buf,
				  enum machine_reg src_reg,
				  unsigned long disp)
{
	__emit_membase(buf, 0, 0xff, src_reg, disp, 6);
}
#endif

static void __emit_mov_reg_membase(struct buffer *buf,
				   int rex_w,
				   enum machine_reg src,
				   enum machine_reg base,
				   unsigned long disp)
{
	__emit_membase(buf, rex_w, 0x89, base, disp, encode_mach_reg(src));
}

static void emit_mov_gpr_membase(struct insn *insn,
				 struct buffer *buf,
				 struct basic_block *bb)
{
	int rex_w = is_64bit_reg(&insn->src);

	__emit_mov_reg_membase(buf, rex_w, mach_reg(&insn->src.reg),
			       mach_reg(&insn->dest.base_reg), insn->dest.disp);
}

static void emit_mov_xmm_membase(struct insn *insn,
				 struct buffer *buf,
				 struct basic_block *bb)
{
	unsigned char opc[3];
	enum machine_reg src, base;
	unsigned long disp;

	if (!is_64bit_reg(&insn->src))
		/* MOVSS */
		opc[0] = 0xF3;
	else
		/* MOVSD */
		opc[0] = 0xF2;
	opc[1] = 0x0F;
	opc[2] = 0x11;

	src = mach_reg(&insn->src.reg);
	base = mach_reg(&insn->dest.base_reg);
	disp = insn->dest.disp;

	__emit_lopc_reg_membase(buf, 0, opc, 3, src, base, disp);
}

static void emit_mov_reg_membase(struct insn *insn,
				 struct buffer *buf,
				 struct basic_block *bb)
{
	if (is_xmm_reg(&insn->src))
		emit_mov_xmm_membase(insn, buf, bb);
	else
		emit_mov_gpr_membase(insn, buf, bb);
}

static void __emit_memdisp(struct buffer *buf,
			   int rex_w,
			   unsigned char opc,
			   unsigned long disp,
			   unsigned char reg_opcode)
{
	unsigned char rex_pfx = 0, mod_rm;

	if (rex_w)
		rex_pfx |= REX_W;
	if (reg_high(reg_opcode))
		rex_pfx |= REX_R;

	mod_rm = encode_modrm(0, reg_opcode, 5);

	if (rex_pfx)
		emit(buf, rex_pfx);
	emit(buf, opc);
	emit(buf, mod_rm);
	emit_imm32(buf, rip_relative(buf, disp, 4));
}

static void __emit_memdisp_reg(struct buffer *buf,
			       int rex_w,
			       unsigned char opc,
			       unsigned long disp,
			       enum machine_reg reg)
{
	__emit_memdisp(buf, rex_w, opc, disp, encode_mach_reg(reg));
}

static void __emit_reg_memdisp(struct buffer *buf,
			       int rex_w,
			       unsigned char opc,
			       enum machine_reg reg,
			       unsigned long disp)
{
	__emit_memdisp(buf, rex_w, opc, disp, encode_mach_reg(reg));
}

static void emit_mov_reg_memdisp(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	int rex_w = is_64bit_reg(&insn->src);

	__emit_reg_memdisp(buf, rex_w, 0x89, mach_reg(&insn->src.reg), insn->dest.imm);
}

static void emit_mov_memdisp_reg(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	int rex_w = is_64bit_reg(&insn->dest);

	__emit_memdisp_reg(buf, rex_w, 0x8b, insn->src.imm, mach_reg(&insn->dest.reg));
}

static void emit_mov_thread_local_memdisp_reg(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	int rex_w = is_64bit_reg(&insn->dest);

	emit(buf, 0x64); /* FS segment override prefix */
	__emit_memdisp_reg(buf, rex_w, 0x8b, insn->src.imm, mach_reg(&insn->dest.reg));
}

static void emit_mov_reg_thread_local_memdisp(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	int rex_w = is_64bit_reg(&insn->src);

	emit(buf, 0x64); /* FS segment override prefix */
	__emit_reg_memdisp(buf, rex_w, 0x89, mach_reg(&insn->src.reg), insn->dest.imm);
}

static void emit_mov_reg_thread_local_membase(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	emit(buf, 0x64); /* FS segment override prefix */
	emit_mov_reg_membase(insn, buf, bb);
}

static void __emit64_test_membase_reg(struct buffer *buf,
				      enum machine_reg src,
				      unsigned long disp,
				      enum machine_reg dest)
{
	__emit_membase_reg(buf, 1, 0x85, src, disp, dest);
}

#ifdef CONFIG_X86_32
static void __emit32_test_membase_reg(struct buffer *buf,
				      enum machine_reg src,
				      unsigned long disp,
				      enum machine_reg dest)
{
	__emit_membase_reg(buf, 0, 0x85, src, disp, dest);
}
#endif

static void emit_test_membase_reg(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	emit_membase_reg(buf, is_64bit_bin_reg_op(&insn->src, &insn->dest), 0x85, &insn->src, &insn->dest);
}

static void emit_indirect_jump_reg(struct buffer *buf, enum machine_reg reg)
{
	unsigned char reg_num = encode_mach_reg(reg);

	emit(buf, 0xff);
	if (reg_high(reg_num))
		emit(buf, REX_B);
	emit(buf, encode_modrm(0x3, 0x04, reg_num));
}

static void __emit64_mov_imm_reg(struct buffer *buf,
				 long imm,
				 enum machine_reg reg)
{
	__emit_reg(buf, 1, 0xb8, reg);
	emit_imm64(buf, imm);
}

static void emit_mov_imm_reg(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	__emit64_mov_imm_reg(buf, insn->src.imm, mach_reg(&insn->dest.reg));
}

static void __emit64_mov_membase_reg(struct buffer *buf,
				     enum machine_reg base_reg,
				     unsigned long disp,
				     enum machine_reg dest_reg)
{
	__emit_membase_reg(buf, 1, 0x8b, base_reg, disp, dest_reg);
}

static void emit_mov_membase_reg(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	emit_membase_reg(buf, 1, 0x8b, &insn->src, &insn->dest);
}

static void emit_mov_memlocal_gpr(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	enum machine_reg dest_reg;
	unsigned long disp;

	dest_reg = mach_reg(&insn->dest.reg);
	disp = slot_offset(insn->src.slot);

	__emit_membase_reg(buf, 1, 0x8b, MACH_REG_RBP, disp, dest_reg);
}

static void emit_mov_memlocal_xmm(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	enum machine_reg dest_reg;
	unsigned long disp;
	unsigned char opc[3];

	dest_reg = mach_reg(&insn->dest.reg);
	disp = slot_offset(insn->src.slot);

	if (!is_64bit_reg(&insn->dest))
		/* MOVSS */
		opc[0] = 0xF3;
	else
		/* MOVSD */
		opc[0] = 0xF2;
	opc[1] = 0x0F;
	opc[2] = 0x10;

	__emit_lopc_membase_reg(buf, 0, opc, 3, MACH_REG_RBP, disp, dest_reg);
}

static void emit_mov_memlocal_reg(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	if (is_xmm_reg(&insn->dest))
		emit_mov_memlocal_xmm(insn, buf, bb);
	else
		emit_mov_memlocal_gpr(insn, buf, bb);
}

static void emit_mov_reg_memlocal(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	enum machine_reg src_reg;
	unsigned long disp;

	src_reg = mach_reg(&insn->src.reg);
	disp = slot_offset(insn->dest.slot);

	__emit_reg_membase(buf, 1, 0x89, src_reg, MACH_REG_RBP, disp);
}

static void __emit_cmp_imm_reg(struct buffer *buf,
			       int rex_w,
			       long imm,
			       enum machine_reg reg)
{
	emit_alu_imm_reg(buf, rex_w, 0x07, imm, reg);
}

static void emit_cmp_imm_reg(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	int rex_w = is_64bit_reg(&insn->dest);

	__emit_cmp_imm_reg(buf, rex_w, insn->src.imm, mach_reg(&insn->dest.reg));
}

static void emit_cmp_membase_reg(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	int rex_w = is_64bit_reg(&insn->dest);

	emit_membase_reg(buf, rex_w, 0x3b, &insn->src, &insn->dest);
}

static void emit_cmp_reg_reg(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	int rex_w = is_64bit_bin_reg_op(&insn->src, &insn->dest);

	emit_reg_reg(buf, rex_w, 0x39, &insn->src, &insn->dest);
}

static void emit_indirect_call(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	/* Go through __emit_membase() to handle %r13. */
	__emit_membase(buf, 0, 0xff, mach_reg(&insn->operand.reg), 0, 0x2);
}

static void __emit_test_imm_memdisp(struct buffer *buf,
				    int rex_w,
				    long imm,
				    long disp)
{
	/* XXX: Supports only byte or long imms */

	if (rex_w)
		emit(buf, REX_W);

	if (is_imm_8(imm))
		emit(buf, 0xf6);
	else
		emit(buf, 0xf7);

	emit(buf, 0x04);
	emit(buf, 0x25);
	emit_imm32(buf, disp);
	emit_imm(buf, imm);
}

static void emit_test_imm_memdisp(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	__emit_test_imm_memdisp(buf, 0, insn->src.imm, insn->dest.disp);
}

static void __emit_lopc_memindex(struct buffer *buf,
				 int rex_w,
				 unsigned char *lopc,
				 size_t lopc_size,
				 unsigned char shift,
				 enum machine_reg index_reg,
				 enum machine_reg base_reg,
				 unsigned char reg_opcode)
{
	unsigned char rex_pfx = 0, mod_rm, sib;
	unsigned char __index_reg = encode_mach_reg(index_reg);
	unsigned char __base_reg = encode_mach_reg(base_reg);

	mod_rm = encode_modrm(0x00, reg_opcode, 0x04);
	sib = encode_sib(shift, __index_reg, __base_reg);

	if (rex_w)
		rex_pfx |= REX_W;
	if (reg_high(reg_opcode))
		rex_pfx |= REX_R;
	if (reg_high(__index_reg))
		rex_pfx |= REX_X;
	if (reg_high(__base_reg))
		rex_pfx |= REX_B;

	emit_lopc(buf, rex_pfx, lopc, lopc_size);
	emit(buf, mod_rm);
	emit(buf, sib);
}

static void __emit_memindex_reg(struct buffer *buf,
				int rex_w,
				unsigned char opc,
				unsigned char shift,
				enum machine_reg index_reg,
				enum machine_reg base_reg,
				enum machine_reg dest_reg)
{
	__emit_lopc_memindex(buf, rex_w, &opc, 1, shift,
			     index_reg, base_reg, encode_mach_reg(dest_reg));
}

static void __emit_reg_memindex(struct buffer *buf,
				int rex_w,
				unsigned char opc,
				enum machine_reg src_reg,
				unsigned char shift,
				enum machine_reg index_reg,
				enum machine_reg base_reg)
{
	__emit_lopc_memindex(buf, rex_w, &opc, 1, shift,
			     index_reg, base_reg, encode_mach_reg(src_reg));
}

static void emit_mov_memindex_reg(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	__emit_memindex_reg(buf, is_64bit_reg(&insn->dest), 0x8b,
			    insn->src.shift, mach_reg(&insn->src.index_reg),
			    mach_reg(&insn->src.base_reg), mach_reg(&insn->dest.reg));
}

static void emit_mov_reg_memindex(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	__emit_reg_memindex(buf, is_64bit_reg(&insn->src), 0x89,
			    mach_reg(&insn->src.reg), insn->dest.shift,
			    mach_reg(&insn->dest.index_reg),
			    mach_reg(&insn->dest.base_reg));
}

static void emit_conv_fpu_to_gpr(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	enum machine_reg src, dest;
	unsigned char opc[3];
	int rex_w;

	src = mach_reg(&insn->src.reg);
	dest = mach_reg(&insn->dest.reg);

	if (!is_64bit_reg(&insn->src))
		opc[0] = 0xF3;
	else
		opc[0] = 0xF2;
	opc[1] = 0x0F;
	opc[2] = 0x2D;

	rex_w = is_64bit_reg(&insn->dest);

	__emit_lopc_reg_reg(buf, rex_w, opc, 3, src, dest);
}

static void emit_conv_gpr_to_fpu(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	enum machine_reg src, dest;
	unsigned char opc[3];
	int rex_w;

	src = mach_reg(&insn->src.reg);
	dest = mach_reg(&insn->dest.reg);

	if (!is_64bit_reg(&insn->dest))
		opc[0] = 0xF3;
	else
		opc[0] = 0xF2;
	opc[1] = 0x0F;
	opc[2] = 0x2A;

	rex_w = is_64bit_reg(&insn->src);

	__emit_lopc_reg_reg(buf, rex_w, opc, 3, src, dest);
}

static void emit_conv_fpu_to_fpu(struct insn *insn, struct buffer *buf, struct basic_block *bb)
{
	enum machine_reg src, dest;
	unsigned char opc[3];

	src = mach_reg(&insn->src.reg);
	dest = mach_reg(&insn->dest.reg);

	if (!is_64bit_reg(&insn->src))
		opc[0] = 0xF3;
	else
		opc[0] = 0xF2;
	opc[1] = 0x0F;
	opc[2] = 0x5A;

	__emit_lopc_reg_reg(buf, 0, opc, 3, src, dest);
}

struct emitter emitters[] = {
	GENERIC_X86_EMITTERS,
	DECL_EMITTER(INSN_ADD_IMM_REG, emit_add_imm_reg),
	DECL_EMITTER(INSN_ADD_REG_REG, emit_add_reg_reg),
	DECL_EMITTER(INSN_CALL_REG, emit_indirect_call),
	DECL_EMITTER(INSN_CMP_IMM_REG, emit_cmp_imm_reg),
	DECL_EMITTER(INSN_CMP_MEMBASE_REG, emit_cmp_membase_reg),
	DECL_EMITTER(INSN_CMP_REG_REG, emit_cmp_reg_reg),
	DECL_EMITTER(INSN_CONV_FPU_TO_GPR, emit_conv_fpu_to_gpr),
	DECL_EMITTER(INSN_CONV_GPR_TO_FPU, emit_conv_gpr_to_fpu),
	DECL_EMITTER(INSN_CONV_XMM_TO_XMM64, emit_conv_fpu_to_fpu),
	DECL_EMITTER(INSN_CONV_XMM64_TO_XMM, emit_conv_fpu_to_fpu),
	DECL_EMITTER(INSN_MOV_IMM_REG, emit_mov_imm_reg),
	DECL_EMITTER(INSN_MOV_MEMBASE_REG, emit_mov_membase_reg),
	DECL_EMITTER(INSN_MOV_MEMDISP_REG, emit_mov_memdisp_reg),
	DECL_EMITTER(INSN_MOV_MEMINDEX_REG, emit_mov_memindex_reg),
	DECL_EMITTER(INSN_MOV_MEMLOCAL_REG, emit_mov_memlocal_reg),
	DECL_EMITTER(INSN_MOV_REG_MEMBASE, emit_mov_reg_membase),
	DECL_EMITTER(INSN_MOV_REG_MEMDISP, emit_mov_reg_memdisp),
	DECL_EMITTER(INSN_MOV_REG_MEMINDEX, emit_mov_reg_memindex),
	DECL_EMITTER(INSN_MOV_REG_MEMLOCAL, emit_mov_reg_memlocal),
	DECL_EMITTER(INSN_MOV_REG_REG, emit_mov_reg_reg),
	DECL_EMITTER(INSN_MOV_REG_THREAD_LOCAL_MEMBASE, emit_mov_reg_thread_local_membase),
	DECL_EMITTER(INSN_MOV_REG_THREAD_LOCAL_MEMDISP, emit_mov_reg_thread_local_memdisp),
	DECL_EMITTER(INSN_MOV_THREAD_LOCAL_MEMDISP_REG, emit_mov_thread_local_memdisp_reg),
	DECL_EMITTER(INSN_MUL_REG_REG, emit_mul_reg_reg),
	DECL_EMITTER(INSN_PUSH_IMM, emit_push_imm),
	DECL_EMITTER(INSN_PUSH_REG, emit_push_reg),
	DECL_EMITTER(INSN_POP_REG, emit_pop_reg),
	DECL_EMITTER(INSN_SUB_IMM_REG, emit_sub_imm_reg),
	DECL_EMITTER(INSN_SUB_REG_REG, emit_sub_reg_reg),
	DECL_EMITTER(INSN_TEST_MEMBASE_REG, emit_test_membase_reg),
	DECL_EMITTER(INSN_TEST_IMM_MEMDISP, emit_test_imm_memdisp),
};

static void __emit64_push_xmm(struct buffer *buf, enum machine_reg reg)
{
	unsigned char opc[3] = { 0xF2, 0x0F, 0x11 };	/* MOVSD */

	__emit64_sub_imm_reg(buf, 0x08, MACH_REG_RSP);
	__emit_lopc_reg_membase(buf, 0, opc, 3, reg, MACH_REG_RSP, 0);
}

static void __emit64_pop_xmm(struct buffer *buf, enum machine_reg reg)
{
	unsigned char opc[3] = { 0xF2, 0x0F, 0x10 };	/* MOVSD */

	__emit_lopc_membase_reg(buf, 0, opc, 3, MACH_REG_RSP, 0, reg);
	__emit64_add_imm_reg(buf, 0x08, MACH_REG_RSP);
}

void emit_prolog(struct buffer *buf, unsigned long nr_locals)
{
	__emit_push_reg(buf, MACH_REG_RBP);
	__emit64_mov_reg_reg(buf, MACH_REG_RSP, MACH_REG_RBP);

	/*
	 * The ABI requires us to clear DF, but we
	 * don't need to. Though keep this in mind:
	 * emit(buf, 0xFC);
	 */

	if (nr_locals)
		__emit64_sub_imm_reg(buf,
				     nr_locals * sizeof(unsigned long),
				     MACH_REG_RSP);

	__emit_push_reg(buf, MACH_REG_RBX);
	__emit_push_reg(buf, MACH_REG_R12);
	__emit_push_reg(buf, MACH_REG_R13);
	__emit_push_reg(buf, MACH_REG_R14);
	__emit_push_reg(buf, MACH_REG_R15);

	__emit64_push_xmm(buf, MACH_REG_XMM8);
	__emit64_push_xmm(buf, MACH_REG_XMM9);
	__emit64_push_xmm(buf, MACH_REG_XMM10);
	__emit64_push_xmm(buf, MACH_REG_XMM11);
	__emit64_push_xmm(buf, MACH_REG_XMM12);
	__emit64_push_xmm(buf, MACH_REG_XMM13);
	__emit64_push_xmm(buf, MACH_REG_XMM14);
	__emit64_push_xmm(buf, MACH_REG_XMM15);

	/* Save *this. */
	__emit_push_reg(buf, MACH_REG_RDI);
}

void emit_epilog(struct buffer *buf)
{
	emit_restore_regs(buf);
	emit_leave(buf);
	encode_ret(buf);
}

static void emit_restore_regs(struct buffer *buf)
{
	/* Clear *this from stack. */
	__emit64_add_imm_reg(buf, 0x08, MACH_REG_RSP);

	__emit64_pop_xmm(buf, MACH_REG_XMM15);
	__emit64_pop_xmm(buf, MACH_REG_XMM14);
	__emit64_pop_xmm(buf, MACH_REG_XMM13);
	__emit64_pop_xmm(buf, MACH_REG_XMM12);
	__emit64_pop_xmm(buf, MACH_REG_XMM11);
	__emit64_pop_xmm(buf, MACH_REG_XMM10);
	__emit64_pop_xmm(buf, MACH_REG_XMM9);
	__emit64_pop_xmm(buf, MACH_REG_XMM8);

	__emit_pop_reg(buf, MACH_REG_R15);
	__emit_pop_reg(buf, MACH_REG_R14);
	__emit_pop_reg(buf, MACH_REG_R13);
	__emit_pop_reg(buf, MACH_REG_R12);
	__emit_pop_reg(buf, MACH_REG_RBX);
}

static void emit_save_regparm(struct buffer *buf)
{
	__emit64_push_reg(buf, MACH_REG_RDI);
	__emit64_push_reg(buf, MACH_REG_RSI);
	__emit64_push_reg(buf, MACH_REG_RDX);
	__emit64_push_reg(buf, MACH_REG_RCX);
	__emit64_push_reg(buf, MACH_REG_R8);
	__emit64_push_reg(buf, MACH_REG_R9);

	__emit64_push_xmm(buf, MACH_REG_XMM0);
	__emit64_push_xmm(buf, MACH_REG_XMM1);
	__emit64_push_xmm(buf, MACH_REG_XMM2);
	__emit64_push_xmm(buf, MACH_REG_XMM3);
	__emit64_push_xmm(buf, MACH_REG_XMM4);
	__emit64_push_xmm(buf, MACH_REG_XMM5);
	__emit64_push_xmm(buf, MACH_REG_XMM6);
	__emit64_push_xmm(buf, MACH_REG_XMM7);
}

static void emit_restore_regparm(struct buffer *buf)
{
	__emit64_pop_xmm(buf, MACH_REG_XMM7);
	__emit64_pop_xmm(buf, MACH_REG_XMM6);
	__emit64_pop_xmm(buf, MACH_REG_XMM5);
	__emit64_pop_xmm(buf, MACH_REG_XMM4);
	__emit64_pop_xmm(buf, MACH_REG_XMM3);
	__emit64_pop_xmm(buf, MACH_REG_XMM2);
	__emit64_pop_xmm(buf, MACH_REG_XMM1);
	__emit64_pop_xmm(buf, MACH_REG_XMM0);

	__emit64_pop_reg(buf, MACH_REG_R9);
	__emit64_pop_reg(buf, MACH_REG_R8);
	__emit64_pop_reg(buf, MACH_REG_RCX);
	__emit64_pop_reg(buf, MACH_REG_RDX);
	__emit64_pop_reg(buf, MACH_REG_RSI);
	__emit64_pop_reg(buf, MACH_REG_RDI);
}

void emit_trampoline(struct compilation_unit *cu,
		     void *call_target,
		     struct jit_trampoline *trampoline)
{
	struct buffer *buf = trampoline->objcode;

	jit_text_lock();

	buf->buf = jit_text_ptr();

	/* This is for __builtin_return_address() to work and to access
	   call arguments in correct manner. */
	__emit64_push_reg(buf, MACH_REG_RBP);
	__emit64_mov_reg_reg(buf, MACH_REG_RSP, MACH_REG_RBP);

	/*
	 * %rdi, %rsi, %rdx, %rcx, %r8 and %r9 are used
	 * to pass parameters, so save them if they get modified.
	 */
	emit_save_regparm(buf);

	__emit64_mov_imm_reg(buf, (unsigned long) cu, MACH_REG_RDI);
	__emit_call(buf, call_target);

	/*
	 * Test for exception occurance.
	 * We do this by polling a dedicated thread-specific pointer,
	 * which triggers SIGSEGV when exception is set.
	 *
	 * mov fs:(0xXXX), %rcx
	 * test (%rcx), %rcx
	 */
	emit(buf, 0x64);
	__emit_memdisp_reg(buf, 1, 0x8b,
			   get_thread_local_offset(&trampoline_exception_guard),
			   MACH_REG_RCX);
	__emit64_test_membase_reg(buf, MACH_REG_RCX, 0, MACH_REG_RCX);

	emit_restore_regparm(buf);

	__emit64_pop_reg(buf, MACH_REG_RBP);
	emit_indirect_jump_reg(buf, MACH_REG_RAX);

	jit_text_reserve(buffer_offset(buf));
	jit_text_unlock();
}

static void emit_exception_test(struct buffer *buf, enum machine_reg reg)
{
	/* mov fs:(0xXXX), %reg */
	emit(buf, 0x64);
	__emit_memdisp_reg(buf, 1, 0x8b,
		get_thread_local_offset(&exception_guard), reg);

	/* test (%reg), %reg */
	__emit64_test_membase_reg(buf, reg, 0, reg);
}

void emit_lock(struct buffer *buf, struct vm_object *obj)
{
	emit_save_regparm(buf);

	__emit64_mov_imm_reg(buf, (unsigned long) obj, MACH_REG_RDI);
	__emit_call(buf, vm_object_lock);

	emit_restore_regparm(buf);

	__emit64_push_reg(buf, MACH_REG_RAX);
	emit_exception_test(buf, MACH_REG_RAX);
	__emit64_pop_reg(buf, MACH_REG_RAX);
}

void emit_unlock(struct buffer *buf, struct vm_object *obj)
{
	__emit64_push_reg(buf, MACH_REG_RAX);
	emit_save_regparm(buf);

	__emit64_mov_imm_reg(buf, (unsigned long) obj, MACH_REG_RDI);
	__emit_call(buf, vm_object_unlock);

	emit_exception_test(buf, MACH_REG_RAX);

	emit_restore_regparm(buf);
	__emit64_pop_reg(buf, MACH_REG_RAX);
}

void emit_lock_this(struct buffer *buf)
{
	__emit64_mov_membase_reg(buf, MACH_REG_RSP, 0x00, MACH_REG_RDI);
	emit_save_regparm(buf);
	__emit_call(buf, vm_object_lock);
	emit_restore_regparm(buf);

	__emit_push_reg(buf, MACH_REG_RAX);
	emit_exception_test(buf, MACH_REG_RAX);
	__emit_pop_reg(buf, MACH_REG_RAX);
}

void emit_unlock_this(struct buffer *buf)
{
	__emit64_mov_membase_reg(buf, MACH_REG_RSP, 0x00, MACH_REG_RDI);
	__emit_push_reg(buf, MACH_REG_RAX);
	emit_save_regparm(buf);
	__emit_call(buf, vm_object_unlock);

	emit_exception_test(buf, MACH_REG_RAX);

	emit_restore_regparm(buf);
	__emit_pop_reg(buf, MACH_REG_RAX);
}

void *emit_itable_resolver_stub(struct vm_class *vmc,
				struct itable_entry **table,
				unsigned int nr_entries)
{
	return NULL;
}

void itable_resolver_stub_error(struct vm_method *method, struct vm_object *obj)
{
	fprintf(stderr, "itable resolver stub error!\n");
	fprintf(stderr, "invokeinterface called on method %s.%s%s "
		"(itable index %d)\n",
		method->class->name, method->name, method->type,
		method->itable_index);
	fprintf(stderr, "object class %s\n", obj->class->name);

	print_trace();
	abort();
}

void emit_jni_trampoline(struct buffer *buf,
			 struct vm_method *vmm,
			 void *target)
{
	abort();
}

#endif /* CONFIG_X86_32 */

static void do_emit_insn(struct emitter *emitter, struct buffer *buf, struct insn *insn, struct basic_block *bb)
{
	emit_fn_t fn = emitter->emit_fn;

	fn(insn, buf, bb);
}

static void __emit_insn(struct buffer *buf, struct basic_block *bb, struct insn *insn)
{
	struct emitter *emitter;

	emitter = &emitters[insn->type];

	do_emit_insn(emitter, buf, insn, bb);
}

void emit_insn(struct buffer *buf, struct basic_block *bb, struct insn *insn)
{
	insn->mach_offset = buffer_offset(buf);

	__emit_insn(buf, bb, insn);
}

void emit_nop(struct buffer *buf)
{
	emit(buf, 0x90);
}
