/**
  ******************************************************************************
  * @file    net_tx.c
  * @brief   发送窗口 = 1 的补传/在线发送编排实现（W4·S24）
  *
  *          三条状态迁移各自只做一件事，绝不越界：
  *            begin   只发不提交；
  *            puback  只提交，条件是标识符匹配；
  *            timeout 只释放窗口，不提交也不回退。
  *          把这三件事分开写，是为了让"PUBACK 才 rp++"变成代码结构本身，
  *          而不是靠注释提醒自己别写错。
  ******************************************************************************
  */

#include "net_tx.h"

#include <stddef.h>

#include "rec_codec.h"

/* "还没记过发送时刻"的哨兵。
 * ⚠️ 不能用 0：启动后第一次发送的 tick 很可能就是 0，
 *    那样 now_ms - 0 会立刻被判成超时，第一条记录永远发不出去重发不完。
 * 用 0xFFFFFFFF 兜住这个边界。*/
#define NET_TX_NO_SENT_AT   0xFFFFFFFFU

/* 在途消息：有它才允许收 PUBACK */
static int      s_inflight;
static uint16_t s_inflight_id;
static uint32_t s_sent_ms;

/* 标识符计数器：1..65535 循环，跳过 0（0 保留） */
static uint16_t s_next_id;

static net_tx_stat_t s_stat;

/* 载荷缓冲：一条记录的 payload 是 32 B，留点余量给 W7 加时间戳前缀 */
static uint8_t s_payload[RB_RECORD_BYTES];

void net_tx_init(void)
{
    s_inflight    = 0;
    s_inflight_id = 0U;
    s_sent_ms     = NET_TX_NO_SENT_AT;
    s_next_id     = NET_TX_PKTID_MIN;

    s_stat.start     = 0U;
    s_stat.ack_ok    = 0U;
    s_stat.ack_bad   = 0U;
    s_stat.timeout   = 0U;
    s_stat.send_fail = 0U;
}

static uint16_t next_pkt_id(void)
{
    uint16_t id = s_next_id;

    /* 自增并绕回。⚠️ 不能写成 s_next_id++ 后判 0，
     *    因为 0xFFFF + 1 在 uint16 下自然回到 0，必须跳到 1。*/
    if (s_next_id >= NET_TX_PKTID_MAX) {
        s_next_id = NET_TX_PKTID_MIN;
    } else {
        s_next_id++;
    }
    return id;
}

net_tx_result_t net_tx_begin(net_tx_send_fn send, uint16_t *out_id)
{
    rb_seq_t seq;
    sensor_rec_t rec;
    uint16_t id;
    int32_t  rc;

    if (out_id != NULL) {
        *out_id = 0U;
    }
    if (send == NULL) {
        return NET_TX_NO_NET;
    }

    /* ① 窗口已占用：一个字都不发。这条 return 就是"窗口=1"。*/
    if (s_inflight) {
        return NET_TX_BUSY;
    }

    /* ② 取最旧一条。peek 不推进 rp，所以取失败也没副作用。*/
    if (rb_peek(&seq) != 0) {
        return NET_TX_NO_DATA;
    }

    /* ③ W5：这里要从 W25Q64 读出 rb_seq_to_offset(seq) 那 32 B 再解码。
     *    现在没有 Flash 后端，用一个确定的占位记录，
     *    但 seq 取真实值，保证补传顺序和缓冲一致。*/
    rec.timestamp_ms = 0U;
    rec.temp_x10     = 0U;
    rec.humi_x10     = 0U;
    rec.light        = 0U;
    rec.seq          = (uint32_t)seq;

    /* ④ 编码成 32 B。序列化本身是真的，W5 接上后原样复用。*/
    rc = rec_encode(&rec, s_payload);
    if (rc != 0) {
        return NET_TX_ERR;
    }

    id = next_pkt_id();
    if (id == 0U) {          /* next_pkt_id 不该返回 0，防一手 */
        return NET_TX_ERR;
    }

    /* ⑤ 标记在途要在回调**之前**：回调里可能同步触发 PUBACK 解析，
     *    那时 s_inflight 必须已经是 1，否则 ACK 会被当成"没有在途"丢掉。*/
    s_inflight    = 1;
    s_inflight_id = id;
    s_sent_ms     = NET_TX_NO_SENT_AT;   /* 由 net_task 随后调 note_sent_at 赋值 */

    /* ⑥ 真正发字节。🚨 返回 0 只代表"交给了 ESP8266"，不代表 broker 收到。*/
    rc = send(s_payload, (uint16_t)RB_RECORD_BYTES, id);
    if (rc != 0) {
        s_inflight    = 0;
        s_inflight_id = 0U;
        s_stat.send_fail++;
        return NET_TX_SEND_FAIL;
    }

    s_stat.start++;
    if (out_id != NULL) {
        *out_id = id;
    }
    return NET_TX_STARTED;
}

void net_tx_note_sent_at(uint32_t now_ms)
{
    s_sent_ms = now_ms;
}

int net_tx_on_puback(uint16_t pkt_id)
{
    if (!s_inflight) {
        /* 没有在途消息却收到 PUBACK：重连后的迟到包，正常现象，忽略。*/
        return 0;
    }
    if (pkt_id != s_inflight_id) {
        /* 标识符对不上：不能拿它当这条记录的成功确认。*/
        s_stat.ack_bad++;
        return 0;
    }

    /* 🚨 到这里才是真正的"发成功了"。
     *    整个工程里推进 rp 的路径只有这里和 net_task 的超时之外没有别处。*/
    if (rb_commit() == 0) {
        s_stat.ack_ok++;
    }

    s_inflight    = 0;
    s_inflight_id = 0U;
    return 1;
}

int net_tx_check_timeout(uint32_t now_ms)
{
    if (!s_inflight) {
        return 0;
    }

    /* 无符号减法处理 32 位回绕。还是哨兵值说明没记过发送时刻，
     * 此时不判超时，等 net_tx_note_sent_at 赋值后再算。*/
    if (s_sent_ms == NET_TX_NO_SENT_AT) {
        return 0;
    }
    if ((uint32_t)(now_ms - s_sent_ms) < NET_TX_ACK_TIMEOUT_MS) {
        return 0;
    }

    /* 超时：只放开窗口。**不提交**（记录还在缓冲里，下次重发），
     *       也**不回退 rp**（rp 根本没动过）—— 这就是 QoS1 的"至少一次"。*/
    s_inflight    = 0;
    s_inflight_id = 0U;
    s_stat.timeout++;
    return 1;
}

int net_tx_is_busy(void)
{
    return s_inflight;
}

uint16_t net_tx_inflight_id(void)
{
    return s_inflight ? s_inflight_id : 0U;
}

void net_tx_stat(net_tx_stat_t *out)
{
    if (out == NULL) {
        return;
    }
    *out = s_stat;
}

const char *net_tx_result_name(net_tx_result_t r)
{
    switch (r) {
    case NET_TX_STARTED:   return "STARTED";
    case NET_TX_BUSY:      return "BUSY";
    case NET_TX_NO_DATA:   return "NO_DATA";
    case NET_TX_NO_NET:    return "NO_NET";
    case NET_TX_SEND_FAIL: return "SEND_FAIL";
    case NET_TX_ERR:       return "ERR";
    default:               return "?";
    }
}