/**
  ******************************************************************************
  * @file    rec_codec.h
  * @brief   sensor_rec_t ↔ 32 B Flash 记录的序列化层（W4·S24）
  *
  *          为什么要有这一层：
  *            sensor_rec_t 是 16 B 的内存结构体，靠编译器对齐；
  *            Flash 里的一条记录是 32 B 的自描述格式，靠字节偏移。
  *            两者一旦混用，换个编译器/加个字段就全乱。所以中间夹一层显式
  *            编解码，字节序和对齐都由代码自己保证，不依赖 ABI。
  *
  *          Flash 记录布局（32 B，小端，除 crc 外全部固定偏移）：
  *
  *            偏移  长度  字段
  *             0     4    timestamp_ms
  *             4     2    temp_x10
  *             6     2    humi_x10
  *             8     2    light
  *            10     2    seq 的高 16 位
  *            12     4    magic（0x47573152 "GW1R"）—— 用来识别"这片区域是空的"
  *            16     4    预留（W5 的写入时间戳 / 版本号）
  *            20     8    预留
  *            28     2    CRC-16/MODBUS（覆盖前 28 B）
  *            30     2    seq 的低 16 位
  *
  *          ⚠️ seq 被拆成两块是刻意的：内存结构体里 seq 在偏移 12，
  *            如果直接 memcpy 丢给 Flash，这里的偏移 12 放的是 magic 冲突。
  *            拆开写能让人一眼看出"落盘格式不等于内存格式"。
  *
  *          本层不依赖 HAL / FreeRTOS / W25Q64，PC 上可直接单测。
  ******************************************************************************
  */

#ifndef __REC_CODEC_H__
#define __REC_CODEC_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "app_tasks.h"      /* sensor_rec_t */
#include "ring_buffer.h"    /* RB_RECORD_BYTES */

/* 空槽标志：读出来的 magic 不等于它，说明这一槽从没写过（新芯片的 0xFF）。*/
#define REC_MAGIC           0x47573152U   /* "GW1R" */

/* 各字段在 32 B 记录里的偏移，编码/解码共用，避免两处写两份魔数 */
#define REC_OFF_TS          0U
#define REC_OFF_TEMP        4U
#define REC_OFF_HUMI        6U
#define REC_OFF_LIGHT       8U
#define REC_OFF_SEQ_HI      10U
#define REC_OFF_MAGIC       12U
#define REC_OFF_RSV_A       16U
#define REC_OFF_RSV_B       20U
#define REC_OFF_CRC         28U
#define REC_OFF_SEQ_LO      30U

/**
  * @brief  把一条内存记录编码成 32 B Flash 记录（含 CRC）
  * @param  rec    源记录，不能为 NULL
  * @param  out    输出缓冲，至少 RB_RECORD_BYTES 字节，不能为 NULL
  * @retval 0      成功
  * @retval -1     参数非法
  * @note   编码前会先把 out 清零，所以预留字节恒为 0，不是随机垃圾。
  */
int rec_encode(const sensor_rec_t *rec, uint8_t *out);

/**
  * @brief  把 32 B Flash 记录解码回内存记录
  * @param  in     源缓冲，至少 RB_RECORD_BYTES 字节，不能为 NULL
  * @param  rec    输出记录，不能为 NULL
  * @retval 0      成功
  * @retval -1     参数非法
  * @retval -2     magic 不对 —— 这一槽没写过
  * @retval -3     CRC 校验失败 —— 掉电半写或数据损坏
  * @note   调用方必须把 -2 和 -3 分开处理：-2 是正常的"还没写"，
  *         -3 才是真的坏块，要计数上报。混成一个 -1 就查不出问题了。
  */
int rec_decode(const uint8_t *in, sensor_rec_t *rec);

/**
  * @brief  这一槽是否为空（magic 不匹配）
  * @retval 1 空 / 0 有数据 / -1 参数非法
  */
int rec_is_blank(const uint8_t *in);

#ifdef __cplusplus
}
#endif

#endif /* __REC_CODEC_H__ */