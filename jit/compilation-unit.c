/*
 * Copyright (c) 2006-2008  Pekka Enberg
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
#include "arch/registers.h"

#include "jit/basic-block.h"
#include "jit/compilation-unit.h"
#include "jit/instruction.h"
#include "jit/stack-slot.h"
#include "jit/statement.h"
#include "jit/vars.h"
#include "lib/buffer.h"
#include "vm/method.h"
#include "vm/die.h"
#include "vm/vm.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

static struct var_info *
do_get_var(struct compilation_unit *cu, enum vm_type vm_type)
{
	struct var_info *ret;

	if (cu->is_reg_alloc_done) {
		die("cannot allocate temporaries after register allocation");
	}

	ret = malloc(sizeof *ret);
	if (!ret)
		goto out;

	ret->vreg = cu->nr_vregs++;
	ret->next = cu->var_infos;
	ret->vm_type = vm_type;

	ret->interval = alloc_interval(ret);

	cu->var_infos = ret;
  out:
	return ret;
}

struct compilation_unit *compilation_unit_alloc(struct vm_method *method)
{
	struct compilation_unit *cu = malloc(sizeof *cu);
	if (cu) {
		memset(cu, 0, sizeof *cu);

		INIT_LIST_HEAD(&cu->bb_list);
		cu->method = method;
		cu->is_compiled = false;

		cu->exit_bb = alloc_basic_block(cu, 0, 0);
		if (!cu->exit_bb)
			goto out_of_memory;

		cu->unwind_bb = alloc_basic_block(cu, 0, 0);
		if (!cu->unwind_bb)
			goto out_of_memory;

		pthread_mutex_init(&cu->mutex, NULL);

		cu->stack_frame = alloc_stack_frame(
			method->args_count,
			method->code_attribute.max_locals);
		if (!cu->stack_frame)
			goto out_of_memory;

		cu->exception_spill_slot = get_spill_slot_32(cu->stack_frame);
		if (!cu->exception_spill_slot)
			goto out_of_memory;

		INIT_LIST_HEAD(&cu->static_fixup_site_list);
		INIT_LIST_HEAD(&cu->call_fixup_site_list);
		INIT_LIST_HEAD(&cu->tableswitch_list);
		INIT_LIST_HEAD(&cu->lookupswitch_list);

		for (unsigned int i = 0; i < NR_FIXED_REGISTERS; ++i) {
			struct var_info *ret;
			enum vm_type type;

			type = reg_default_type(i);
			ret = do_get_var(cu, type);
			if (ret) {
				ret->interval->reg	= i;
				ret->interval->flags	|= INTERVAL_FLAG_FIXED_REG;
			}

			cu->fixed_var_infos[i] = ret;
		}

		cu->lir_insn_map = NULL;
	}

	return cu;

out_of_memory:
	free_compilation_unit(cu);
	return NULL;
}

static void free_var_info(struct var_info *var)
{
	free_interval(var->interval);
	free(var);
}

static void free_var_infos(struct var_info *var_infos)
{
	struct var_info *this, *next;

	for (this = var_infos; this != NULL; this = next) {
		next = this->next;
		free_var_info(this);
	}
}

static void free_bc_offset_map(unsigned long *map)
{
	free(map);
}

static void free_tableswitch_list(struct compilation_unit *cu)
{
	struct tableswitch *this, *next;

	list_for_each_entry_safe(this, next, &cu->tableswitch_list, list_node) {
		list_del(&this->list_node);
		free_tableswitch(this);
	}
}

static void free_lookupswitch_list(struct compilation_unit *cu)
{
	struct lookupswitch *this, *next;

	list_for_each_entry_safe(this, next, &cu->lookupswitch_list, list_node)
	{
		list_del(&this->list_node);
		free_lookupswitch(this);
	}
}

static void free_call_fixup_sites(struct compilation_unit *cu)
{
	struct fixup_site *this, *next;

	list_for_each_entry_safe(this, next, &cu->call_fixup_site_list, cu_node)
	{
		list_del(&this->cu_node);

		pthread_mutex_lock(&this->target->mutex);
		list_del(&this->trampoline_node);
		pthread_mutex_unlock(&this->target->mutex);

		free_fixup_site(this);
	}
}

static void free_lir_insn_map(struct compilation_unit *cu)
{
	free_radix_tree(cu->lir_insn_map);
}

/* Free everything that is not required at run-time.  */
void shrink_compilation_unit(struct compilation_unit *cu)
{
	struct basic_block *bb, *tmp_bb;

	list_for_each_entry_safe(bb, tmp_bb, &cu->bb_list, bb_list_node)
		shrink_basic_block(bb);

	free_var_infos(cu->var_infos);
	cu->var_infos = NULL;
}

