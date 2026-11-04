/**
  ******************************************************************************
  * @file    net_fsm.h
  * @brief   网络三态机（W4·S24）—— OFFLINE / ONLINE / BACKFILL
  *
  *          ⚠️ 为什么不是 bool is_online：
  *             "连着但还在补传"是一个真实的中间态。
  *             当 bool 处理 → 补传期间按普通节拍发，积压永远清不完；
  *             当离线处理   → 就不该发。
  *             三态是刚需，不是设计过度。
  *
  *          状态迁移：
  *
  *            ┌──────────── 连接失败 / 断开 ────────────┐
  *            ▼                                         │
  *       OFFLINE ──CONNACK=00──► ONLINE ──有积压──► BACKFILL
  *            ▲                 （按节拍发）              │
  *            └──────────────── 补完 ─────────────────────┘
  *
  *          这个模块是纯逻辑，不碰 HAL 也不碰 FreeRTOS，
  *          所以能在 PC 上直接单元测试，和 W2/W3 的协议层一个路子。
  ******************************************************************************
  */

#ifndef __NET_FSM_H__
#define __NET_FSM_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

typedef enum {
    NET_OFFLINE = 0,   /* 没连上或已断开：只重连，不发数据 */
    NET_ONLINE,        /* 连着且无积压：按节拍发新数据 */
    NET_BACKFILL       /* 连着但有积压：快速补传，补完回 ONLINE */
} net_state_t;

/* 指数退避参数：1s → 2s → 4s → 8s → 16s → 30s（封顶）
 * ⚠️ 封顶是必须的：不封顶的话断网 10 分钟，下次重连要等半个多小时。*/
#define NET_BACKOFF_MIN_MS   1000U
#define NET_BACKOFF_MAX_MS  30000U

/**
  * @brief  初始化状态机（回到 OFFLINE，退避重置为最小值）
  */
void net_fsm_init(void);

/**
  * @brief 读当前状态
  */
net_state_t net_fsm_state(void);

/**
  * @brief 是否允许发送业务数据（只有 ONLINE / BACKFILL 允许）
  */
int net_fsm_can_send(void);

/**
  * @brief  收到 CONNACK=0x00（连接成功）
  * @param  pending 当前环形缓冲里积压的条数
  * @note   有积压直接进 BACKFILL，没积压才进 ONLINE。
  *         这一步是"重连后先补传还是先发新数据"的分叉点。
  */
void net_fsm_on_connected(uint32_t pending);

/**
  * @brief 连接失败 / 链路断开 → 回 OFFLINE
  * @note  重连期间 rp 停在原处，已经发出去的记录等重连后重发（QoS1 至少一次）。
  */
void net_fsm_on_disconnected(void);

/**
  * @brief  一次 PUBACK 处理完之后调用，检查要不要退出 BACKFILL
  * @param  pending 当前剩余积压条数
  * @note   只有 BACKFILL 会响应它：pending 归零就回 ONLINE。
  */
void net_fsm_after_ack(uint32_t pending);

/**
  * @brief 取本次重连应该等多久（指数退避），并在内部把下一次间隔翻倍
  * @note   调用一次拿到一个值。封顶 NET_BACKOFF_MAX_MS。
  */
uint32_t net_fsm_next_backoff_ms(void);

/**
  * @brief 连接成功时把退避计数器复位（下次断网又从 1s 开始等）
  */
void net_fsm_reset_backoff(void);

/**
  * @brief 当前退避值（只读，给 monitor 打印）
  */
uint32_t net_fsm_backoff_ms(void);

/**
  * @brief 状态名（只读常量字符串，用于打印）
  */
const char *net_fsm_state_name(net_state_t st);

#ifdef __cplusplus
}
#endif

#endif /* __NET_FSM_H__ */