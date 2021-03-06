#ifndef __VM_OBJECT_H
#define __VM_OBJECT_H

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#include "vm/jni.h"
#include "vm/field.h"
#include "vm/thread.h"
#include "vm/vm.h"

struct vm_class;
enum vm_type;

struct vm_monitor {
	pthread_mutex_t mutex;
	pthread_cond_t cond;

	/* Holds the owner of this monitor or NULL when not locked. */
	struct vm_thread *owner;

	/*
	 * owner field might be read by other threads even if lock on
	 * @mutex is not held by them. For example vm_monitor_unlock()
	 * checks if the current thread is the owner of monitor. In
	 * SMP systems we must either ensure atomicity and use memory
	 * barriers or use a mutex. The latter approach is choosed.
	 */
	pthread_mutex_t owner_mutex;

	/*
	 * Holds the number of recursive locks on this monitor. This
	 * field is protected by @mutex.
	 */
	int lock_count;
};

struct vm_object {
	/* For arrays, this points to the array type, e.g. for int arrays,
	 * this points to the (artificial) class named "[I". We actually rely
	 * on this being the first field in the struct, because this way we
	 * don't need a null-pointer check for accessing this object whenever
	 * we access ->class first. */
	struct vm_class *class;

	struct vm_monitor monitor;

	jsize array_length;
	uint8_t fields[];
};

/* XXX: BUILD_BUG_ON(offsetof(vm_object, class) != 0); */

int init_vm_objects(void);
int init_vm_monitors(void);

struct vm_object *vm_object_alloc(struct vm_class *class);
struct vm_object *vm_object_alloc_primitive_array(int type, int count);
struct vm_object *vm_object_alloc_multi_array(struct vm_class *class,
	int nr_dimensions, int *count);
struct vm_object *vm_object_alloc_array(struct vm_class *class, int count);

struct vm_object *vm_object_clone(struct vm_object *obj);

struct vm_object *
vm_object_alloc_string_from_utf8(const uint8_t bytes[], unsigned int length);
struct vm_object *vm_object_alloc_string_from_c(const char *bytes);
struct vm_object *new_exception(struct vm_class *vmc, const char *message);
bool vm_object_is_instance_of(const struct vm_object *obj, const struct vm_class *class);
void vm_object_check_null(struct vm_object *obj);
void vm_object_check_array(struct vm_object *obj, jsize index);
void vm_object_check_cast(struct vm_object *obj, struct vm_class *class);

void vm_object_lock(struct vm_object *obj);
void vm_object_unlock(struct vm_object *obj);

void array_store_check(struct vm_object *arrayref, struct vm_object *obj);
void array_store_check_vmtype(struct vm_object *arrayref, enum vm_type vm_type);
void array_size_check(int size);
void multiarray_size_check(int n, ...);
char *vm_string_to_cstr(const struct vm_object *string);

int vm_monitor_init(struct vm_monitor *mon);
int vm_monitor_lock(struct vm_monitor *mon);
int vm_monitor_unlock(struct vm_monitor *mon);
int vm_monitor_wait(struct vm_monitor *mon);
int vm_monitor_timed_wait(struct vm_monitor *mon, long long ms, int ns);
int vm_monitor_notify(struct vm_monitor *mon);
int vm_monitor_notify_all(struct vm_monitor *mon);
struct vm_thread *vm_monitor_get_owner(struct vm_monitor *mon);
void vm_monitor_set_owner(struct vm_monitor *mon, struct vm_thread *owner);

