/*
 * Per-phone constants for wland. Same SoC (SM8150) on both phones: SMEM, SMP2P, GLINK, the WLAN
 * block at 0x18800000, its SMMU stream 0x640, the MSA region and the WLAN regulators (pm8150 l1/l7,
 * pm8150l l2/l11) are identical. What differs is the modem carve-out and the board data file.
 *
 * Build: BOARD=hotdogb (default, OnePlus 7T) or BOARD=flame (Pixel 4) in build.sh.
 */
#ifndef WLAND_BOARD_H
#define WLAND_BOARD_H

#ifndef SWEET_HOTDOGB
#define SWEET_HOTDOGB 1
#endif

#if SWEET_HOTDOGB
#define BOARD_NAME	"hotdogb"
/* OnePlus 7T live FDT: reserved-memory/modem_region <0x8d800000 0x9600000> */
#define MPSS_PA		0x8d800000ull
#define MPSS_SIZE	0x09600000ull
/* the OnePlus board data, copied to this name (HOTDOGB.md "Wi-Fi") */
#define BDF_PATH	"/var/local/fw/modem/bdwlan.bin"
#else
#define BOARD_NAME	"flame"
/* Pixel 4: modem_region <0x8dc00000 0xb400000> */
#define MPSS_PA		0x8dc00000ull
#define MPSS_SIZE	0x0b400000ull
#define BDF_PATH	"/var/local/fw/modem/bdwlan-flame.bin"
#endif

#endif
