#ifndef X86_ATOMIC_32_H
#define X86_ATOMIC_32_H

#include <stdint.h>

extern uint64_t atomic_cmpxchg_64(uint64_t *p, uint64_t old, uint64_t new);

static inline void *atomic_cmpxchg_ptr(void *p, void *old, void *new)
{
	return (void *) atomic_cmpxchg_32((uint32_t *) p, (uint32_t) old, (uint32_t) new);
}

#endif /* X86_ATOMIC_32_H */
