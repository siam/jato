/*
 * Copyright (C) 2009 Pekka Enberg
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

#include "jit/exception.h"

#include "vm/backtrace.h"
#include "vm/call.h"
#include "vm/class.h"
#include "vm/gc.h"
#include "vm/jni.h"
#include "vm/object.h"
#include "vm/preload.h"
#include "vm/signal.h"
#include "vm/stack-trace.h"

#include "arch/signal.h"

#include <ucontext.h>
#include <stddef.h>
#include <unistd.h>
#include <stdio.h>

static __thread struct register_state thread_register_state;

static unsigned long throw_arithmetic_exception(unsigned long src_addr)
{
	signal_new_exception(vm_java_lang_ArithmeticException,
			     "division by zero");
	return throw_from_signal_bh(src_addr);
}

static unsigned long throw_null_pointer_exception(unsigned long src_addr)
{
	signal_new_exception(vm_java_lang_NullPointerException, NULL);
	return throw_from_signal_bh(src_addr);
}

static unsigned long throw_stack_overflow_error(unsigned long src_addr)
{
	struct vm_object *obj;

	obj = vm_alloc_stack_overflow_error();
	if (!obj)
		error("failed to allocate instance of StackOverflowError.");

	signal_exception(obj);

	return throw_from_signal_bh(src_addr);
}

static unsigned long rethrow_bh(unsigned long src_addr)
{
	return throw_from_signal_bh(src_addr);
}

static void sigfpe_handler(int sig, siginfo_t *si, void *ctx)
{
	if (signal_from_native(ctx))
		goto exit;

	if (si->si_code == FPE_INTDIV) {
		if (install_signal_bh(ctx, throw_arithmetic_exception) == 0)
			return;

		fprintf(stderr, "%s: install_signal_bh() failed.\n", __func__);
	}

 exit:
	print_backtrace_and_die(sig, si, ctx);
}

static void sigsegv_handler(int sig, siginfo_t *si, void *ctx)
{
	if (signal_from_native(ctx))
		goto exit;

	/* Assume that zero-page access is caused by dereferencing a
	   null pointer */
	if (!si->si_addr) {
		/* We must be extra caucious here because IP might be
		   invalid */
		if (get_signal_source_cu(ctx) == NULL)
			goto exit;

		if (install_signal_bh(ctx, throw_null_pointer_exception) == 0)
			return;

		fprintf(stderr, "%s: install_signal_bh() failed.\n", __func__);
		goto exit;
	}

	/* Garbage collection safepoint */
	if (si->si_addr == gc_safepoint_page) {
		ucontext_t *uc = ctx;

		save_signal_registers(&thread_register_state, uc->uc_mcontext.gregs);
		gc_safepoint(&thread_register_state);
		return;
	}

	/* Check if exception was triggered by exception guard */
	if (si->si_addr == exceptions_guard_page ||
	    si->si_addr == trampoline_exceptions_guard_page) {
		struct vm_object *exception;

		exception = exception_occurred();
		if (exception == NULL) {
			fprintf(stderr, "%s: spurious exception-test failure\n",
				__func__);
			goto exit;
		}

		if (si->si_addr == trampoline_exceptions_guard_page)
			throw_from_trampoline(ctx, exception);
		else
			install_signal_bh(ctx, &rethrow_bh);

		return;
	}

	/* Static field access */
	if (si->si_addr == static_guard_page) {
		install_signal_bh(ctx, &static_field_signal_bh);
		return;
	}

	if (si->si_addr == jni_stack_badoffset ||
	    si->si_addr == vm_native_stack_badoffset) {
		install_signal_bh(ctx, throw_stack_overflow_error);
		return;
	}

 exit:
	vm_jni_check_trap(si->si_addr);

	print_backtrace_and_die(sig, si, ctx);
}

void setup_signal_handlers(void)
{
	struct sigaction sa;
	sigset_t sigusr_mask;
	sigset_t sigset;

	sigemptyset(&sigusr_mask);
	sigaddset(&sigusr_mask, SIGUSR1);
	sigaddset(&sigusr_mask, SIGUSR2);

	sigemptyset(&sa.sa_mask);
	sa.sa_flags	= SA_RESTART | SA_SIGINFO;

	/*
	 * Block SIGUSR1 during SIGSEGV to prevent suspension when
	 * entering safepoint through a SIGSEGV. Calls to sigwait() in
	 * gc_jit_safepoint() assume that SIGUSR1 and SIGUSR2 are
	 * blocked.
	 */
	sa.sa_sigaction	= sigsegv_handler;
	sa.sa_mask	= sigusr_mask;
	sigaction(SIGSEGV, &sa, NULL);
	sigemptyset(&sa.sa_mask);

	sa.sa_sigaction	= sigfpe_handler;
	sigaction(SIGFPE, &sa, NULL);

	sa.sa_sigaction	= suspend_handler;
	sigaction(SIGUSR1, &sa, NULL);

	sa.sa_sigaction	= wakeup_handler;
	sigaction(SIGUSR2, &sa, NULL);

	/*
	 * SIGUSR2 is used to resume threads. Make sure the signal is blocked
	 * by default to avoid races with sigwait().
	 */
	sigemptyset(&sigset);
	sigaddset(&sigset, SIGUSR2);
	pthread_sigmask(SIG_BLOCK, &sigset, NULL);
}
