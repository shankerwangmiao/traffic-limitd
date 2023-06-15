#ifndef __ASM_BYTEORDER_H
#define __ASM_BYTEORDER_H

#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
# include <linux/byteorder/big_endian.h>
#elif __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
# include <linux/byteorder/little_endian.h>
#else
# error "Fix your compiler's __BYTE_ORDER__?!"
#endif

#endif  /* __ASM_BYTEORDER_H */
