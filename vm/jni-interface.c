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

#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>

#include "jit/exception.h"

#include "lib/guard-page.h"

#include "vm/call.h"
#include "vm/class.h"
#include "vm/classloader.h"
#include "vm/die.h"
#include "vm/jni.h"
#include "vm/method.h"
#include "vm/object.h"
#include "vm/preload.h"
#include "vm/stack-trace.h"

#define check_null(x)							\
	if ((x) == NULL) {						\
		signal_new_exception(vm_java_lang_NullPointerException, \
				     NULL);				\
		return NULL;						\
	}

#define check_class_object(x)						\
	if (!vm_object_is_instance_of((x), vm_java_lang_Class))		\
		return NULL;

static jfieldID
vm_jni_common_get_field_id(jclass clazz, const char *name, const char *sig)
{
	struct vm_class *class;
	struct vm_field *fb;

	check_null(clazz);
	check_class_object(clazz);

	class = vm_class_get_class_from_class_object(clazz);
	check_null(class);

	vm_class_ensure_init(class);
	if (exception_occurred())
		return NULL;

	fb = vm_class_get_field(class, name, sig);
	if (!fb) {
		signal_new_exception(vm_java_lang_NoSuchFieldError, NULL);
		return NULL;
	}

	return fb;
}

static void vm_jni_destroy_java_vm(void)
{
	NOT_IMPLEMENTED;
}

static void vm_jni_attach_current_thread(void)
{
	NOT_IMPLEMENTED;
}

static void vm_jni_detach_current_thread(void)
{
	NOT_IMPLEMENTED;
}

static jint vm_jni_get_env(JavaVM *vm, void **env, jint version)
{
	enter_vm_from_jni();

	if (version > JNI_VERSION_1_4) {
		*env = NULL;
		return JNI_EVERSION;
	}

	*env = vm_jni_get_jni_env();
	return JNI_OK;
}

static void vm_jni_attach_current_thread_as_daemon(void)
{
	NOT_IMPLEMENTED;
}

static void *vm_jni_invoke_interface[] = {
	NULL,
	NULL,
	NULL,

	vm_jni_destroy_java_vm,
	vm_jni_attach_current_thread,
	vm_jni_detach_current_thread,

	/* JNI_VERSION_1_2 */
	vm_jni_get_env,
	vm_jni_attach_current_thread_as_daemon,
};

static struct java_vm vm_jni_default_java_vm = {
	.jni_invoke_interface_table = vm_jni_invoke_interface,
};

struct java_vm *vm_jni_get_current_java_vm(void)
{
	return &vm_jni_default_java_vm;
}

static jclass
vm_jni_find_class(struct vm_jni_env *env, const char *name)
{
	struct vm_class *class;

	enter_vm_from_jni();

	/* XXX: which classloader should we use here? */
	class = classloader_load(NULL, name);
	if (!class) {
		signal_new_exception(vm_java_lang_NoClassDefFoundError,
				     name);
		return NULL;
	}

	vm_class_ensure_init(class);
	if (exception_occurred())
		return NULL;

	return class->object;
}

static jmethodID
vm_jni_get_method_id(struct vm_jni_env *env, jclass clazz, const char *name,
		     const char *sig)
{
	struct vm_method *mb;
	struct vm_class *class;

	enter_vm_from_jni();

	check_null(clazz);
	check_class_object(clazz);

	class = vm_class_get_class_from_class_object(clazz);
	check_null(class);

	vm_class_ensure_init(class);
	if (exception_occurred())
		return NULL;

	/* XXX: Make sure it's not static. */
	mb = vm_class_get_method_recursive(class, name, sig);
	if (!mb) {
		signal_new_exception(vm_java_lang_NoSuchMethodError, NULL);
		return NULL;
	}

	return mb;
}

static jfieldID
vm_jni_get_field_id(struct vm_jni_env *env, jclass clazz, const char *name,
		    const char *sig)
{
	struct vm_field *fb;

	enter_vm_from_jni();

	fb = vm_jni_common_get_field_id(clazz, name, sig);
	if (!fb)
		return NULL;

	if (vm_field_is_static(fb))
		return NULL;

	return fb;
}

static jmethodID
vm_jni_get_static_method_id(struct vm_jni_env *env, jclass clazz,
			    const char *name, const char *sig)
{
	struct vm_method *mb;
	struct vm_class *class;

	enter_vm_from_jni();

	check_null(clazz);

	class = vm_class_get_class_from_class_object(clazz);
	check_null(class);

	vm_class_ensure_init(class);
	if (exception_occurred())
		return NULL;

	/* XXX: Make sure it's actually static. */
	mb = vm_class_get_method_recursive(class, name, sig);
	if (!mb) {
		signal_new_exception(vm_java_lang_NoSuchMethodError, NULL);
		return NULL;
	}

	return mb;
}

static jobject vm_jni_new_string_utf(struct vm_jni_env *env, const char *bytes)
{
	enter_vm_from_jni();

	return vm_object_alloc_string_from_c(bytes);
}

static const jbyte*
vm_jni_get_string_utf_chars(struct vm_jni_env *env, jobject string,
			    jboolean *is_copy)
{
	jbyte *array;

	enter_vm_from_jni();

	if (!string)
		return NULL;

	array = (jbyte *) vm_string_to_cstr(string);
	if (!array)
		return NULL;

	if (is_copy)
		*is_copy = true;

	return array;
}

