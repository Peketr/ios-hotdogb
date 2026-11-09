/* libmacho bits for the XNU SETUP host tools on Linux. Host and target are both
 * little-endian, so the tools never need byte swapping: the swap_* entry points
 * only exist to link and abort if a big-endian file ever shows up. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mach-o/arch.h>
#include <mach-o/swap.h>

static const NXArchInfo archs[] = {
	{ "x86_64", CPU_TYPE_X86_64, CPU_SUBTYPE_X86_64_ALL, NX_LittleEndian, "Intel x86-64" },
	{ "x86_64h", CPU_TYPE_X86_64, CPU_SUBTYPE_X86_64_H, NX_LittleEndian, "Intel x86-64h" },
	{ "arm64", CPU_TYPE_ARM64, CPU_SUBTYPE_ARM64_ALL, NX_LittleEndian, "arm64" },
	{ "arm64e", CPU_TYPE_ARM64, CPU_SUBTYPE_ARM64E, NX_LittleEndian, "arm64e" },
	{ "armv7", CPU_TYPE_ARM, CPU_SUBTYPE_ARM_V7, NX_LittleEndian, "armv7" },
	{ "armv7s", CPU_TYPE_ARM, CPU_SUBTYPE_ARM_V7S, NX_LittleEndian, "armv7s" },
	{ "armv7k", CPU_TYPE_ARM, CPU_SUBTYPE_ARM_V7K, NX_LittleEndian, "armv7k" },
};
const NXArchInfo *NXGetLocalArchInfo(void) { return &archs[0]; }
const NXArchInfo *NXGetArchInfoFromName(const char *n) {
	for (size_t i = 0; i < sizeof archs / sizeof archs[0]; i++) if (!strcmp(archs[i].name, n)) return &archs[i];
	return NULL;
}
const NXArchInfo *NXGetArchInfoFromCpuType(cpu_type_t t, cpu_subtype_t s) {
	for (size_t i = 0; i < sizeof archs / sizeof archs[0]; i++)
		if (archs[i].cputype == t && (s == -1 || archs[i].cpusubtype == (s & ~CPU_SUBTYPE_MASK))) return &archs[i];
	return NULL;
}
static void noswap(const char *f) { fprintf(stderr, "%s: big-endian Mach-O not supported on this Linux build\n", f); abort(); }
#define NOSWAP1(n, T) void n(T *p, enum NXByteOrder o) { (void)p; (void)o; noswap(#n); }
#define NOSWAPN(n, T) void n(T *p, uint32_t c, enum NXByteOrder o) { (void)p; (void)c; (void)o; noswap(#n); }
NOSWAP1(swap_fat_header, struct fat_header)
NOSWAPN(swap_fat_arch, struct fat_arch)
NOSWAP1(swap_mach_header, struct mach_header)
NOSWAP1(swap_mach_header_64, struct mach_header_64)
NOSWAP1(swap_load_command, struct load_command)
NOSWAP1(swap_segment_command, struct segment_command)
NOSWAP1(swap_segment_command_64, struct segment_command_64)
NOSWAPN(swap_section, struct section)
NOSWAPN(swap_section_64, struct section_64)
NOSWAP1(swap_symtab_command, struct symtab_command)
NOSWAP1(swap_uuid_command, struct uuid_command)
NOSWAPN(swap_nlist, struct nlist)
NOSWAPN(swap_nlist_64, struct nlist_64)
