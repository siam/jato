/*
 * Copyright (c) 2009 Tomasz Grabiec
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

#include "cafebabe/code_attribute.h"

#include "jit/exception.h"
#include "jit/compilation-unit.h"
#include "jit/bc-offset-mapping.h"
#include "jit/basic-block.h"
#include "jit/exception.h"
#include "jit/compiler.h"

#include "lib/buffer.h"
#include "lib/guard-page.h"

#include "vm/preload.h"
#include "vm/method.h"
#include "vm/object.h"
#include "vm/thread.h"
#include "vm/class.h"
#include "vm/trace.h"
#include "vm/call.h"
#include "vm/die.h"

#include "arch/stack-frame.h"
#include "arch/instruction.h"
#include <errno.h>

__thread struct vm_object *exception_holder = NULL;
__thread void *exception_guard = NULL;
__thread void *trampoline_exception_guard = NULL;

void *exceptions_guard_page;
void *trampoline_exceptions_guard_page;

void init_exceptions(void)
{
	exceptions_guard_page = alloc_guard_page(true);
	trampoline_exceptions_guard_page = alloc_guard_page(true);

	if (!exceptions_guard_page || !trampoline_exceptions_guard_page)
		die("failed to allocate exceptions guard page");

	/* TODO: Should be called from thread initialization code. */
	thread_init_exceptions();
}

/**
 * thread_init_exceptions - initializes per-thread structures.
 */
void thread_init_exceptions(void)
{
	/* Assign safe pointers. */
	exception_guard = &exception_guard;
	trampoline_exception_guard = &trampoline_exception_guard;
}

/**
 * signal_exception - used for signaling that exception has occurred
 *         in jato functions. Exception will be thrown when controll
 *         is returned to JIT code.
 *
 * @exception: exception object to be thrown.
 */
void signal_exception(struct vm_object *exception)
{
	assert(exception);

	trampoline_exception_guard = trampoline_exceptions_guard_page;
	exception_guard  = exceptions_guard_page;
	exception_holder = exception;
}

void signal_new_exception(struct vm_class *vmc, const char *msg)
{
	struct vm_object *exception;

	exception = new_exception(vmc, msg);
	if (!exception)
		die("out of memory");

	signal_exception(exception);
}

void signal_new_exception_with_cause(struct vm_class *vmc,
				     struct vm_object *cause,
				     const char *msg)
{
	struct vm_object *exception;
	struct vm_method *init;

	/*
	 * Some exception classes have dedicated constructors for
	 * setting exception's cause. For such classes we shouldn't
	 * set the cause with initCause(). See for example
	 * java/lang/ExceptionInInitializerError class.
	 */
	init = vm_class_get_method(vmc, "<init>", "(Ljava/lang/Throwable;)V");
	if (init) {
		exception = vm_object_alloc(vmc);
		if (!exception)
			die("out of memory");

		clear_exception();

		vm_call_method(init, exception, cause);

		if (exception_occurred())
			return;

		signal_exception(exception);
	}

	exception = new_exception(vmc, msg);
	if (!exception)
		die("out of memory");

	clear_exception();

	vm_call_method(vm_java_lang_Throwable_initCause, exception, cause);

	if (exception_occurred())
		return;

	signal_exception(exception);
}

void clear_exception(void)
{
	trampoline_exception_guard = &trampoline_exception_guard;
	exception_guard  = &exception_guard;
	exception_holder = NULL;
}

struct cafebabe_code_attribute_exception *
lookup_eh_entry(struct vm_method *method, unsigned long target)
{
	int i;

	for (i = 0; i < method->code_attribute.exception_table_length; i++) {
		struct cafebabe_code_attribute_exception *eh
			= &method->code_attribute.exception_table[i];

		if (eh->handler_pc == target)
			return eh;
	}

	return NULL;
}

static unsigned char *eh_native_ptr(struct compilation_unit *cu,
	struct cafebabe_code_attribute_exception *eh)
{
	struct basic_block *bb;

	bb = find_bb(cu, eh->handler_pc);
	assert(bb != NULL);

	return bb_native_ptr(bb);
}

int build_exception_handlers_table(struct compilation_unit *cu)
{
	struct vm_method *method;
	int size;
	int i;

	method = cu->method;
	size = method->code_attribute.exception_table_length;

	if (size == 0)
		return 0;

	cu->exception_handlers = malloc(sizeof(void *) * size);
	if (!cu->exception_handlers)
		return -ENOMEM;

	for (i = 0; i < size; i++) {
		struct cafebabe_code_attribute_exception *eh
			= &method->code_attribute.exception_table[i];

		cu->exception_handlers[i] = eh_native_ptr(cu, eh);
	}

	return 0;
}

