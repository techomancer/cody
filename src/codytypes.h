#ifndef CODY_TYPES_H
#define CODY_TYPES_H

#ifdef __WATCOMC__
/* On DOS, pull in mTCP's types.h directly — it is the authoritative source
   for uint8_t/uint16_t/uint32_t/int8_t/int16_t on this platform.
   mTCP/tcpinc/ must be first on the -i= include path (it is, via cody.rsp).
   If mTCP headers were included before us, _TYPES_H is already set and
   this include is a no-op (guarded inside mTCP's types.h). */
#  include "types.h"
#elif defined(__sgi)
/* IRIX MIPSPro: stdint.h requires C99 mode (-c99), which CC doesn't support
   in C++ compilation.  Define the fixed-width types directly. */
typedef signed char        int8_t;
typedef short              int16_t;
typedef int                int32_t;
typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
#else
#  include <stdint.h>
#endif

#ifndef NULL
#  define NULL 0
#endif

#endif