static void
vm_release_string_utf_chars(struct vm_jni_env *env, jobject string,
			    const char *utf)
{
	enter_vm_from_jni();

	free((char *)utf);
}

static jint
vm_jni_throw(struct vm_jni_env *env, jthrowable exception)
{
	enter_vm_from_jni();

	if (!vm_object_is_instance_of(exception, vm_java_lang_Throwable))
		return -1;

	signal_exception(exception);
	return 0;
}

static jint
vm_jni_throw_new(struct vm_jni_env *env, jclass clazz, const char *message)
{
	struct vm_class *class;

	enter_vm_from_jni();

	if (!clazz)
		return -1;

	if (!vm_object_is_instance_of(clazz, vm_java_lang_Class))
		return -1;

	class = vm_class_get_class_from_class_object(clazz);

	signal_new_exception(class, message);

	return 0;
}

static jthrowable vm_jni_exception_occurred(struct vm_jni_env *env)
{
	enter_vm_from_jni();

	return exception_occurred();
}

static void vm_jni_exception_describe(struct vm_jni_env *env)
{
	enter_vm_from_jni();

	if (exception_occurred())
		vm_print_exception(exception_occurred());
}

static void vm_jni_exception_clear(struct vm_jni_env *env)
{
	enter_vm_from_jni();

	clear_exception();
}

static void
vm_jni_fatal_error(struct vm_jni_env *env, const char *msg)
{
	enter_vm_from_jni();

	die("%s", msg);
}

static void
vm_jni_call_static_void_method(struct vm_jni_env *env, jclass clazz,
			       jmethodID methodID, ...)
{
	va_list args;

	enter_vm_from_jni();

	va_start(args, methodID);
	vm_call_method_v(methodID, args, NULL);
	va_end(args);
}

static void
vm_jni_call_static_void_method_v(struct vm_jni_env *env, jclass clazz,
				 jmethodID methodID, va_list args)
{
	enter_vm_from_jni();
	vm_call_method_v(methodID, args, NULL);
}

#define DECLARE_CALL_STATIC_XXX_METHOD(name, symbol)			\
	static j ## name vm_jni_call_static_ ## name ## _method		\
	(struct vm_jni_env *env, jclass clazz, jmethodID methodID, ...) \
	{								\
		union jvalue result;					\
		va_list args;						\
									\
		enter_vm_from_jni();					\
									\
		va_start(args, methodID);				\
		vm_call_method_v(methodID, args, &result);		\
		va_end(args);						\
									\
		return result.symbol;					\
	}

DECLARE_CALL_STATIC_XXX_METHOD(boolean, z);
DECLARE_CALL_STATIC_XXX_METHOD(byte, b);
DECLARE_CALL_STATIC_XXX_METHOD(char, c);
DECLARE_CALL_STATIC_XXX_METHOD(short, s);
DECLARE_CALL_STATIC_XXX_METHOD(int, i);
DECLARE_CALL_STATIC_XXX_METHOD(long, j);
DECLARE_CALL_STATIC_XXX_METHOD(float, f);
DECLARE_CALL_STATIC_XXX_METHOD(double, d);
DECLARE_CALL_STATIC_XXX_METHOD(object, l);

#define DECLARE_CALL_STATIC_XXX_METHOD_V(name, symbol)			\
	static j ## name vm_jni_call_static_ ## name ## _method_v	\
	(struct vm_jni_env *env, jclass clazz,				\
	 jmethodID methodID, va_list args)				\
	{								\
		union jvalue result;					\
									\
		enter_vm_from_jni();					\
									\
		vm_call_method_v(methodID, args, &result);		\
		return result.symbol;					\
	}

DECLARE_CALL_STATIC_XXX_METHOD_V(boolean, z);
DECLARE_CALL_STATIC_XXX_METHOD_V(byte, b);
DECLARE_CALL_STATIC_XXX_METHOD_V(char, c);
DECLARE_CALL_STATIC_XXX_METHOD_V(short, s);
DECLARE_CALL_STATIC_XXX_METHOD_V(int, i);
DECLARE_CALL_STATIC_XXX_METHOD_V(long, j);
DECLARE_CALL_STATIC_XXX_METHOD_V(float, f);
DECLARE_CALL_STATIC_XXX_METHOD_V(double, d);
DECLARE_CALL_STATIC_XXX_METHOD_V(object, l);

#define DECLARE_SET_XXX_FIELD(type, vmtype)				\
static void								\
vm_jni_set_ ## type ## _field(struct vm_jni_env *env, jobject object,	\
			      jfieldID field, j ## type value)		\
	{								\
	enter_vm_from_jni();						\
									\
	if (!object) {							\
	signal_new_exception(vm_java_lang_NullPointerException, NULL);	\
	return;								\
	}								\
									\
	if (vm_field_type(field) != vmtype)				\
		return;							\
									\
	field_set_ ## type (object, field, value);			\
}

