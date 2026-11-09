#ifndef _COMPAT_MACH_O_SWAP_H_
#define _COMPAT_MACH_O_SWAP_H_
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include <mach-o/fat.h>
#include <architecture/byte_order.h>
void swap_fat_header(struct fat_header *, enum NXByteOrder);
void swap_fat_arch(struct fat_arch *, uint32_t, enum NXByteOrder);
void swap_mach_header(struct mach_header *, enum NXByteOrder);
void swap_mach_header_64(struct mach_header_64 *, enum NXByteOrder);
void swap_load_command(struct load_command *, enum NXByteOrder);
void swap_segment_command(struct segment_command *, enum NXByteOrder);
void swap_segment_command_64(struct segment_command_64 *, enum NXByteOrder);
void swap_section(struct section *, uint32_t, enum NXByteOrder);
void swap_section_64(struct section_64 *, uint32_t, enum NXByteOrder);
void swap_symtab_command(struct symtab_command *, enum NXByteOrder);
void swap_uuid_command(struct uuid_command *, enum NXByteOrder);
void swap_nlist(struct nlist *, uint32_t, enum NXByteOrder);
void swap_nlist_64(struct nlist_64 *, uint32_t, enum NXByteOrder);
#endif
