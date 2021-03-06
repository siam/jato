#ifndef __JATO_VM_REFLECTION_H
#define __JATO_VM_REFLECTION_H

#include "vm/jni.h"

struct vm_object;

struct vm_class *vm_object_to_vm_class(struct vm_object *object);
struct vm_field *vm_object_to_vm_field(struct vm_object *field);

struct vm_object *
native_vmclass_get_declared_fields(struct vm_object *class_object,
				   jboolean public_only);
struct vm_object *
native_vmclass_get_declared_methods(struct vm_object *class_object,
				    jboolean public_only);
struct vm_object *
native_vmclass_get_declared_constructors(struct vm_object *class_object,
					 jboolean public_only);
struct vm_object *
native_constructor_get_parameter_types(struct vm_object *ctor);
struct vm_object *
native_method_get_parameter_types(struct vm_object *ctor);
jint native_constructor_get_modifiers_internal(struct vm_object *ctor);
struct vm_object *
native_constructor_construct_native(struct vm_object *this,
				    struct vm_object *args,
				    struct vm_object *declaring_class,
				    int slot);
struct vm_object *native_vmclass_get_interfaces(struct vm_object *clazz);
struct vm_object *native_vmclass_get_superclass(struct vm_object *clazz);
struct vm_object *native_field_get(struct vm_object *this, struct vm_object *o);
jlong native_field_get_long(struct vm_object *this, struct vm_object *o);
jint native_field_get_modifiers_internal(struct vm_object *this);
struct vm_object *native_field_gettype(struct vm_object *this);

jint native_method_get_modifiers_internal(struct vm_object *this);
struct vm_object *
native_method_invokenative(struct vm_object *method, struct vm_object *o,
			   struct vm_object *args,
			   struct vm_object *declaringClass,
			   jint slot);
void native_field_set(struct vm_object *this, struct vm_object *o, struct vm_object *value_obj);
struct vm_object *native_method_getreturntype(struct vm_object *method);

#endif /* __JATO_VM_REFLECTION_H */
