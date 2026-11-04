/**
  ******************************************************************************
  * @file    puback.c
  * @brief   PUBACK 解析实现（W4·S24）
  ******************************************************************************
  */

#include "puback.h"

#include <stddef.h>

#include "mqtt_codec.h"     /* MQTT_MSG_PUBACK —— 不为 0x40 写第二份魔数 */

puback_result_t puback_parse(const uint8_t *buf, uint16_t len,
                             uint16_t expect_id, uint16_t *out_used)
{
    uint16_t id;

    if (buf == NULL) {
        return PUBACK_INCOMPLETE;
    }

    /* ① 先看有没有首字节、以及是不是 PUBACK 类型 */
    if (len < 1U) {
        return PUBACK_INCOMPLETE;
    }
    if (buf[0] != MQTT_MSG_PUBACK) {
        return PUBACK_NOT_FOUND;
    }

    /* ② 校验 Remaining Length。PUBACK 的 RL 必须是 2，
     *    不是 2 说明这个 0x40 其实是别的报文或粘包里的错位字节。*/
    if (len < 2U) {
        return PUBACK_INCOMPLETE;
    }
    if (buf[1] != 2U) {
        return PUBACK_NOT_FOUND;
    }

    /* ③ 字节不够整帧就等下一次（TCP 半包是常态，不是错误） */
    if (len < PUBACK_FRAME_BYTES) {
        return PUBACK_INCOMPLETE;
    }

    id = (uint16_t)(((uint16_t)buf[2] << 8) | (uint16_t)buf[3]);

    if (out_used != NULL) {
        *out_used = (uint16_t)PUBACK_FRAME_BYTES;
    }

    /* ④ 标识符必须和我们发出去的那条一致。
     *    QoS1 允许重发，所以 broker 可能回一个旧 id 的 PUBACK ——
     *    那不是这条记录的成功确认，不能拿它推进 rp。*/
    if (id != expect_id) {
        return PUBACK_ID_MISMATCH;
    }

    return PUBACK_OK;
}

const char *puback_result_name(puback_result_t r)
{
    switch (r) {
    case PUBACK_OK:          return "OK";
    case PUBACK_NOT_FOUND:   return "NOT_PUBACK";
    case PUBACK_INCOMPLETE:  return "INCOMPLETE";
    case PUBACK_ID_MISMATCH: return "ID_MISMATCH";
    default:                 return "?";
    }
}

void puback_acc_reset(puback_acc_t *acc)
{
    if (acc == NULL) {
        return;
    }
    acc->n = 0U;
}

puback_result_t puback_acc_feed(puback_acc_t *acc, uint8_t byte,
                                uint16_t expect_id)
{
    uint16_t used = 0U;
    puback_result_t r;

    if (acc == NULL) {
        return PUBACK_INCOMPLETE;
    }

    /* 第一个字节就必须是 0x40，否则整段丢弃。
     * 这样调用方不用先切帧 —— AT 回显、OK 尾巴全都会被自然滤掉。*/
    if (acc->n == 0U) {
        if (byte != MQTT_MSG_PUBACK) {
            return PUBACK_NOT_FOUND;
        }
        acc->buf[0] = byte;
        acc->n = 1U;
        return PUBACK_INCOMPLETE;
    }

    if (acc->n >= (uint8_t)PUBACK_ACC_BYTES) {
        acc->n = 0U;         /* 防御：不该走到这里 */
        return PUBACK_NOT_FOUND;
    }

    acc->buf[acc->n] = byte;
    acc->n++;

    if (acc->n < (uint8_t)PUBACK_ACC_BYTES) {
        return PUBACK_INCOMPLETE;
    }

    /* 攒够 4 B，交给真正的判定函数 */
    acc->n = 0U;
    r = puback_parse(acc->buf, (uint16_t)PUBACK_ACC_BYTES, expect_id, &used);

    if (r == PUBACK_NOT_FOUND) {
        /* 4 B 攒齐但不是合法 PUBACK（比如 RL 不是 2）：丢掉。
         * 这一帧占的 4 B 里没准还藏着下一帧的开头，但为了简单起见
         * 先整体丢弃 —— PUBACK 是 4 B 固定帧，错位只可能来自线路噪声。*/
        return PUBACK_NOT_FOUND;
    }
    return r;
}