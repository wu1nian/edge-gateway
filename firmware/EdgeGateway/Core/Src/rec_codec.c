/**
  ******************************************************************************
  * @file    rec_codec.c
  * @brief   sensor_rec_t ↔ 32 B Flash 记录的序列化实现（W4·S24）
  *
  *          读写的字节全是手工拼的，不用 memcpy 整个结构体：
  *          结构体布局由编译器决定（见 app_tasks.h 的 _Static_assert），
  *          而 Flash 格式必须由我们自己决定，两者不能共用一个契约。
  ******************************************************************************
  */

#include "rec_codec.h"

#include <stddef.h>
#include <string.h>

#include "modbus_host.h"    /* mb_crc16 —— CRC-16/MODBUS，和 Modbus 帧同一个算法 */

/* 单字节读写：显式小端，不依赖 MCU 的字节序和结构体对齐 */
static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFU);
    p[1] = (uint8_t)((v >> 8) & 0xFFU);
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFU);
    p[1] = (uint8_t)((v >> 8) & 0xFFU);
    p[2] = (uint8_t)((v >> 16) & 0xFFU);
    p[3] = (uint8_t)((v >> 24) & 0xFFU);
}

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

int rec_encode(const sensor_rec_t *rec, uint8_t *out)
{
    if ((rec == NULL) || (out == NULL)) {
        return -1;
    }

    /* 先清零：预留区必须是确定的 0，不能把栈上的垃圾写进 Flash。
     * 这也让同一条记录每次编码的结果完全相同（便于比对/测试）。*/
    (void)memset(out, 0, RB_RECORD_BYTES);

    put_u32(&out[REC_OFF_TS],  rec->timestamp_ms);
    put_u16(&out[REC_OFF_TEMP], rec->temp_x10);
    put_u16(&out[REC_OFF_HUMI], rec->humi_x10);
    put_u16(&out[REC_OFF_LIGHT], rec->light);

    /* seq 拆两块：高位放前面、低位放最后，见头文件的说明 */
    put_u16(&out[REC_OFF_SEQ_HI], (uint16_t)((rec->seq >> 16) & 0xFFFFU));
    put_u16(&out[REC_OFF_SEQ_LO], (uint16_t)(rec->seq & 0xFFFFU));

    put_u32(&out[REC_OFF_MAGIC], REC_MAGIC);

    /* CRC 最后算，只覆盖前 28 B（CRC 字段本身不参与） */
    put_u16(&out[REC_OFF_CRC],
            mb_crc16(out, (uint16_t)REC_OFF_CRC));

    return 0;
}

int rec_decode(const uint8_t *in, sensor_rec_t *rec)
{
    uint16_t crc_calc;
    uint16_t crc_stored;

    if ((in == NULL) || (rec == NULL)) {
        return -1;
    }

    /* 先验 magic 再验 CRC：
     * 新芯片擦除后是 0xFF，magic 必然不匹配。若先算 CRC，
     * 空槽也会被判成"坏块"，计数就全废了。*/
    if (get_u32(&in[REC_OFF_MAGIC]) != REC_MAGIC) {
        return -2;
    }

    crc_stored = get_u16(&in[REC_OFF_CRC]);
    crc_calc   = mb_crc16(in, (uint16_t)REC_OFF_CRC);
    if (crc_calc != crc_stored) {
        return -3;
    }

    rec->timestamp_ms = get_u32(&in[REC_OFF_TS]);
    rec->temp_x10     = get_u16(&in[REC_OFF_TEMP]);
    rec->humi_x10     = get_u16(&in[REC_OFF_HUMI]);
    rec->light        = get_u16(&in[REC_OFF_LIGHT]);
    rec->seq          = ((uint32_t)get_u16(&in[REC_OFF_SEQ_HI]) << 16)
                      | (uint32_t)get_u16(&in[REC_OFF_SEQ_LO]);

    return 0;
}

int rec_is_blank(const uint8_t *in)
{
    if (in == NULL) {
        return -1;
    }
    return (get_u32(&in[REC_OFF_MAGIC]) == REC_MAGIC) ? 0 : 1;
}