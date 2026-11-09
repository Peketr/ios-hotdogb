#ifndef _COMPAT_BYTE_ORDER_H_
#define _COMPAT_BYTE_ORDER_H_
#include <stdint.h>
enum NXByteOrder { NX_UnknownByteOrder, NX_LittleEndian, NX_BigEndian };
#define OSSwapInt16(x) __builtin_bswap16(x)
#define OSSwapInt32(x) __builtin_bswap32(x)
#define OSSwapInt64(x) __builtin_bswap64(x)
#define OSSwapBigToHostInt32(x) __builtin_bswap32(x)
#define OSSwapHostToBigInt32(x) __builtin_bswap32(x)
#define OSSwapLittleToHostInt32(x) (x)
#define OSSwapHostToLittleInt32(x) (x)
#endif