DECLARE_SET_XXX_FIELD(boolean, J_BOOLEAN);
DECLARE_SET_XXX_FIELD(byte, J_BYTE);
DECLARE_SET_XXX_FIELD(char, J_CHAR);
DECLARE_SET_XXX_FIELD(double, J_DOUBLE);
DECLARE_SET_XXX_FIELD(float, J_FLOAT);
DECLARE_SET_XXX_FIELD(int, J_INT);
DECLARE_SET_XXX_FIELD(long, J_LONG);
DECLARE_SET_XXX_FIELD(object, J_REFERENCE);
DECLARE_SET_XXX_FIELD(short, J_SHORT);

static jobject vm_jni_new_global_ref(struct vm_jni_env *env, jobject obj)
{
	enter_vm_from_jni();

	/* TODO: fix this when GC is implemented. */

	return obj;
}

static void vm_jni_delete_global_ref(struct vm_jni_env *env, jobject obj)
{
	enter_vm_from_jni();

	/* TODO: fix this when GC is implemented. */
}

static jint vm_jni_get_java_vm(struct vm_jni_env *env, struct java_vm **vm)
{
	enter_vm_from_jni();

	*vm = vm_jni_get_current_java_vm();
	return 0;
}

static jobject vm_jni_get_object_class(struct vm_jni_env *env, jobject obj)
{
	enter_vm_from_jni();

	return obj->class->object;
}

static jboolean
vm_jni_is_assignable_from(struct vm_jni_env *env, jobject clazz1,
			  jobject clazz2)
{
	struct vm_class *class1, *class2;

	enter_vm_from_jni();

	if (!vm_object_is_instance_of(clazz1, vm_java_lang_Class) ||
	    !vm_object_is_instance_of(clazz2, vm_java_lang_Class))
		return JNI_FALSE;

	class1 = vm_class_get_class_from_class_object(clazz1);
	class2 = vm_class_get_class_from_class_object(clazz2);

	return vm_class_is_assignable_from(class2, class1);
}

static void vm_jni_delete_local_ref(struct vm_jni_env *env, jobject ref)
{
	enter_vm_from_jni();

	/* TODO: fix this when GC is implemented. */
}

static jint vm_jni_monitor_enter(struct vm_jni_env *env, jobject obj)
{
	enter_vm_from_jni();

	int err = vm_monitor_lock(&obj->monitor);

	if (exception_occurred())
		clear_exception();

	return err;
}

static jint vm_jni_monitor_exit(struct vm_jni_env *env, jobject obj)
{
	enter_vm_from_jni();

	int err = vm_monitor_unlock(&obj->monitor);

	if (exception_occurred())
		clear_exception();

	return err;
}

static jobject vm_jni_new_object(struct vm_jni_env *env, jobject clazz,
				 jmethodID method, ...)
{
	va_list args;

	enter_vm_from_jni();

	struct vm_object *obj;
	struct vm_class *class;

	if (!vm_object_is_instance_of(clazz, vm_java_lang_Class))
		return NULL;

	class = vm_class_get_class_from_class_object(clazz);
	obj = vm_object_alloc(class);

	va_start(args, method);
	vm_call_method_this_v(method, obj, args, NULL);
	va_end(args);

	return obj;
}

#define DECLARE_GET_XXX_FIELD(type, vmtype)				\
static j ## type							\
vm_jni_get_ ## type ## _field(struct vm_jni_env *env, jobject object,	\
			      jfieldID field)				\
	{								\
	enter_vm_from_jni();						\
									\
	if (!object) {							\
	signal_new_exception(vm_java_lang_NullPointerException, NULL);	\
	return 0;							\
	}								\
									\
	if (vm_field_type(field) != vmtype) {				\
	NOT_IMPLEMENTED;						\
	return 0;							\
	}								\
									\
	return field_get_ ## type (object, field);			\
}

DECLARE_GET_XXX_FIELD(boolean, J_BOOLEAN);
DECLARE_GET_XXX_FIELD(byte, J_BYTE);
DECLARE_GET_XXX_FIELD(char, J_CHAR);
DECLARE_GET_XXX_FIELD(double, J_DOUBLE);
DECLARE_GET_XXX_FIELD(float, J_FLOAT);
DECLARE_GET_XXX_FIELD(int, J_INT);
DECLARE_GET_XXX_FIELD(long, J_LONG);
DECLARE_GET_XXX_FIELD(object, J_REFERENCE);
DECLARE_GET_XXX_FIELD(short, J_SHORT);

