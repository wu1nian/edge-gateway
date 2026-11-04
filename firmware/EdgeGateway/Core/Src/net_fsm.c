/**
  ******************************************************************************
  * @file    net_fsm.c
  * @brief   网络三态机实现
  *
  *          纯逻辑，不依赖 HAL / FreeRTOS / 队列。
  *          输入是"连接结果"和"积压条数"，输出是"当前状态"和"该等多久"。
  *
  *          这样拆的好处：三态机的每个迁移都能在 PC 上跑单元测试，
  *          不用等 ESP8266 到位。和 W2 的 Modbus、W3 的 MQTT 一个套路。
  ******************************************************************************
  */

#include "net_fsm.h"

#include <stddef.h>

static net_state_t s_state = NET_OFFLINE;
static uint32_t    s_backoff_ms = NET_BACKOFF_MIN_MS;

void net_fsm_init(void)
{
    s_state      = NET_OFFLINE;
    s_backoff_ms = NET_BACKOFF_MIN_MS;
}

net_state_t net_fsm_state(void)
{
    return s_state;
}

int net_fsm_can_send(void)
{
    /* OFFLINE 一个字都不发：没连接时发只会把数据丢在半路 */
    return (s_state == NET_OFFLINE) ? 0 : 1;
}

void net_fsm_on_connected(uint32_t pending)
{
    /* 分叉点：有积压先补传，没积压直接进正常节拍。
     * 如果这里不分叉、一律进 ONLINE，积压就只能靠"按 1s 节拍慢慢发"，
     * 断网 30 分钟攒下的 1800 条要花 30 分钟才发完 —— 期间又会产生新数据。*/
    if (pending > 0U) {
        s_state = NET_BACKFILL;
    } else {
        s_state = NET_ONLINE;
    }

    /* 连上了就重置退避，下次断网又从 1s 开始等 */
    net_fsm_reset_backoff();
}

void net_fsm_on_disconnected(void)
{
    s_state = NET_OFFLINE;
    /* 注意：这里不动 rp。已经发出去但没等到 PUBACK 的记录会重发，
     * 这是 QoS1"至少一次"的语义，代价是可能重复，不是丢失。*/
}

void net_fsm_after_ack(uint32_t pending)
{
    /* 只有 BACKFILL 关心积压清零。
     * ONLINE 状态下 pending 短暂为 0 是常态，不需要做任何事。*/
    if (s_state == NET_BACKFILL) {
        if (pending == 0U) {
            s_state = NET_ONLINE;
        }
    }
}

uint32_t net_fsm_next_backoff_ms(void)
{
    uint32_t wait = s_backoff_ms;

    /* 翻倍，封顶。不封顶的话断网久了下次重连要等到天荒地老。*/
    if (s_backoff_ms < (NET_BACKOFF_MAX_MS / 2U)) {
        s_backoff_ms *= 2U;
    } else {
        s_backoff_ms = NET_BACKOFF_MAX_MS;
    }

    return wait;
}

void net_fsm_reset_backoff(void)
{
    s_backoff_ms = NET_BACKOFF_MIN_MS;
}

uint32_t net_fsm_backoff_ms(void)
{
    return s_backoff_ms;
}

const char *net_fsm_state_name(net_state_t st)
{
    switch (st) {
    case NET_OFFLINE:  return "OFFLINE";
    case NET_ONLINE:   return "ONLINE";
    case NET_BACKFILL: return "BACKFILL";
    default:           return "?";
    }
}