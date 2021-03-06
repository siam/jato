#ifndef RUNTIME_UNSAFE_H
#define RUNTIME_UNSAFE_H

#include "vm/jni.h"

struct vm_object;

jint native_unsafe_compare_and_swap_int(struct vm_object *this,
					struct vm_object *obj, jlong offset,
					jint expect, jint update);
jint native_unsafe_compare_and_swap_long(struct vm_object *this,
					 struct vm_object *obj, jlong offset,
					 jlong expect, jlong update);
jint native_unsafe_compare_and_swap_object(struct vm_object *this,
					   struct vm_object *obj,
					   jlong offset,
					   struct vm_object *expect,
					   struct vm_object *update);
jlong native_unsafe_object_field_offset(struct vm_object *this,
					struct vm_object *field);

#endif /* RUNTIME_UNSAFE_H */
