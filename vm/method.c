#include "cafebabe/attribute_array.h"
#include "cafebabe/attribute_info.h"
#include "cafebabe/line_number_table_attribute.h"
#include "cafebabe/class.h"
#include "cafebabe/code_attribute.h"
#include "cafebabe/constant_pool.h"
#include "cafebabe/method_info.h"
#include "cafebabe/stream.h"

#include "vm/class.h"
#include "vm/method.h"
#include "vm/natives.h"

#include "jit/compilation-unit.h"
#include "jit/cu-mapping.h"
#include "jit/args.h"
#include "jit/gdb.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static void init_abstract_method(struct vm_method *vmm)
{
	/* Hm, we're now modifying a cafebabe structure. */
	vmm->code_attribute.max_stack = 0;
	vmm->code_attribute.max_locals = vmm->args_count;

	vmm->line_number_table_attribute.line_number_table_length = 0;
	vmm->line_number_table_attribute.line_number_table = NULL;
}

int vm_method_init(struct vm_method *vmm,
	struct vm_class *vmc, unsigned int method_index)
{
	const struct cafebabe_class *class = vmc->class;
	const struct cafebabe_method_info *method
		= &class->methods[method_index];

	vmm->class = vmc;
	vmm->method_index = method_index;
	vmm->method = method;

	const struct cafebabe_constant_info_utf8 *name;
	if (cafebabe_class_constant_get_utf8(class, method->name_index, &name))
		return -1;

	vmm->name = strndup((char *) name->bytes, name->length);
	if (!vmm->name)
		return -1;

	const struct cafebabe_constant_info_utf8 *type;
	if (cafebabe_class_constant_get_utf8(class, method->descriptor_index, &type))
		goto error_free_name;

	vmm->type = strndup((char *) type->bytes, type->length);
	if (!vmm->type)
		goto error_free_name;

	if (parse_method_type(vmm)) {
		warn("method type parsing failed for: %s", vmm->type);
		goto error_free_type;
	}

	/*
	 * XXX: Jam VM legacy? It seems that JamVM counts the number of
	 * _32-bit_ arguments. This probably needs some fixing. What do we do
	 * on x86_64?
	 */
	vmm->args_count = count_arguments(vmm);
	if (vmm->args_count < 0)
		goto error_free_type;

	if (!vm_method_is_static(vmm))
		++vmm->args_count;

	vmm->is_vm_native = false;

	if (vm_method_is_native(vmm)) {
		vmm->is_vm_native =
			vm_lookup_native(vmm->class->name, vmm->name);
	}

	if (args_map_init(vmm))
		goto error_free_type;

	/*
	 * Note: We can return here because the rest of the function deals
	 * with loading attributes which native and abstract methods don't have.
	 */
	if (vm_method_is_native(vmm) || vm_method_is_abstract(vmm)) {
		init_abstract_method(vmm);
		return 0;
	}

	unsigned int code_index = 0;
	if (cafebabe_attribute_array_get(&method->attributes, "Code", class, &code_index))
		goto error_free_type;

	/* There must be only one "Code" attribute for the method! */
	unsigned int code_index2 = code_index + 1;
	if (!cafebabe_attribute_array_get(&method->attributes, "Code", class, &code_index2))
		goto error_free_type;

	const struct cafebabe_attribute_info *attribute
		= &method->attributes.array[code_index];

	struct cafebabe_stream stream;
	cafebabe_stream_open_buffer(&stream,
		attribute->info, attribute->attribute_length);

	if (cafebabe_code_attribute_init(&vmm->code_attribute, &stream))
		goto error_free_type;

	cafebabe_stream_close_buffer(&stream);

	if (cafebabe_read_line_number_table_attribute(class, &vmm->code_attribute.attributes, &vmm->line_number_table_attribute))
		goto error_free_type;

	return 0;

error_free_type:
	free(vmm->type);
error_free_name:
	free(vmm->name);

	return -1;
}

int vm_method_init_from_interface(struct vm_method *vmm, struct vm_class *vmc,
				  unsigned int method_index,
				  struct vm_method *interface_method)
{
	/* NOTE: If we ever keep reference counts on loaded classes, we should
	 * perhaps _copy_ the interformation from the interface method instead
	 * of just grabbing a reference to the same information. */

	vmm->class = vmc;
	vmm->method_index = method_index;
	vmm->method = interface_method->method;

	vmm->name = interface_method->name;
	vmm->type = interface_method->type;

	vmm->args_count = interface_method->args_count;
	vmm->is_vm_native = false;

	if (parse_method_type(vmm)) {
		warn("method type parsing failed for: %s", vmm->type);
		return -1;
	}

	init_abstract_method(vmm);

	return 0;
}

int vm_method_prepare_jit(struct vm_method *vmm)
{
	struct compilation_unit *cu;

	cu = compilation_unit_alloc(vmm);
	if (!cu)
		return -1;

	vmm->compilation_unit = cu;

	/*
	 * VM native methods are linked on initialization.
	 */
	if (vm_method_is_vm_native(vmm)) {
		cu->native_ptr = vm_lookup_native(vmm->class->name, vmm->name);
		cu->is_compiled = true;

		if (add_cu_mapping((unsigned long)cu->native_ptr, cu))
			goto error_free_cu;
	}

	vmm->trampoline = build_jit_trampoline(cu);
	if (!vmm->trampoline)
		goto error_free_cu;

	gdb_register_trampoline(vmm);

	return 0;

error_free_cu:
	free_compilation_unit(cu);

	return -1;
}
