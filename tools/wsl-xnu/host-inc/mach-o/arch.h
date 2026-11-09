#ifndef _COMPAT_MACH_O_ARCH_H_
#define _COMPAT_MACH_O_ARCH_H_
#include <mach/machine.h>
#include <architecture/byte_order.h>
typedef struct { const char *name; cpu_type_t cputype; cpu_subtype_t cpusubtype; enum NXByteOrder byteorder; const char *description; } NXArchInfo;
const NXArchInfo *NXGetLocalArchInfo(void);
const NXArchInfo *NXGetArchInfoFromName(const char *name);
const NXArchInfo *NXGetArchInfoFromCpuType(cpu_type_t cputype, cpu_subtype_t cpusubtype);
#endif
