#ifndef _COMPAT_MACH_INIT_H_
#define _COMPAT_MACH_INIT_H_
#include <stdint.h>
typedef uintptr_t vm_offset_t;
typedef uintptr_t vm_size_t;
typedef uintptr_t vm_address_t;
typedef int boolean_t;
typedef int kern_return_t;
#define KERN_SUCCESS 0
#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif
#define mach_vm_round_page(x) (((uint64_t)(x) + 0x3fffULL) & ~0x3fffULL)
#endif
