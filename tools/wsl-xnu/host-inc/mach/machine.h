/* Minimal <mach/machine.h> for XNU SETUP host tools on Linux. */
#ifndef _COMPAT_MACH_MACHINE_H_
#define _COMPAT_MACH_MACHINE_H_
#include <stdint.h>
typedef int integer_t;
typedef unsigned int natural_t;
typedef integer_t cpu_type_t;
typedef integer_t cpu_subtype_t;
typedef integer_t cpu_threadtype_t;
#define CPU_ARCH_ABI64 0x01000000
#define CPU_ARCH_ABI64_32 0x02000000
#define CPU_TYPE_ANY ((cpu_type_t)-1)
#define CPU_TYPE_X86 ((cpu_type_t)7)
#define CPU_TYPE_I386 CPU_TYPE_X86
#define CPU_TYPE_X86_64 (CPU_TYPE_X86 | CPU_ARCH_ABI64)
#define CPU_TYPE_ARM ((cpu_type_t)12)
#define CPU_TYPE_ARM64 (CPU_TYPE_ARM | CPU_ARCH_ABI64)
#define CPU_TYPE_ARM64_32 (CPU_TYPE_ARM | CPU_ARCH_ABI64_32)
#define CPU_SUBTYPE_MASK 0xff000000
#define CPU_SUBTYPE_LIB64 0x80000000
#define CPU_SUBTYPE_X86_64_ALL ((cpu_subtype_t)3)
#define CPU_SUBTYPE_X86_64_H ((cpu_subtype_t)8)
#define CPU_SUBTYPE_ARM_ALL ((cpu_subtype_t)0)
#define CPU_SUBTYPE_ARM_V7 ((cpu_subtype_t)9)
#define CPU_SUBTYPE_ARM_V7S ((cpu_subtype_t)11)
#define CPU_SUBTYPE_ARM_V7K ((cpu_subtype_t)12)
#define CPU_SUBTYPE_ARM64_ALL ((cpu_subtype_t)0)
#define CPU_SUBTYPE_ARM64_V8 ((cpu_subtype_t)1)
#define CPU_SUBTYPE_ARM64E ((cpu_subtype_t)2)
#define CPU_SUBTYPE_ARM64_E CPU_SUBTYPE_ARM64E
#endif