/**
 * find_handler - return native pointer to exception handler for given
 *                @exception_class and @bc_offset of source.
 */
static unsigned char *find_handler(struct compilation_unit *cu,
	struct vm_class *exception_class, unsigned long bc_offset)
{
	struct cafebabe_code_attribute_exception *eh;
	struct vm_method *method;
	int size;
	int i;

	method = cu->method;
	size = method->code_attribute.exception_table_length;

	for (i = 0; i < size; i++) {
		struct vm_class *catch_class;

		eh = &method->code_attribute.exception_table[i];
		if (!exception_covers(eh, bc_offset))
			continue;

		/* This matches to everything. */
		if (eh->catch_type == 0)
			break;

		catch_class = vm_class_resolve_class(method->class,
			eh->catch_type);

		if (vm_class_is_assignable_from(catch_class, exception_class))
			break;
	}

	if (i < size)
		return cu->exception_handlers[i];

	return NULL;
}

static bool
is_inside_exit_unlock(struct compilation_unit *cu, unsigned char *ptr)
{
	return ptr >= cu->exit_bb_ptr &&
		ptr < cu->exit_past_unlock_ptr;
}

static bool
is_inside_unwind_unlock(struct compilation_unit *cu, unsigned char *ptr)
{
	return ptr >= cu->unwind_bb_ptr &&
		ptr < cu->unwind_past_unlock_ptr;
}

/**
 * throw_from_jit - returns native pointer inside jitted method
 *                  that sould be executed to handle exception.
 *                  This can be one of the following:
 *                        1) registered exception handler (catch/finally block)
 *                        2) method's unwind block (when no handler is found)
 *                        3) method's exit block (when no handler is found and
 *                           unwind can't be done because the method's caller
 *                           is not a jitted method).
 *
 * @cu: compilation unit
 * @frame: frame pointer of method throwing exception
 * @native_ptr: pointer to instruction that caused exception
 */
unsigned char *
throw_from_jit(struct compilation_unit *cu, struct jit_stack_frame *frame,
	       unsigned char *native_ptr)
{
	struct vm_object *exception;
	unsigned long bc_offset;
	unsigned char *eh_ptr;

	eh_ptr = NULL;

	exception = exception_occurred();
	assert(exception != NULL);

	if (opt_trace_exceptions)
		trace_exception(cu, frame, native_ptr);

	clear_exception();

	bc_offset = jit_lookup_bc_offset(cu, native_ptr);
	if (bc_offset != BC_OFFSET_UNKNOWN) {
		eh_ptr = find_handler(cu, exception->class, bc_offset);
		if (eh_ptr != NULL) {
			signal_exception(exception);

			if (opt_trace_exceptions)
				trace_exception_handler(cu, eh_ptr);

			return eh_ptr;
		}
	}

	signal_exception(exception);

	if (is_native(frame->return_address)) {
		/*
		 * No handler found within jitted method call chain.
		 * Return to previous (not jit) method.
		 */
		if (opt_trace_exceptions)
			trace_exception_unwind_to_native(frame);

		if (is_inside_exit_unlock(cu, native_ptr))
			return cu->exit_past_unlock_ptr;

		return cu->exit_bb_ptr;
	}

	if (opt_trace_exceptions)
		trace_exception_unwind(frame);

	if (is_inside_unwind_unlock(cu, native_ptr))
		return cu->unwind_past_unlock_ptr;

	return cu->unwind_bb_ptr;
}

void
print_exception_table(const struct vm_method *method,
	const struct cafebabe_code_attribute_exception *exception_table,
	int exception_table_length)
{
	if (exception_table_length == 0) {
		trace_printf("\t(empty)\n");
		return;
	}

	trace_printf("\tfrom\tto\ttarget\ttype\n");
	for (int i = 0; i < exception_table_length; i++) {
		const struct cafebabe_code_attribute_exception *eh;

		eh = &exception_table[i];

		trace_printf("\t%d\t%d\t%d\t", eh->start_pc, eh->end_pc,
		       eh->handler_pc);

		if (!eh->catch_type) {
			trace_printf("all\n");
			continue;
		}

		const struct vm_class *catch_class;
		catch_class = vm_class_resolve_class(method->class,
						     eh->catch_type);

		trace_printf("Class %s\n", catch_class->name);
	}
}

unsigned char *
throw_from_jit_checked(struct compilation_unit *cu, struct jit_stack_frame *frame,
		       unsigned char *native_ptr)
{
	if (exception_occurred())
		return throw_from_jit(cu, frame, native_ptr);

	return NULL;
}
