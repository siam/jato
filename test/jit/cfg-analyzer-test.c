/*
 * Copyright (C) 2006  Pekka Enberg
 */

#include "vm/method.h"
#include "vm/system.h"
#include "vm/vm.h"
#include "jit/compiler.h"
#include <libharness.h>
#include <basic-block-assert.h>

/* public String defaultString(String s) { if (s == null) { s = ""; } return s; } */
static unsigned char default_string[9] = {
	/* 0 */ OPC_ALOAD_1,
	/* 1 */ OPC_IFNONNULL, 0x00, 0x06, /* Jumps to 0x07 */
	
	/* 4 */ OPC_LDC, 0x02,
	/* 6 */ OPC_ASTORE_1,

	/* 7 */ OPC_ALOAD_1,
	/* 8 */ OPC_ARETURN,
};

void test_branch_opcode_ends_basic_block(void)
{
	struct basic_block *bb1, *bb2, *bb3;
	struct compilation_unit *cu;
	struct vm_method method = {
		.code_attribute.code = default_string,
		.code_attribute.code_length = ARRAY_SIZE(default_string)
	};
	
	cu = compilation_unit_alloc(&method);

	analyze_control_flow(cu);

	assert_int_equals(3, nr_bblocks(cu));

	bb1 = bb_entry(cu->bb_list.next);
	bb2 = bb_entry(bb1->bb_list_node.next);
	bb3 = bb_entry(bb2->bb_list_node.next);

	assert_basic_block(cu, 0, 4, bb1);
	assert_basic_block(cu, 4, 7, bb2);
	assert_basic_block(cu, 7, 9, bb3);

	assert_basic_block_successors((struct basic_block*[]){bb2, bb3}, 2, bb1);
	assert_basic_block_successors((struct basic_block*[]){bb3     }, 1, bb2);
	assert_basic_block_successors((struct basic_block*[]){        }, 0, bb3);

	free_compilation_unit(cu);
}

/* public boolean greaterThanZero(int i) { return i > 0; } */ 
static unsigned char greater_than_zero[10] = {
	/* 0 */ OPC_ILOAD_1,
	/* 1 */ OPC_IFLE, 0x00, 0x07,

	/* 4 */ OPC_ICONST_1,
	/* 5 */ OPC_GOTO, 0x00, 0x04,

	/* 8 */ OPC_ICONST_0,

	/* 9 */ OPC_IRETURN,
};

void test_multiple_branches(void)
{
	struct basic_block *bb1, *bb2, *bb3, *bb4;
	struct compilation_unit *cu;

	struct vm_method method = {
		.code_attribute.code = greater_than_zero,
		.code_attribute.code_length = ARRAY_SIZE(greater_than_zero) 
	};

	cu = compilation_unit_alloc(&method);

	analyze_control_flow(cu);
	assert_int_equals(4, nr_bblocks(cu));

	bb1 = bb_entry(cu->bb_list.next);
	bb2 = bb_entry(bb1->bb_list_node.next);
	bb3 = bb_entry(bb2->bb_list_node.next);
	bb4 = bb_entry(bb3->bb_list_node.next);

	assert_basic_block_successors((struct basic_block*[]){bb2, bb3}, 2, bb1);
	assert_basic_block_successors((struct basic_block*[]){bb4     }, 1, bb2);
	assert_basic_block_successors((struct basic_block*[]){bb4     }, 1, bb3);
	assert_basic_block_successors((struct basic_block*[]){        }, 0, bb4);

	free_compilation_unit(cu);
}

/*
 * public void setValue(int i) {
 * 	int j;
 *
 * 	if (i == 0)
 * 		j = 0;
 *
 * 	j = 0;
 *
 * 	if (i = 0)
 * 		j = 0;
 *
 * 	return;
 * }
 */
static unsigned char set_value[15] = {
	/* 0 */ OPC_ILOAD_0,
	/* 1 */ OPC_IFNE, 0x00, 0x05, /* jump to 0x06 */

	/* 4 */ OPC_ICONST_0,
	/* 5 */ OPC_ISTORE_1,

	/* 6 */ OPC_ICONST_0,
	/* 7 */ OPC_ISTORE_1,
	/* 8 */ OPC_ILOAD_0,
	/* 9 */ OPC_IFNE, 0x00, 0x05, /* jump tp 0x0E */

	/* 12 */ OPC_ICONST_0,
	/* 13 */ OPC_ISTORE_1,

	/* 14 */ OPC_RETURN,

};

void test_multiple_branch_with_target_instruction_splitting(void)
{
	struct basic_block *bb1, *bb2, *bb3, *bb4, *bb5;
	struct compilation_unit *cu;
	struct vm_method method = {
		.code_attribute.code = set_value,
		.code_attribute.code_length = ARRAY_SIZE(set_value)
	};

	cu = compilation_unit_alloc(&method);

	analyze_control_flow(cu);

	assert_int_equals(5, nr_bblocks(cu));

	bb1 = bb_entry(cu->bb_list.next);
	bb2 = bb_entry(bb1->bb_list_node.next);
	bb3 = bb_entry(bb2->bb_list_node.next);
	bb4 = bb_entry(bb3->bb_list_node.next);
	bb5 = bb_entry(bb4->bb_list_node.next);

	assert_basic_block(cu, 0, 4, bb1);
	assert_basic_block(cu, 4, 6, bb2);
	assert_basic_block(cu, 6, 12, bb3);
	assert_basic_block(cu, 12, 14, bb4);
	assert_basic_block(cu, 14, 15, bb5);

	assert_basic_block_successors((struct basic_block*[]){bb2, bb3}, 2, bb1);
	assert_basic_block_successors((struct basic_block*[]){bb3     }, 1, bb2);
	assert_basic_block_successors((struct basic_block*[]){bb4, bb5}, 2, bb3);
	assert_basic_block_successors((struct basic_block*[]){bb5     }, 1, bb4);
	assert_basic_block_successors((struct basic_block*[]){        }, 0, bb5);

	free_compilation_unit(cu);
}
