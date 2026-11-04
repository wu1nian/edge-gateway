/**
  ******************************************************************************
  * @file    net_tx.h
  * @brief   发送窗口 = 1 的补传/在线发送编排器（W4·S24）
  *
  *          这一层回答一个具体问题：**什么时候才允许 rb_commit()**。
  *          答案只有一句话 —— 解析出标识符匹配的 PUBACK 之后，其他任何时候都不行。
  *          把这条规则写成一个状态机，而不是散在 net_task 的 if 里，
  *          是因为它有三个独立的时刻（发出、超时、确认），每个都要正确。
  *
  *          一次发送的生命周期：
  *
  *            空闲 ──net_tx_begin──► 等待中(WM) ──net_tx_on_puback(匹配)──► 空闲
  *                                        │                                 + rb_commit()
  *                                        ├──net_tx_on_puback(不匹配)──► 仍在等待
  *                                        └──net_tx_check_timeout 到期──► 空闲（不回退 rp）
  *
  *          为什么窗口必须是 1：
  *            ① 断网 30 分钟积压 1800 条，窗口放开会把 ESP8266 的 TCP 缓冲和
  *               broker 一起压垮；窗口 1 天然限速，不用再算令牌桶。
  *            ② 窗口 1 时"当前在等哪条"只有一个答案，PUBACK 的标识符
  *               一定能对上号，不会出现"这个 ACK 是哪条的"这种问题。
  *
  *          硬件隔离：
  *            真正发字节要走 ESP8266（W7）。本层不碰串口，只通过
  *            net_tx_send_fn 回调把"要发的载荷"交出去。PC 上换个假回调
  *            就能把整个补传流程跑完，不用等硬件。
  *
  *          本层只依赖 ring_buffer / rec_codec / puback，不依赖 HAL / FreeRTOS。
  ******************************************************************************
  */

#ifndef __NET_TX_H__
#define __NET_TX_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ring_buffer.h"

/* 等 PUBACK 的超时。取 3 s：比一次 AT 往返（正常几十 ms）宽得多，
 * 又能保证断线时 net_task 不会卡死等不到（NET_RECV_TIMEOUT_MS 是 500 ms，
 * 所以实际检查粒度受任务节拍限制）。*/
#define NET_TX_ACK_TIMEOUT_MS   3000U

/* 报文标识符的取值范围是 1..65535，0 保留（MQTT §2.3.1） */
#define NET_TX_PKTID_MIN          1U
#define NET_TX_PKTID_MAX      65535U

/**
  * @brief 发送回调：把载荷交出去。
  * @param payload  已组好的 payload（不含 MQTT 头，W7 负责封装）
  * @param len      字节数
  * @param pkt_id   本次的报文标识符，调用方要记住，PUBACK 回来会带它
  * @return 0 表示"字节已交给 ESP8266"；非 0 表示当场失败（不用等 PUBACK）
  * @note   🚨 返回 0 **不等于** broker 收到了。这只是"发出去了"，
  *         成功与否只能由 net_tx_on_puback() 判定。
  */
typedef int (*net_tx_send_fn)(const uint8_t *payload, uint16_t len, uint16_t pkt_id);

/**
  * @brief 初始化（清空在途状态、重置标识符计数器）
  */
void net_tx_init(void);

/**
  * @brief 从环形缓冲取最旧一条，组装并发出（窗口 1）
  * @param  send  发送回调；为 NULL 时视为"没有网络"，直接返回 NET_TX_NO_NET
  * @param  out_id 输出：本次使用的报文标识符；可为 NULL
  * @retval NET_TX_STARTED     已发出，进入等待 PUBACK 状态
  * @retval NET_TX_BUSY        上一条还没确认，这次不发（窗口 1 的直接体现）
  * @retval NET_TX_NO_DATA     环形缓冲空，没东西可发
  * @retval NET_TX_NO_NET      发送回调为空
  * @retval NET_TX_SEND_FAIL   回调返回非 0，已退出等待状态
  * @retval NET_TX_ERR         参数/编码异常
  * @note   本函数**不**推进 rp。没收到 PUBACK 之前，那条记录还在缓冲里。
  */
typedef enum {
    NET_TX_STARTED = 0,
    NET_TX_BUSY,
    NET_TX_NO_DATA,
    NET_TX_NO_NET,
    NET_TX_SEND_FAIL,
    NET_TX_ERR
} net_tx_result_t;

net_tx_result_t net_tx_begin(net_tx_send_fn send, uint16_t *out_id);

/**
  * @brief  告诉本层"这条是什么时刻发出去的"
  * @param  now_ms 发送时刻（ms，来自 xTaskGetTickCount）
  * @note   单独一个函数而不是塞进 begin：begin 是纯逻辑，
  *         不该去读时基；时间由调用方（net_task）提供，测试里就能喂假时间。
  *         不调它的话，超时判定不会生效（宁可等不到超时，也不能误判超时重发）。
  */
void net_tx_note_sent_at(uint32_t now_ms);

/**
  * @brief  收到 PUBACK 时调用 —— 这里是唯一会推进 rp 的地方之一
  * @param  pkt_id  收到的 PUBACK 里的标识符
  * @retval 1  匹配，已 rb_commit()，通道空出来了
  * @retval 0  不匹配或当前没有在途消息 —— 忽略，不改任何状态
  * @note   只有匹配才提交。旧 id 的迟到 ACK 不能用来推进 rp。
  */
int net_tx_on_puback(uint16_t pkt_id);

/**
  * @brief  检查等 PUBACK 是否超时
  * @param  now_ms 当前毫秒
  * @retval 1  超时了，已退出等待状态（**但没有回退 rp，也没有提交**）
  * @retval 0  没超时 / 本来就不在等待
  * @note   超时的语义是"这条没确认，保留在缓冲里，下次重发"，
  *         所以这里什么都不提交 —— QoS1 的"至少一次"就是这样来的。
  */
int net_tx_check_timeout(uint32_t now_ms);

/**
  * @brief 是否正在等 PUBACK（窗口占用中）
  */
int net_tx_is_busy(void);

/**
  * @brief 当前在途报文的标识符；没有在途返回 0
  */
uint16_t net_tx_inflight_id(void);

/**
  * @brief 统计快照，给 monitor 打印
  */
typedef struct {
    uint32_t start;      /* net_tx_begin 成功发出的次数 */
    uint32_t ack_ok;     /* 匹配 PUBACK 并提交的次数 */
    uint32_t ack_bad;    /* 标识符对不上的 PUBACK 次数 */
    uint32_t timeout;    /* 等 PUBACK 超时次数 */
    uint32_t send_fail;  /* 回调当场失败的次数 */
} net_tx_stat_t;

void net_tx_stat(net_tx_stat_t *out);

/**
  * @brief 结果码翻译成人话（给日志用），永不返回 NULL
  */
const char *net_tx_result_name(net_tx_result_t r);

#ifdef __cplusplus
}
#endif

#endif /* __NET_TX_H__ */