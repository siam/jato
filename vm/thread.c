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

#include "vm/call.h"
#include "vm/class.h"
#include "vm/die.h"
#include "vm/gc.h"
#include "vm/object.h"
#include "vm/preload.h"
#include "vm/signal.h"
#include "vm/stdlib.h"
#include "vm/thread.h"

#include "jit/exception.h"

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

__thread struct vm_exec_env current_exec_env;

static struct vm_object *main_thread_group;

/* This mutex protects global operations on thread structures. */
pthread_mutex_t threads_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Condition variable used for waiting on any thread's death. */
static pthread_cond_t thread_terminate_cond = PTHREAD_COND_INITIALIZER;

static unsigned int nr_threads;
static int nr_non_daemons;

struct list_head thread_list;

static bool thread_count_locked;
static pthread_cond_t thread_count_lock_cond = PTHREAD_COND_INITIALIZER;

static void vm_thread_free(struct vm_thread *thread)
{
	free(thread);
}

static struct vm_thread *vm_thread_alloc(void)
{
	struct vm_thread *thread = malloc(sizeof(struct vm_thread));
	if (!thread)
		return NULL;

	if (pthread_mutex_init(&thread->mutex, NULL)) {
		warn("pthread_mutex_init() failed");
		vm_thread_free(thread);
		return NULL;
	}

	thread->state = VM_THREAD_STATE_NEW;
	thread->vmthread = NULL;
	thread->posix_id = -1;
	thread->interrupted = false;
	thread->wait_mon = NULL;
	thread->thread_state = THREAD_STATE_CONSISTENT;

	return thread;
}

/**
 * Returns instance of java.lang.Thread associated with given thread.
 */
struct vm_object *vm_thread_get_java_thread(struct vm_thread *thread)
{
	struct vm_object *result;

	pthread_mutex_lock(&thread->mutex);
	result = field_get_object(thread->vmthread,
				  vm_java_lang_VMThread_thread);
	pthread_mutex_unlock(&thread->mutex);

	return result;
}

static bool vm_thread_is_daemon(struct vm_thread *thread)
{
	struct vm_object *jthread;

	jthread = vm_thread_get_java_thread(thread);
	return field_get_int(jthread, vm_java_lang_Thread_daemon) != 0;
}

/* Must hold threads_mutex */
unsigned int vm_nr_threads(void)
{
	return nr_threads;
}

void vm_thread_set_state(struct vm_thread *thread, enum vm_thread_state state)
{
	pthread_mutex_lock(&thread->mutex);
	thread->state = state;
	pthread_mutex_unlock(&thread->mutex);
}

static void vm_thread_attach_thread(struct vm_thread *thread)
{
	list_add(&thread->list_node, &thread_list);
	nr_threads++;
}

/* The caller must hold threads_mutex */
static void vm_thread_detach_thread(struct vm_thread *thread)
{
	vm_thread_set_state(thread, VM_THREAD_STATE_TERMINATED);

	list_del(&thread->list_node);

	if (!vm_thread_is_daemon(thread))
		nr_non_daemons--;

	nr_threads--;

	pthread_cond_broadcast(&thread_terminate_cond);
}

int init_threading(void)
{
	INIT_LIST_HEAD(&thread_list);
	nr_non_daemons = 0;
	nr_threads = 0;

	main_thread_group = vm_object_alloc(vm_java_lang_ThreadGroup);
	if (!main_thread_group)
		return -ENOMEM;

	vm_call_method(vm_java_lang_ThreadGroup_init, main_thread_group);
	if (exception_occurred())
		return -1;

	/*
	 * Initialize the main thread
	 */
	struct vm_object *thread = vm_object_alloc(vm_java_lang_Thread);
	if (!thread)
		return -ENOMEM;

	struct vm_object *thread_name = vm_object_alloc_string_from_c("main");
	if (!thread_name)
		return -ENOMEM;

	struct vm_object *vmthread = vm_object_alloc(vm_java_lang_VMThread);
	if (!vmthread)
		return -ENOMEM;

	struct vm_thread *main_thread = vm_thread_alloc();
	if (!main_thread)
		return -ENOMEM;

	main_thread->state = VM_THREAD_STATE_RUNNABLE;
	main_thread->vmthread = vmthread;
	main_thread->posix_id = pthread_self();

	vm_get_exec_env()->thread = main_thread;

	field_set_int(thread, vm_java_lang_Thread_priority, 5);
	field_set_int(thread, vm_java_lang_Thread_daemon, 0);
	field_set_object(thread, vm_java_lang_Thread_name, thread_name);
	field_set_object(thread, vm_java_lang_Thread_group, main_thread_group);
	field_set_object(thread, vm_java_lang_Thread_vmThread, vmthread);

	field_set_object(thread,
		vm_java_lang_Thread_contextClassLoader, NULL);
	field_set_int(thread,
		vm_java_lang_Thread_contextClassLoaderIsSystemClassLoader, 1);

	field_set_object(vmthread,
		vm_java_lang_VMThread_thread, thread);

	vm_call_method(vm_java_lang_ThreadGroup_addThread, main_thread_group,
		       thread);
	if (exception_occurred())
		return -1;

	vm_thread_attach_thread(main_thread);

	return 0;
}