#define DECLARE_FIELD_SETTER(type)					\
static inline void							\
field_set_ ## type (struct vm_object *obj, const struct vm_field *field,\
		    j ## type value)					\
{									\
	*(j ## type *) &obj->fields[field->offset] = value;		\
}

#define DECLARE_FIELD_GETTER(type)					\
static inline j ## type							\
field_get_ ## type (const struct vm_object *obj, const struct vm_field *field)\
{									\
	return *(j ## type *) &obj->fields[field->offset];		\
}

/*
 * We can not use generic setters for types of size less than machine
 * word. We currently load/store whole machine words therefore we must
 * set higher bits too, with sign extension for signed types.
 *
 * This should be fixed when register allocator finally supports
 * register constraints so that 8-bit and 16-bit load and stores can
 * be implemented in instruction selector.
 */

static inline void
field_set_byte(struct vm_object *obj, const struct vm_field *field, jbyte value)
{
	*(long *) &obj->fields[field->offset] = value;
}

static inline void
field_set_short(struct vm_object *obj, const struct vm_field *field,
		jshort value)
{
	*(long *) &obj->fields[field->offset] = value;
}

static inline void
field_set_boolean(struct vm_object *obj, const struct vm_field *field,
		  jboolean value)
{
	*(unsigned long *) &obj->fields[field->offset] = value;
}

static inline void
field_set_char(struct vm_object *obj, const struct vm_field *field, jchar value)
{
	*(unsigned long *) &obj->fields[field->offset] = value;
}

DECLARE_FIELD_SETTER(double);
DECLARE_FIELD_SETTER(float);
DECLARE_FIELD_SETTER(int);
DECLARE_FIELD_SETTER(long);
DECLARE_FIELD_SETTER(object);

DECLARE_FIELD_GETTER(byte);
DECLARE_FIELD_GETTER(boolean);
DECLARE_FIELD_GETTER(char);
DECLARE_FIELD_GETTER(double);
DECLARE_FIELD_GETTER(float);
DECLARE_FIELD_GETTER(int);
DECLARE_FIELD_GETTER(long);
DECLARE_FIELD_GETTER(object);
DECLARE_FIELD_GETTER(short);

#define DECLARE_ARRAY_FIELD_SETTER(type, vmtype)			\
static inline void							\
array_set_field_ ## type(struct vm_object *obj, int index,		\
			 j ## type value)				\
{									\
	*(j ## type *) &obj->fields[index * get_vmtype_size(vmtype)] = value; \
}

#define DECLARE_ARRAY_FIELD_GETTER(type, vmtype)			\
static inline j ## type							\
array_get_field_ ## type(const struct vm_object *obj, int index)	\
{									\
	return *(j ## type *) &obj->fields[index * get_vmtype_size(vmtype)]; \
}

static inline void
array_set_field_byte(struct vm_object *obj, int index, jbyte value)
{
	*(long *) &obj->fields[index * get_vmtype_size(J_BYTE)] = value;
}

static inline void
array_set_field_short(struct vm_object *obj, int index, jshort value)
{
	*(long *) &obj->fields[index * get_vmtype_size(J_SHORT)] = value;
}

static inline void
array_set_field_boolean(struct vm_object *obj, int index, jboolean value)
{
	*(unsigned long *) &obj->fields[index * get_vmtype_size(J_BOOLEAN)] = value;
}

static inline void
array_set_field_char(struct vm_object *obj, int index, jchar value)
{
	*(unsigned long *) &obj->fields[index * get_vmtype_size(J_CHAR)] = value;
}

DECLARE_ARRAY_FIELD_SETTER(double, J_DOUBLE);
DECLARE_ARRAY_FIELD_SETTER(float, J_FLOAT);
DECLARE_ARRAY_FIELD_SETTER(int, J_INT);
DECLARE_ARRAY_FIELD_SETTER(long, J_LONG);
DECLARE_ARRAY_FIELD_SETTER(object, J_REFERENCE);

DECLARE_ARRAY_FIELD_GETTER(byte, J_BYTE);
DECLARE_ARRAY_FIELD_GETTER(boolean, J_BOOLEAN);
DECLARE_ARRAY_FIELD_GETTER(char, J_CHAR);
DECLARE_ARRAY_FIELD_GETTER(double, J_DOUBLE);
DECLARE_ARRAY_FIELD_GETTER(float, J_FLOAT);
DECLARE_ARRAY_FIELD_GETTER(int, J_INT);
DECLARE_ARRAY_FIELD_GETTER(long, J_LONG);
DECLARE_ARRAY_FIELD_GETTER(object, J_REFERENCE);
DECLARE_ARRAY_FIELD_GETTER(short, J_SHORT);

static inline void
array_set_field_ptr(struct vm_object *obj, int index, void *value)
{
	*(void **) &obj->fields[index * get_vmtype_size(J_NATIVE_PTR)] = value;
}

static inline void *
array_get_field_ptr(struct vm_object *obj, int index)
{
	return *(void **) &obj->fields[index * get_vmtype_size(J_NATIVE_PTR)];
}

#endif
