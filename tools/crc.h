#ifndef _RTS_CRC_H_
#define _RTS_CRC_H_ 1

#ifdef __cplusplus
extern "C" {
#endif


#define CRC8_INIT_VALUE  0xff       /* Initial CRC8 checksum value */
#define CRC8_GOOD_VALUE  0x9f       /* Good final CRC8 checksum value */

#define CRC16_INIT_VALUE 0xffff     /* Initial CRC16 checksum value */
#define CRC16_GOOD_VALUE 0xf0b8     /* Good final CRC16 checksum value */

#define CRC32_INIT_VALUE 0xffffffff /* Initial CRC32 checksum value */
#define CRC32_GOOD_VALUE 0xdebb20e3 /* Good final CRC32 checksum value */

uint8_t fw_crc8(uint8_t *, unsigned int, uint8_t);
uint16_t fw_crc16(uint8_t *, unsigned int, uint16_t);
uint32_t fw_crc32(uint8_t *, unsigned int, uint32_t);

/* macros for common usage */

#define APPEND_CRC8(pbytes, nbytes)                           \
do {                                                          \
    uint8_t tmp = fw_crc8(pbytes, nbytes, CRC8_INIT_VALUE) ^ 0xff; \
    (pbytes)[(nbytes)] = tmp;                                 \
    (nbytes) += 1;                                            \
} while (0)

#define APPEND_CRC16(pbytes, nbytes)                               \
do {                                                               \
    uint16_t tmp = fw_crc16(pbytes, nbytes, CRC16_INIT_VALUE) ^ 0xffff; \
    (pbytes)[(nbytes) + 0] = (tmp >> 0) & 0xff;                    \
    (pbytes)[(nbytes) + 1] = (tmp >> 8) & 0xff;                    \
    (nbytes) += 2;                                                 \
} while (0)

#define APPEND_CRC32(pbytes, nbytes)                                   \
do {                                                                   \
    uint32_t tmp = fw_crc32(pbytes, nbytes, CRC32_INIT_VALUE) ^ 0xffffffff; \
    (pbytes)[(nbytes) + 0] = (tmp >>  0) & 0xff;                       \
    (pbytes)[(nbytes) + 1] = (tmp >>  8) & 0xff;                       \
    (pbytes)[(nbytes) + 2] = (tmp >> 16) & 0xff;                       \
    (pbytes)[(nbytes) + 3] = (tmp >> 24) & 0xff;                       \
    (nbytes) += 4;                                                     \
} while (0)

#ifdef __cplusplus
}
#endif

#endif /* _RTS_CRC_H_ */