#define DECLARE_GET_XXX_ARRAY_ELEMENTS(type)				\
static j ## type *							\
vm_jni_get_ ## type ## _array_elements(struct vm_jni_env *env,		\
				       jobject array,			\
				       jboolean *is_copy)		\
{									\
	j ## type *result;						\
									\
	enter_vm_from_jni();						\
									\
	if (!vm_class_is_array_class(array->class) ||			\
	    vm_class_get_array_element_class(array->class)		\
	    != vm_ ## type ## _class)					\
		return NULL;						\
									\
	result = malloc(sizeof(j ## type) * array->array_length);	\
	if (!result)							\
		return NULL;						\
									\
	for (long i = 0; i < array->array_length; i++)			\
		result[i] = array_get_field_##type(array, i);		\
									\
	if (is_copy)							\
		*is_copy = JNI_TRUE;					\
									\
	return result;							\
}

DECLARE_GET_XXX_ARRAY_ELEMENTS(byte);
DECLARE_GET_XXX_ARRAY_ELEMENTS(char);
DECLARE_GET_XXX_ARRAY_ELEMENTS(double);
DECLARE_GET_XXX_ARRAY_ELEMENTS(float);
DECLARE_GET_XXX_ARRAY_ELEMENTS(int);
DECLARE_GET_XXX_ARRAY_ELEMENTS(long);
DECLARE_GET_XXX_ARRAY_ELEMENTS(short);
DECLARE_GET_XXX_ARRAY_ELEMENTS(boolean);

#define DECLARE_RELEASE_XXX_ARRAY_ELEMENTS(type)			\
static void								\
vm_jni_release_ ## type ## _array_elements(struct vm_jni_env *env,	\
					   jobject array,		\
					   j ## type *elems, jint mode)	\
{									\
	enter_vm_from_jni();						\
									\
	if (!vm_class_is_array_class(array->class) ||			\
	    vm_class_get_array_element_class(array->class)		\
	    != vm_ ## type ## _class)					\
		return;							\
									\
	if (mode == 0 || mode == JNI_COMMIT) { /* copy back */		\
		for (long i = 0; i < array->array_length; i++)		\
			array_set_field_ ## type(array, i, elems[i]); \
	}								\
									\
	if (mode == 0 || mode == JNI_ABORT) /* free buffer */		\
		free(elems);						\
}

DECLARE_RELEASE_XXX_ARRAY_ELEMENTS(byte);
DECLARE_RELEASE_XXX_ARRAY_ELEMENTS(char);
DECLARE_RELEASE_XXX_ARRAY_ELEMENTS(double);
DECLARE_RELEASE_XXX_ARRAY_ELEMENTS(float);
DECLARE_RELEASE_XXX_ARRAY_ELEMENTS(int);
DECLARE_RELEASE_XXX_ARRAY_ELEMENTS(long);
DECLARE_RELEASE_XXX_ARRAY_ELEMENTS(short);
DECLARE_RELEASE_XXX_ARRAY_ELEMENTS(boolean);

static void *
vm_jni_get_direct_buffer_address(struct vm_jni_env *env, jobject buf)
{
	enter_vm_from_jni();

	/* We can't return direct buffer because we use machine word size
	   elements for arrays. */
	return NULL;
}

/*
 * TODO: When these functions are used to call private methods and
 * constructors, the method ID must be derived from the real class of
 * obj, not from one of its superclasses.
 */
static void vm_jni_call_void_method(struct vm_jni_env *env, jobject this,
				    jmethodID methodID, ...)
{
	va_list args;

	enter_vm_from_jni();

	va_start(args, methodID);
	vm_call_method_this_v(methodID, this, args, NULL);
	va_end(args);
}

/*
 * TODO: When these functions are used to call private methods and
 * constructors, the method ID must be derived from the real class of
 * obj, not from one of its superclasses.
 */
#define DECLARE_CALL_XXX_METHOD(type, symbol)				\
	static j ## type vm_jni_call_ ## type ## _method		\
	(struct vm_jni_env *env, jobject this, jmethodID methodID, ...)	\
	{								\
		va_list args;						\
		union jvalue result;					\
									\
		enter_vm_from_jni();					\
									\
		va_start(args, methodID);				\
		vm_call_method_this_v(methodID, this, args, &result);	\
		va_end(args);						\
									\
		return result.symbol;					\
	}

DECLARE_CALL_XXX_METHOD(boolean, z);
DECLARE_CALL_XXX_METHOD(byte, b);
DECLARE_CALL_XXX_METHOD(char, c);
DECLARE_CALL_XXX_METHOD(short, s);
DECLARE_CALL_XXX_METHOD(int, i);
DECLARE_CALL_XXX_METHOD(long, j);
DECLARE_CALL_XXX_METHOD(float, f);
DECLARE_CALL_XXX_METHOD(double, d);
DECLARE_CALL_XXX_METHOD(object, l);

static jfieldID
vm_jni_get_static_field_id(struct vm_jni_env *env, jclass clazz,
			   const char *name, const char *sig)
{
	struct vm_field *fb;

	enter_vm_from_jni();

	fb = vm_jni_common_get_field_id(clazz, name, sig);
	if (!fb)
		return NULL;

	if (!vm_field_is_static(fb))
		return NULL;

	return fb;
}

#define DEFINE_GET_STATIC_FIELD(func, type, get)			\
	static type							\
	func(struct vm_jni_env *env, jobject object, jfieldID field)	\
	{								\
		enter_vm_from_jni();					\
									\
		return get(field);					\
	}								\

DEFINE_GET_STATIC_FIELD(vm_jni_get_static_object_field, jobject, static_field_get_object);
DEFINE_GET_STATIC_FIELD(vm_jni_get_static_boolean_field, jboolean, static_field_get_boolean);
DEFINE_GET_STATIC_FIELD(vm_jni_get_static_byte_field, jbyte, static_field_get_byte);
DEFINE_GET_STATIC_FIELD(vm_jni_get_static_char_field, jchar, static_field_get_char);
DEFINE_GET_STATIC_FIELD(vm_jni_get_static_short_field, jshort, static_field_get_short);
DEFINE_GET_STATIC_FIELD(vm_jni_get_static_int_field, jint, static_field_get_int);
DEFINE_GET_STATIC_FIELD(vm_jni_get_static_long_field, jlong, static_field_get_long);
DEFINE_GET_STATIC_FIELD(vm_jni_get_static_float_field, jfloat, static_field_get_float);
DEFINE_GET_STATIC_FIELD(vm_jni_get_static_double_field, jdouble, static_field_get_double);

#define DEFINE_SET_STATIC_FIELD(func, type, set)			\
	static void							\
	func(struct vm_jni_env *env, jobject object,			\
		 jfieldID field, type value)				\
	{								\
		enter_vm_from_jni();					\
									\
		set(field, value);					\
	}								\

DEFINE_SET_STATIC_FIELD(vm_jni_set_static_object_field, jobject, static_field_set_object);
DEFINE_SET_STATIC_FIELD(vm_jni_set_static_boolean_field, jboolean, static_field_set_boolean);
DEFINE_SET_STATIC_FIELD(vm_jni_set_static_byte_field, jbyte, static_field_set_byte);
DEFINE_SET_STATIC_FIELD(vm_jni_set_static_char_field, jchar, static_field_set_char);
DEFINE_SET_STATIC_FIELD(vm_jni_set_static_short_field, jshort, static_field_set_short);
DEFINE_SET_STATIC_FIELD(vm_jni_set_static_int_field, jint, static_field_set_int);
DEFINE_SET_STATIC_FIELD(vm_jni_set_static_long_field, jlong, static_field_set_long);
DEFINE_SET_STATIC_FIELD(vm_jni_set_static_float_field, jfloat, static_field_set_float);
DEFINE_SET_STATIC_FIELD(vm_jni_set_static_double_field, jdouble, static_field_set_double);

static jobject
vm_jni_new_object_array(struct vm_jni_env *env, jsize size,
			jclass element_class, jobject initial_element)
{
	struct vm_object *array;
	struct vm_class *vmc;

	enter_vm_from_jni();

	check_null(element_class);

	vmc = vm_class_get_class_from_class_object(element_class);
	if (!vmc)
		return NULL;

	vmc = vm_class_get_array_class(vmc);
	if (!vmc)
		return NULL;

	array = vm_object_alloc_array(vmc, size);
	if (!array) {
		NOT_IMPLEMENTED;
		return NULL;
	}

	while (size)
		array_set_field_object(array, --size, initial_element);

	return array;
}

static jobject vm_jni_get_object_array_element(struct vm_jni_env *env,
					       jobjectArray array,
					       jsize index)
{
	enter_vm_from_jni();

	if (array == NULL) {
		signal_new_exception(vm_java_lang_NullPointerException, NULL);
		return NULL;
	}
	if (index >= array->array_length) {
		signal_new_exception(vm_java_lang_ArrayIndexOutOfBoundsException, NULL);
		return NULL;
	}

	return array_get_field_object(array, index);
}

static void vm_jni_set_object_array_element(struct vm_jni_env *env,
					    jobjectArray array,
					    jsize index,
					    jobject value)
{
	enter_vm_from_jni();

	if (array == NULL) {
		signal_new_exception(vm_java_lang_NullPointerException, NULL);
		return;
	}
	if (index >= array->array_length) {
		signal_new_exception(vm_java_lang_ArrayIndexOutOfBoundsException, NULL);
		return;
	}

	return array_set_field_object(array, index, value);
}

static jsize vm_jni_get_array_length(struct vm_jni_env *env, jarray array)
{
	enter_vm_from_jni();

	if (!vm_class_is_array_class(array->class)) {
		warn("argument is not an array");
		return 0;
	}

	return array->array_length;
}

#define DECLARE_NEW_XXX_ARRAY(type, arr_type)				\
static jobject								\
vm_jni_new_ ## type ## _array(struct vm_jni_env *env, jsize size)	\
{									\
	jobject result;							\
									\
	enter_vm_from_jni();						\
									\
	result = vm_object_alloc_primitive_array(arr_type, size);	\
	return result;							\
}

DECLARE_NEW_XXX_ARRAY(boolean, T_BOOLEAN);
DECLARE_NEW_XXX_ARRAY(byte, T_BYTE);
DECLARE_NEW_XXX_ARRAY(char, T_CHAR);
DECLARE_NEW_XXX_ARRAY(double, T_DOUBLE);
DECLARE_NEW_XXX_ARRAY(float, T_FLOAT);
DECLARE_NEW_XXX_ARRAY(long, T_LONG);
DECLARE_NEW_XXX_ARRAY(int, T_INT);
DECLARE_NEW_XXX_ARRAY(short, T_SHORT);

static inline void pack_args(struct vm_method *vmm, unsigned long *packed_args,
			     uint64_t *args)
{
#ifdef CONFIG_32_BIT
	struct vm_method_arg *arg;
	int packed_idx;
	int idx;

	packed_idx = 0;
	idx = 0;

	list_for_each_entry(arg, &vmm->args, list_node) {
		if (arg->type_info.vm_type == J_LONG ||
		    arg->type_info.vm_type == J_DOUBLE) {
			packed_args[packed_idx++] = low_64(args[idx]);
			packed_args[packed_idx++] = high_64(args[idx++]);
		} else {
			packed_args[packed_idx++] = low_64(args[idx++]);
		}
	}
#else
	int count;

	count = count_java_arguments(vmm);
	memcpy(packed_args, args, sizeof(uint64_t) * count);
#endif
}

static jobject
vm_jni_new_object_a(struct vm_jni_env *env, jclass clazz, jmethodID method,
		    uint64_t *args)
{
	struct vm_class *vmc;
	struct vm_object *result;

	enter_vm_from_jni();

	vmc = vm_class_get_class_from_class_object(clazz);
	result = vm_object_alloc(vmc);

	unsigned long packed_args[method->args_count];

	packed_args[0] = (unsigned long) result;
	pack_args(method, packed_args + 1, args);

	vm_call_method_this_a(method, result, packed_args, NULL);

	return result;
}

#define DECLARE_GET_XXX_ARRAY_REGION(type)				\
static void								\
 vm_jni_get_ ## type ## _array_region(struct vm_jni_env *env,		\
				      jobject array,			\
				      jsize start,			\
				      jsize len,			\
				      j ## type *buf)			\
{									\
	enter_vm_from_jni();						\
									\
	if (!vm_class_is_array_class(array->class) ||			\
	    vm_class_get_array_element_class(array->class)		\
	    != vm_ ## type ## _class)					\
		return;							\
									\
	if (start < 0 || len < 0 || start + len > array->array_length) { \
		signal_new_exception(vm_java_lang_ArrayIndexOutOfBoundsException, \
				     NULL);				\
		return;							\
	}								\
									\
	for (long i = 0; i < len; i++)					\
		buf[i] = array_get_field_##type(array, start + i);	\
}

DECLARE_GET_XXX_ARRAY_REGION(byte);
DECLARE_GET_XXX_ARRAY_REGION(char);
DECLARE_GET_XXX_ARRAY_REGION(double);
DECLARE_GET_XXX_ARRAY_REGION(float);
DECLARE_GET_XXX_ARRAY_REGION(int);
DECLARE_GET_XXX_ARRAY_REGION(long);
DECLARE_GET_XXX_ARRAY_REGION(short);
DECLARE_GET_XXX_ARRAY_REGION(boolean);

#define DECLARE_SET_XXX_ARRAY_REGION(type)				\
static void								\
 vm_jni_set_ ## type ## _array_region(struct vm_jni_env *env,		\
				      jobject array,			\
				      jsize start,			\
				      jsize len,			\
				      j ## type *buf)			\
{									\
	enter_vm_from_jni();						\
									\
	if (!vm_class_is_array_class(array->class) ||			\
	    vm_class_get_array_element_class(array->class)		\
	    != vm_ ## type ## _class)					\
		return;							\
									\
	if (start < 0 || len < 0 || start + len > array->array_length) { \
		signal_new_exception(vm_java_lang_ArrayIndexOutOfBoundsException, \
				     NULL);				\
		return;							\
	}								\
									\
	for (long i = 0; i < len; i++)					\
		array_set_field_##type(array, start + i, buf[i]);	\
}

DECLARE_SET_XXX_ARRAY_REGION(byte);
DECLARE_SET_XXX_ARRAY_REGION(char);
DECLARE_SET_XXX_ARRAY_REGION(double);
DECLARE_SET_XXX_ARRAY_REGION(float);
DECLARE_SET_XXX_ARRAY_REGION(int);
DECLARE_SET_XXX_ARRAY_REGION(long);
DECLARE_SET_XXX_ARRAY_REGION(short);
DECLARE_SET_XXX_ARRAY_REGION(boolean);

/*
 * The JNI native interface table.
 * See: http://java.sun.com/j2se/1.4.2/docs/guide/jni/spec/functions.html
 */
void *vm_jni_native_interface[] = {
	/* 0 */
	NULL,
	NULL,
	NULL,
	NULL,
	NULL, /* GetVersion */

	/* 5 */
	NULL, /* DefineClass */
	vm_jni_find_class,
	NULL,
	NULL,
	NULL,

	/* 10 */
	NULL, /* GetSuperclass */
	vm_jni_is_assignable_from,
	NULL,
	vm_jni_throw,
	vm_jni_throw_new,

	/* 15 */
	vm_jni_exception_occurred,
	vm_jni_exception_describe,
	vm_jni_exception_clear,
	vm_jni_fatal_error,
	NULL,

	/* 20 */
	NULL,
	vm_jni_new_global_ref,
	vm_jni_delete_global_ref,
	vm_jni_delete_local_ref,
	NULL, /* IsSameObject */

	/* 25 */
	NULL,
	NULL,
	NULL, /* AllocObject */
	vm_jni_new_object,
	NULL, /* NewObjectV */

	/* 30 */
	vm_jni_new_object_a,
	vm_jni_get_object_class,
	NULL, /* IsInstanceOf */
	vm_jni_get_method_id,
	vm_jni_call_object_method,

	/* 35 */
	NULL, /* CallObjectMethodV */
	NULL, /* CallObjectMethodA */
	vm_jni_call_boolean_method,
	NULL, /* CallBooleanMethodV */
	NULL, /* CallBooleanMethodA */

	/* 40 */
	vm_jni_call_byte_method,
	NULL, /* CallByteMethodV */
	NULL, /* CallByteMethodA */
	vm_jni_call_char_method,
	NULL, /* CallCharMethodV */

	/* 45 */
	NULL, /* CallCharMethodA */
	vm_jni_call_short_method,
	NULL, /* CallShortMethodV */
	NULL, /* CallShortMethodA */
	vm_jni_call_int_method,

	/* 50 */
	NULL, /* CallIntMethodV */
	NULL, /* CallIntMethodA */
	vm_jni_call_long_method,
	NULL, /* CallLongMethodV */
	NULL, /* CallLongMethodA */

	/* 55 */
	vm_jni_call_float_method,
	NULL, /* CallFloatMethodV */
	NULL, /* CallFloatMethodA */
	vm_jni_call_double_method,
	NULL, /* CallDoubleMethodV */

	/* 60 */
	NULL, /* CallDoubleMethodA */
	vm_jni_call_void_method,
	NULL, /* CallVoidMethodV */
	NULL, /* CallVoidMethodA */
	NULL, /* CallNonvirtualObjectMethod */

	/* 65 */
	NULL, /* CallNonvirtualObjectMethodV */
	NULL, /* CallNonvirtualObjectMethodA */
	NULL, /* CallNonvirtualBooleanMethod */
	NULL, /* CallNonvirtualBooleanMethodV */
	NULL, /* CallNonvirtualBooleanMethodA */

	/* 70 */
	NULL, /* CallNonvirtualByteMethod */
	NULL, /* CallNonvirtualByteMethodV */
	NULL, /* CallNonvirtualByteMethodA */
	NULL, /* CallNonvirtualCharMethod */
	NULL, /* CallNonvirtualCharMethodV */

	/* 75 */
	NULL, /* CallNonvirtualCharMethodA */
	NULL, /* CallNonvirtualShortMethod */
	NULL, /* CallNonvirtualShortMethodV */
	NULL, /* CallNonvirtualShortMethodA */
	NULL, /* CallNonvirtualIntMethod */

	/* 80 */
	NULL, /* CallNonvirtualIntMethodV */
	NULL, /* CallNonvirtualIntMethodA */
	NULL, /* CallNonvirtualLongMethod */
	NULL, /* CallNonvirtualLongMethodV */
	NULL, /* CallNonvirtualLongMethodA */

	/* 85 */
	NULL, /* CallNonvirtualFloatMethod */
	NULL, /* CallNonvirtualFloatMethodV */
	NULL, /* CallNonvirtualFloatMethodA */
	NULL, /* CallNonvirtualDoubleMethod */
	NULL, /* CallNonvirtualDoubleMethodV */

	/* 90 */
	NULL, /* CallNonvirtualDoubleMethodA */
	NULL, /* CallNonvirtualVoidMethod */
	NULL, /* CallNonvirtualVoidMethodV */
	NULL, /* CallNonvirtualVoidMethodA */
	vm_jni_get_field_id,

	/* 95 */
	vm_jni_get_object_field,
	vm_jni_get_boolean_field,
	vm_jni_get_byte_field,
	vm_jni_get_char_field,
	vm_jni_get_short_field,

	/* 100 */
	vm_jni_get_int_field,
	vm_jni_get_long_field,
	vm_jni_get_float_field,
	vm_jni_get_double_field,
	vm_jni_set_object_field,

	/* 105 */
	vm_jni_set_boolean_field,
	vm_jni_set_byte_field,
	vm_jni_set_char_field,
	vm_jni_set_short_field,
	vm_jni_set_int_field,

	/* 110 */
	vm_jni_set_long_field,
	vm_jni_set_float_field,
	vm_jni_set_double_field,
	vm_jni_get_static_method_id,
	vm_jni_call_static_object_method,

	/* 115 */
	vm_jni_call_static_object_method_v,
	NULL, /* CallStaticObjectMethodA */
	vm_jni_call_static_boolean_method,
	vm_jni_call_static_boolean_method_v,
	NULL, /* CallStaticBooleanMethodA */

	/* 120 */
	vm_jni_call_static_byte_method,
	vm_jni_call_static_byte_method_v,
	NULL, /* CallStaticByteMethodA */
	vm_jni_call_static_char_method,
	vm_jni_call_static_char_method_v,

	/* 125 */
	NULL, /* CallStaticCharMethodA */
	vm_jni_call_static_short_method,
	vm_jni_call_static_short_method_v,
	NULL, /* CallStaticShortMethodA */
	vm_jni_call_static_int_method,

	/* 130 */
	vm_jni_call_static_int_method_v,
	NULL, /* CallStaticIntMethodA */
	vm_jni_call_static_long_method,
	vm_jni_call_static_long_method_v,
	NULL, /* CallStaticLongMethodA */

	/* 135 */
	vm_jni_call_static_float_method,
	vm_jni_call_static_float_method_v,
	NULL, /* CallStaticFloatMethodA */
	vm_jni_call_static_double_method,
	vm_jni_call_static_double_method_v,

	/* 140 */
	NULL, /* CallStaticDoubleMethodA */
	vm_jni_call_static_void_method,
	vm_jni_call_static_void_method_v,
	NULL, /* CallStaticVoidMethodA */
	vm_jni_get_static_field_id,

	/* 145 */
	vm_jni_get_static_object_field,
	vm_jni_get_static_boolean_field,
	vm_jni_get_static_byte_field,
	vm_jni_get_static_char_field,
	vm_jni_get_static_short_field,

	/* 150 */
	vm_jni_get_static_int_field,
	vm_jni_get_static_long_field,
	vm_jni_get_static_float_field,
	vm_jni_get_static_double_field,
	vm_jni_set_static_object_field,

	/* 155 */
	vm_jni_set_static_boolean_field,
	vm_jni_set_static_byte_field,
	vm_jni_set_static_char_field,
	vm_jni_set_static_short_field,
	vm_jni_set_static_int_field,

	/* 160 */
	vm_jni_set_static_long_field,
	vm_jni_set_static_float_field,
	vm_jni_set_static_double_field,
	NULL, /* NewString */
	NULL, /* GetStringLength */

	/* 165 */
	NULL, /* GetStringChars */
	NULL, /* ReleaseStringChars */
	vm_jni_new_string_utf,
	NULL, /* GetStringUTFLength */
	vm_jni_get_string_utf_chars,

	/* 170 */
	vm_release_string_utf_chars,
	vm_jni_get_array_length,
	vm_jni_new_object_array,
	vm_jni_get_object_array_element,
	vm_jni_set_object_array_element,

	/* 175 */
	vm_jni_new_boolean_array,
	vm_jni_new_byte_array,
	vm_jni_new_char_array,
	vm_jni_new_short_array,
	vm_jni_new_int_array,

	/* 180 */
	vm_jni_new_long_array,
	vm_jni_new_float_array,
	vm_jni_new_double_array,
	vm_jni_get_boolean_array_elements,
	vm_jni_get_byte_array_elements,

	/* 185 */
	vm_jni_get_char_array_elements,
	vm_jni_get_short_array_elements,
	vm_jni_get_int_array_elements,
	vm_jni_get_long_array_elements,
	vm_jni_get_float_array_elements,

	/* 190 */
	vm_jni_get_double_array_elements,
	vm_jni_release_boolean_array_elements,
	vm_jni_release_byte_array_elements,
	vm_jni_release_char_array_elements,
	vm_jni_release_short_array_elements,

	/* 195 */
	vm_jni_release_int_array_elements,
	vm_jni_release_long_array_elements,
	vm_jni_release_float_array_elements,
	vm_jni_release_double_array_elements,
	vm_jni_get_boolean_array_region,

	/* 200 */
	vm_jni_get_byte_array_region,
	vm_jni_get_char_array_region,
	vm_jni_get_short_array_region,
	vm_jni_get_int_array_region,
	vm_jni_get_long_array_region,

	/* 205 */
	vm_jni_get_float_array_region,
	vm_jni_get_double_array_region,
	vm_jni_set_boolean_array_region,
	vm_jni_set_byte_array_region,
	vm_jni_set_char_array_region,

	/* 210 */
	vm_jni_set_short_array_region,
	vm_jni_set_int_array_region,
	vm_jni_set_long_array_region,
	vm_jni_set_float_array_region,
	vm_jni_set_double_array_region,

	/* 215 */
	NULL, /* RegisterNatives */
	NULL, /* UnregisterNatives */
	vm_jni_monitor_enter,
	vm_jni_monitor_exit,
	vm_jni_get_java_vm,

	/* JNI 1.2 functions */

	/* 220 */
	NULL, /* GetStringRegion */
	NULL, /* GetStringUTFRegion */
	NULL, /* GetPrimitiveArrayCritical */
	NULL, /* ReleasePrimitiveArrayCritical */
	NULL, /* GetStringCritical */

	/* 225 */
	NULL, /* ReleaseStringCritical */
	NULL, /* NewWeakGlobalRef */
	NULL, /* DeleteWeakGlobalRef */
	NULL, /* ExceptionCheck */

	/* JNI 1.4 functions */

	NULL, /* NewDirectByteBuffer */

	/* 230 */
	vm_jni_get_direct_buffer_address,
	NULL, /* GetDirectBufferCapacity */
	NULL,
	NULL,
	NULL,
};

struct vm_jni_env vm_jni_default_env = {
	.jni_table = vm_jni_native_interface,
};

struct vm_jni_env *vm_jni_get_jni_env(void)
{
	return &vm_jni_default_env;
}

static void *jni_not_implemented_trap;

void vm_jni_init_interface(void)
{
	jni_not_implemented_trap = alloc_guard_page(true);
	if (!jni_not_implemented_trap)
		die("guard page alloc failed");

	/* Initialize traps in the vm_jni_default_native_interface[] */
	unsigned int table_size = ARRAY_SIZE(vm_jni_native_interface);
	for (unsigned int i = 0; i < table_size; i++) {
		if (vm_jni_native_interface[i])
			continue;

		vm_jni_native_interface[i] =
			jni_not_implemented_trap + i;
	}
}

bool vm_jni_check_trap(void *ptr)
{
	int table_size;
	int index;

	index = (int)(ptr - jni_not_implemented_trap);
	table_size = ARRAY_SIZE(vm_jni_native_interface);

	if (index < 0 || index >= table_size)
		return false;

	warn("JNI handler for index %d not implemented.", index);
	return true;
}
