#ifndef JIT_BYTECODE_TO_IR_H
#define JIT_BYTECODE_TO_IR_H

#include "jit/compiler.h"

/*
 * This macro magic sets up the converter lookup table.
 */

typedef int (*convert_fn_t) (struct parse_context *);

#define DECLARE_CONVERTER(name) int convert_##name(struct parse_context *)

DECLARE_CONVERTER(aaload);
DECLARE_CONVERTER(aastore);
DECLARE_CONVERTER(aconst_null);
DECLARE_CONVERTER(aload);
DECLARE_CONVERTER(aload_n);
DECLARE_CONVERTER(anewarray);
DECLARE_CONVERTER(arraylength);
DECLARE_CONVERTER(astore);
DECLARE_CONVERTER(astore_n);
DECLARE_CONVERTER(athrow);
DECLARE_CONVERTER(baload);
DECLARE_CONVERTER(bastore);
DECLARE_CONVERTER(bipush);
DECLARE_CONVERTER(caload);
DECLARE_CONVERTER(castore);
DECLARE_CONVERTER(checkcast);
DECLARE_CONVERTER(d2f);
DECLARE_CONVERTER(d2i);
DECLARE_CONVERTER(d2l);
DECLARE_CONVERTER(dadd);
DECLARE_CONVERTER(daload);
DECLARE_CONVERTER(dastore);
DECLARE_CONVERTER(dconst_n);
DECLARE_CONVERTER(ddiv);
DECLARE_CONVERTER(dload);
DECLARE_CONVERTER(dload_n);
DECLARE_CONVERTER(dmul);
DECLARE_CONVERTER(dneg);
DECLARE_CONVERTER(drem);
DECLARE_CONVERTER(dstore);
DECLARE_CONVERTER(dstore_n);
DECLARE_CONVERTER(dsub);
DECLARE_CONVERTER(dup);
DECLARE_CONVERTER(dup2);
DECLARE_CONVERTER(dup2_x1);
DECLARE_CONVERTER(dup2_x2);
DECLARE_CONVERTER(dup_x1);
DECLARE_CONVERTER(dup_x2);
DECLARE_CONVERTER(f2d);
DECLARE_CONVERTER(f2i);
DECLARE_CONVERTER(f2l);
DECLARE_CONVERTER(fadd);
DECLARE_CONVERTER(faload);
DECLARE_CONVERTER(fastore);
DECLARE_CONVERTER(fconst_n);
DECLARE_CONVERTER(fdiv);
DECLARE_CONVERTER(fload);
DECLARE_CONVERTER(fload_n);
DECLARE_CONVERTER(fmul);
DECLARE_CONVERTER(fneg);
DECLARE_CONVERTER(frem);
DECLARE_CONVERTER(fstore);
DECLARE_CONVERTER(fstore_n);
DECLARE_CONVERTER(fsub);
DECLARE_CONVERTER(getfield);
DECLARE_CONVERTER(getstatic);
DECLARE_CONVERTER(goto);
DECLARE_CONVERTER(goto_w);
DECLARE_CONVERTER(i2b);
DECLARE_CONVERTER(i2c);
DECLARE_CONVERTER(i2d);
DECLARE_CONVERTER(i2f);
DECLARE_CONVERTER(i2l);
DECLARE_CONVERTER(i2s);
DECLARE_CONVERTER(iadd);
DECLARE_CONVERTER(iaload);
DECLARE_CONVERTER(iand);
DECLARE_CONVERTER(iastore);
DECLARE_CONVERTER(iconst_n);
DECLARE_CONVERTER(idiv);
DECLARE_CONVERTER(if_acmpeq);
DECLARE_CONVERTER(if_acmpne);
DECLARE_CONVERTER(ifeq);
DECLARE_CONVERTER(ifge);
DECLARE_CONVERTER(ifgt);
DECLARE_CONVERTER(if_icmpeq);
DECLARE_CONVERTER(if_icmpge);
DECLARE_CONVERTER(if_icmpgt);
DECLARE_CONVERTER(if_icmple);
DECLARE_CONVERTER(if_icmplt);
DECLARE_CONVERTER(if_icmpne);
DECLARE_CONVERTER(ifle);
DECLARE_CONVERTER(iflt);
DECLARE_CONVERTER(ifne);
DECLARE_CONVERTER(ifnonnull);
DECLARE_CONVERTER(ifnull);
DECLARE_CONVERTER(iinc);
DECLARE_CONVERTER(iload);
DECLARE_CONVERTER(iload_n);
DECLARE_CONVERTER(imul);
DECLARE_CONVERTER(ineg);
DECLARE_CONVERTER(instanceof);
DECLARE_CONVERTER(invokeinterface);
DECLARE_CONVERTER(invokespecial);
DECLARE_CONVERTER(invokestatic);
DECLARE_CONVERTER(invokevirtual);
DECLARE_CONVERTER(ior);
DECLARE_CONVERTER(irem);
DECLARE_CONVERTER(ishl);
DECLARE_CONVERTER(ishr);
DECLARE_CONVERTER(istore);
DECLARE_CONVERTER(istore_n);
DECLARE_CONVERTER(isub);
DECLARE_CONVERTER(iushr);
DECLARE_CONVERTER(ixor);
DECLARE_CONVERTER(jsr);
DECLARE_CONVERTER(jsr_w);
DECLARE_CONVERTER(l2d);
DECLARE_CONVERTER(l2f);
DECLARE_CONVERTER(l2i);
DECLARE_CONVERTER(ladd);
DECLARE_CONVERTER(laload);
DECLARE_CONVERTER(land);
DECLARE_CONVERTER(lastore);
DECLARE_CONVERTER(lcmp);
DECLARE_CONVERTER(lconst_n);
DECLARE_CONVERTER(ldc);
DECLARE_CONVERTER(ldc2_w);
DECLARE_CONVERTER(ldc_w);
DECLARE_CONVERTER(ldiv);
DECLARE_CONVERTER(lload);
DECLARE_CONVERTER(lload_n);
DECLARE_CONVERTER(lmul);
DECLARE_CONVERTER(lneg);
DECLARE_CONVERTER(lookupswitch);
DECLARE_CONVERTER(lor);
DECLARE_CONVERTER(lrem);
DECLARE_CONVERTER(lshl);
DECLARE_CONVERTER(lshr);
DECLARE_CONVERTER(lstore);
DECLARE_CONVERTER(lstore_n);
DECLARE_CONVERTER(lsub);
DECLARE_CONVERTER(lushr);
DECLARE_CONVERTER(lxor);
DECLARE_CONVERTER(monitorenter);
DECLARE_CONVERTER(monitorexit);
DECLARE_CONVERTER(multianewarray);
DECLARE_CONVERTER(new);
DECLARE_CONVERTER(newarray);
DECLARE_CONVERTER(nop);
DECLARE_CONVERTER(pop);
DECLARE_CONVERTER(putfield);
DECLARE_CONVERTER(putstatic);
DECLARE_CONVERTER(ret);
DECLARE_CONVERTER(return);
DECLARE_CONVERTER(saload);
DECLARE_CONVERTER(sastore);
DECLARE_CONVERTER(sipush);
DECLARE_CONVERTER(swap);
DECLARE_CONVERTER(tableswitch);
DECLARE_CONVERTER(wide);
DECLARE_CONVERTER(xcmpg);
DECLARE_CONVERTER(xcmpl);
DECLARE_CONVERTER(xreturn);

int convert_instruction(struct parse_context *ctx);

#endif /* JIT_BYTECODE_TO_IR_H */