void free_compilation_unit(struct compilation_unit *cu)
{
	struct basic_block *bb, *tmp_bb;

	free_call_fixup_sites(cu);
	shrink_compilation_unit(cu);

	list_for_each_entry_safe(bb, tmp_bb, &cu->bb_list, bb_list_node)
		free_basic_block(bb);

	pthread_mutex_destroy(&cu->mutex);
	free_basic_block(cu->exit_bb);
	free_basic_block(cu->unwind_bb);
	free_buffer(cu->objcode);
	free_stack_frame(cu->stack_frame);
	free_bc_offset_map(cu->bc_offset_map);
	free_lookupswitch_list(cu);
	free_tableswitch_list(cu);
	free_lir_insn_map(cu);
	free(cu->exception_handlers);

	free(cu);
}

struct var_info *get_var(struct compilation_unit *cu, enum vm_type vm_type)
{
	return do_get_var(cu, vm_type);
}

struct var_info *get_fixed_var(struct compilation_unit *cu, enum machine_reg reg)
{
	assert(reg < NR_FIXED_REGISTERS);

	return cu->fixed_var_infos[reg];
}

/**
 * 	bb_find - Find basic block containing @offset.
 * 	@bb_list: First basic block in list.
 * 	@offset: Offset to find.
 * 
 * 	Find the basic block that contains the given offset and returns a
 * 	pointer to it.
 */
struct basic_block *find_bb(struct compilation_unit *cu, unsigned long offset)
{
	struct basic_block *bb;

	for_each_basic_block(bb, &cu->bb_list) {
		if (offset >= bb->start && offset < bb->end)
			return bb;
	}
	return NULL;
}

unsigned long nr_bblocks(struct compilation_unit *cu)
{
	struct basic_block *bb;
	unsigned long nr = 0;

	for_each_basic_block(bb, &cu->bb_list) {
		nr++;
	}

	return nr;
}

void compute_insn_positions(struct compilation_unit *cu)
{
	struct basic_block *bb;
	unsigned long pos = 0;

	cu->lir_insn_map = alloc_radix_tree(8, 8 * sizeof(pos));
	if (!cu->lir_insn_map)
		die("oom");

	for_each_basic_block(bb, &cu->bb_list) {
		struct insn *insn;

		bb->start_insn = pos;

		for_each_insn(insn, &bb->insn_list) {
			insn->lir_pos = pos;

			radix_tree_insert(cu->lir_insn_map, pos, insn);

			pos += 2;
		}

		bb->end_insn = pos;
	}

	cu->last_insn = pos;
}

static void resolve_static_fixup_offsets(struct compilation_unit *cu)
{
	struct static_fixup_site *this;

	list_for_each_entry(this, &cu->static_fixup_site_list, cu_node) {
		this->mach_offset = this->insn->mach_offset;
	}
}

static void resolve_call_fixup_offsets(struct compilation_unit *cu)
{
	struct fixup_site *this;

	list_for_each_entry(this, &cu->call_fixup_site_list, cu_node) {
		this->mach_offset = this->relcall_insn->mach_offset;
	}
}

void resolve_fixup_offsets(struct compilation_unit *cu)
{
	resolve_call_fixup_offsets(cu);

	resolve_static_fixup_offsets(cu);
}

struct stack_slot *get_scratch_slot(struct compilation_unit *cu)
{
	if (!cu->scratch_slot)
		cu->scratch_slot = get_spill_slot_64(cu->stack_frame);

	return cu->scratch_slot;
}
