#include <stdio.h>
#include "modbus_host.h"

static void dump(const char *tag, const uint8_t *p, int n)
{
    printf("%-6s", tag);
    for (int i = 0; i < n; i++) {
        printf("%02X ", p[i]);
    }
    printf("\n");
}

int main(void)
{
    uint8_t tx[8];

    printf("CRC(123456789) = 0x%04X (expect 4B37)\n",
           mb_crc16((const uint8_t *)"123456789", 9));

    if (mb_build_read_req(0x01, MB_FC_READ_HOLDING, 0x0001, 2, tx) == 8)
        dump("0x03:", tx, 8);   /* 01 03 00 01 00 02 95 CB */

    if (mb_build_read_req(0x01, MB_FC_READ_INPUT, 0x0001, 2, tx) == 8)
        dump("0x04:", tx, 8);   /* 01 04 00 01 00 02 20 0B */

    if (mb_build_write_single(0x01, SHT20_REG_ADDR, 0x0008, tx) == 8)
        dump("0x06:", tx, 8);   /* 01 06 01 01 00 08 D8 30 */

    return 0;
}
