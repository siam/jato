#ifndef JATO_JIT_EXCEPTION_H
#define JATO_JIT_EXCEPTION_H

#include <stdbool.h>

#include "cafebabe/code_attribute.h"

#include "arch/stack-frame.h"
#include "jit/compilation-unit.h"
#include "jit/compiler.h"
#include "jit/cu-mapping.h"

#include "vm/die.h"
#include "vm/method.h"
#include "vm/stack-trace.h"
#include "vm/vm.h"

struct cafebabe_code_attribute_exception;
struct compilation_unit;
struct jit_stack_frame;
struct vm_object;
struct vm_method;

/*
 * This is a per-thread pointer to a memory location which should be
 * polled by JIT code to check for asynchronous exception
 * occurrance. When exception is set this pointer will point to a
 * hidden guard page which will trigger SIGSEGV on access. The signal
 * handler will throw the exception then.
 */
extern __thread void *exception_guard;

/* Same as exception_guard but destined to be used in trampolines
   to distinguish between them and the general case. */
extern __thread void *trampoline_exception_guard;

extern void *exceptions_guard_page;
extern void *trampoline_exceptions_guard_page;

/*
 * Holds a reference to exception that has been signalled.  This
 * pointer is cleared when handler is executed or
 * clear_exception() is called.
 */
extern __thread struct vm_object *exception_holder;

struct cafebabe_code_attribute_exception *
lookup_eh_entry(struct vm_method *method, unsigned long target);

unsigned char *throw_from_jit(struct compilation_unit *cu,
			      struct jit_stack_frame *frame,
			      unsigned char *native_ptr);
unsigned char *throw_from_jit_checked(struct compilation_unit *cu,
				      struct jit_stack_frame *frame,
				      unsigned char *native_ptr);
int insert_exception_spill_insns(struct compilation_unit *cu);
unsigned char *throw_exception(struct compilation_unit *cu,
			       struct vm_object *exception);
void throw_from_trampoline(void *ctx, struct vm_object *exception);
void unwind(void);
void exception_check(void);
void signal_exception(struct vm_object *obj);
void signal_new_exception(struct vm_class *vmc, const char *msg);
void signal_new_exception_with_cause(struct vm_class *vmc,
				     struct vm_object *cause,
				     const char *msg);
void clear_exception(void);
void init_exceptions(void);
void thread_init_exceptions(void);
void print_exception_table(const struct vm_method *,
	const struct cafebabe_code_attribute_exception *, int);
int build_exception_handlers_table(struct compilation_unit *cu);

static inline bool
exception_covers(struct cafebabe_code_attribute_exception *eh, unsigned long offset)
{
	return eh->start_pc <= offset && offset < eh->end_pc;
}

static inline struct vm_object *exception_occurred(void)
{
	return exception_holder;
}

#endif /* JATO_JIT_EXCEPTION_H */