/**
 * This is the entry point for all java threads.
 */
static void *vm_thread_entry(void *arg)
{
	struct vm_thread *thread = arg;

	vm_get_exec_env()->thread = thread;

	setup_signal_handlers();
	thread_init_exceptions();

	vm_call_method(vm_java_lang_VMThread_run, thread->vmthread);

	if (exception_occurred())
		vm_print_exception(exception_occurred());

	pthread_mutex_lock(&threads_mutex);
	while (thread_count_locked)
		pthread_cond_wait(&thread_count_lock_cond, &threads_mutex);

	vm_thread_detach_thread(vm_thread_self());
	pthread_mutex_unlock(&threads_mutex);

	return NULL;
}

/**
 * Creates new native thread representing a java thread.
 */
int vm_thread_start(struct vm_object *vmthread)
{
	struct vm_thread *thread = vm_thread_alloc();
	if (!thread) {
		NOT_IMPLEMENTED;
		return -1;
	}

	/* XXX: no need to lock because @thread is not yet visible to
	 * other threads. */
	thread->vmthread = vmthread;
	thread->state = VM_THREAD_STATE_RUNNABLE;

	field_set_object(vmthread, vm_java_lang_VMThread_vmdata,
			 (struct vm_object *) thread);

	if (!vm_thread_is_daemon(thread)) {
		pthread_mutex_lock(&threads_mutex);
		nr_non_daemons++;
		pthread_mutex_unlock(&threads_mutex);
	}

	pthread_mutex_lock(&threads_mutex);
	while (thread_count_locked)
		pthread_cond_wait(&thread_count_lock_cond, &threads_mutex);

	vm_thread_attach_thread(thread);

	if (pthread_create(&thread->posix_id, NULL, &vm_thread_entry, thread)) {
		vm_thread_detach_thread(thread);
		pthread_mutex_unlock(&threads_mutex);
		return -1;
	}

	pthread_mutex_unlock(&threads_mutex);
	return 0;
}

void vm_thread_wait_for_non_daemons(void)
{
	pthread_mutex_lock(&threads_mutex);

	while (nr_non_daemons)
		pthread_cond_wait(&thread_terminate_cond, &threads_mutex);

	pthread_mutex_unlock(&threads_mutex);
}

char *vm_thread_get_name(struct vm_thread *thread)
{
	struct vm_object *jthread;
	struct vm_object *name;

	jthread = vm_thread_get_java_thread(thread);
	name = field_get_object(jthread, vm_java_lang_Thread_name);

	return vm_string_to_cstr(name);
}

bool vm_thread_is_interrupted(struct vm_thread *thread)
{
	bool status;

	pthread_mutex_lock(&thread->mutex);
	status = thread->interrupted;
	pthread_mutex_unlock(&thread->mutex);
	return status;
}

bool vm_thread_interrupted(struct vm_thread *thread)
{
	bool status;

	pthread_mutex_lock(&thread->mutex);
	status = thread->interrupted;
	thread->interrupted = false;
	pthread_mutex_unlock(&thread->mutex);
	return status;
}

void vm_thread_interrupt(struct vm_thread *thread)
{
	struct vm_monitor *mon;

	pthread_mutex_lock(&thread->mutex);
	thread->interrupted = true;
	mon = thread->wait_mon;
	pthread_mutex_unlock(&thread->mutex);

	if (!mon)
		return;

	/*
	 * We should wait until thread actually enters a conditional
	 * wait so that the signal will not be lost. This can
	 * spuriously wake threads after @thread has left the wait()
	 * operation but it is fine with the JVM spec.
	 */
	while (pthread_mutex_trylock(&mon->mutex))
		sched_yield();
	pthread_cond_broadcast(&mon->cond);
	pthread_mutex_unlock(&mon->mutex);
}

void vm_lock_thread_count(void)
{
	pthread_mutex_lock(&threads_mutex);
	thread_count_locked = true;
	pthread_mutex_unlock(&threads_mutex);
}

void vm_unlock_thread_count(void)
{
	pthread_mutex_lock(&threads_mutex);
	thread_count_locked = false;
	pthread_cond_broadcast(&thread_count_lock_cond);
	pthread_mutex_unlock(&threads_mutex);
}
