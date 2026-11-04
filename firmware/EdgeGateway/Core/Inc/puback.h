/**
  ******************************************************************************
  * @file    puback.h
  * @brief   PUBACK 收包解析 —— "这条数据到底发成功没有"的唯一判定点（W4·S24）
  *
  *          为什么单独一个文件：
  *            这是整个数据流里最容易被写错的一步。发送函数 return 0、
  *            AT 指令回 OK，都不代表 broker 收到了。只有解析出 PUBACK、
  *            而且标识符对得上，才算成功。把这件事收进一个小模块，
  *            就是为了让"判定条件"只有一处、能单独测。
  *
  *          PUBACK 帧（MQTT 3.1.1 §3.4，固定 4 字节）：
  *            40  02  <ID 高>  <ID 低>
  *            │   │   └────────┴─ 报文标识符，必须和发出去的 PUBLISH 一致
  *            │   └─ Remaining Length = 2
  *            └─ 首字节：类型 4 << 4
  *
  *          ⚠️ 只看首字节 0x40 是不够的：Espressif AT 回显、TCP 粘包都可能
  *             让缓冲区里出现半帧或多帧，必须校验 RL 和总长度。
  *
  *          本层不依赖 HAL / FreeRTOS / ESP8266，PC 上可直接单测。
  ******************************************************************************
  */

#ifndef __PUBACK_H__
#define __PUBACK_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* PUBACK 固定 4 字节：首字节 + RL(1) + 标识符(2)。小于它一定不是完整帧。*/
#define PUBACK_FRAME_BYTES   4U

/* 解析结果。用枚举而不是 0/-1，是因为"没收全"和"不是 PUBACK"
 * 对调用方的处理方式完全不同：前者要等更多字节，后者要丢弃。*/
typedef enum {
    PUBACK_OK = 0,          /* 完整帧，标识符匹配 */
    PUBACK_NOT_FOUND,       /* 不是 PUBACK 帧（首字节不是 0x40） */
    PUBACK_INCOMPLETE,      /* 首字节像 PUBACK，但字节还不够，需要继续收 */
    PUBACK_ID_MISMATCH      /* 是合法 PUBACK，但标识符不是我们要等的那条 */
} puback_result_t;

/**
  * @brief  在收到的字节流里找 PUBACK，并核对标识符
  * @param  buf        收到的数据
  * @param  len        已收到的字节数
  * @param  expect_id  正在等的那条 PUBLISH 的报文标识符（不能为 0）
  * @param  out_used   输出：这一帧实际占用的字节数（成功时恒为 4）；
  *                    可为 NULL
  * @retval 见 puback_result_t
  * @note   不修改 buf，不访问 len 之外的任何字节。
  */
puback_result_t puback_parse(const uint8_t *buf, uint16_t len,
                             uint16_t expect_id, uint16_t *out_used);

/**
  * @brief  把解析结果翻译成一句人话（给日志用）
  * @return 静态字符串，永不返回 NULL
  */
const char *puback_result_name(puback_result_t r);

/* ==========================================================================
 * 收包累积器
 *
 *  串口是字节流，不给你"一帧"。AT 通道上更麻烦：前面可能挂着 "OK\r\n"、
 *  "> "、"+IPD," 之类的尾巴，PUBACK 可能被切在两片数据里。
 *
 *  所以不能"收到就解析"，得先攒。攒够 4 个字节、而且第一个是 0x40，
 *  才交给 puback_parse()。这份缓冲只有 4 B，因为 PUBACK 是 4 B 的固定帧。
 * ========================================================================== */

/* 累积器里最多存几个字节（PUBACK 固定帧长） */
#define PUBACK_ACC_BYTES   PUBACK_FRAME_BYTES

typedef struct {
    uint8_t  buf[PUBACK_ACC_BYTES];
    uint8_t  n;          /* 已攒字节数 */
} puback_acc_t;

/**
  * @brief  清空累积器
  */
void puback_acc_reset(puback_acc_t *acc);

/**
  * @brief  喂一个字节进去
  * @param  acc        累积器
  * @param  byte       收到的字节
  * @param  expect_id  当前在等的报文标识符
  * @retval PUBACK_OK          攒齐了一帧且 id 匹配（内部已自动清空，可继续喂）
  * @retval PUBACK_ID_MISMATCH 攒齐了一帧但 id 不匹配（也已清空）
  * @retval PUBACK_INCOMPLETE  还不够 4 字节，继续喂
  * @retval PUBACK_NOT_FOUND   这个字节不是 PUBACK 的开头，已丢弃
  * @note   一次只消费 4 B，所以串口来的垃圾数据不需要调用方先切帧。
  */
puback_result_t puback_acc_feed(puback_acc_t *acc, uint8_t byte,
                                uint16_t expect_id);

#ifdef __cplusplus
}
#endif

#endif /* __PUBACK_H__ */