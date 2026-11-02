/**
  ******************************************************************************
  * @file    app_tasks.h
  * @brief   W4·S25：任务骨架的对外接口
  *
  *          设计原则：对外**只开两个门**——init 和读网络状态。
  *          其余（任务句柄、队列句柄、内部计数）全部 static 收在 app_tasks.c 里。
  *          这是对原项目坑 6（大量 extern 全局变量、模块边界模糊）的直接修正。
  ******************************************************************************
  */

#ifndef __APP_TASKS_H__
#define __APP_TASKS_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* ==========================================================================
 * 网络状态机（W4·S24 定的三态）
 *
 * ⚠️ 为什么不能用 bool is_online：
 *    "连着但还在补传历史数据"是一个**真实的中间态**。
 *    当 bool 处理 → 补传期间按普通节拍发，积压永远清不完；
 *    当离线处理   → 就不该发。所以三态是刚需，不是设计过度。
 * ========================================================================== */
typedef enum {
    NET_OFFLINE = 0,   /* 没连上或已断开：只重连，不发数据 */
    NET_ONLINE,        /* 连着且无积压：按节拍发新数据 */
    NET_BACKFILL       /* 连着但有积压：快速补传，补完回 ONLINE */
} net_state_t;

/* ==========================================================================
 * 一条传感记录（W5 落盘格式与本结构对应；注意全整数，不用 float）
 * 25.3℃ → temp_x10 = 253，避免在 MCU 上做浮点格式化
 * ========================================================================== */
typedef struct {
    uint32_t timestamp_ms;   /* 系统启动后的 ms 数 */
    uint16_t temp_x10;       /* 温度 ×10 */
    uint16_t humi_x10;       /* 湿度 ×10 */
    uint16_t light;          /* 光照原始值 */
    uint16_t _pad;           /* 对齐填充 → 结构体 16 B */
    uint32_t seq;            /* 自增序号，补传时用它确认顺序 */
} sensor_rec_t;

/**
  * @brief  一次性创建所有队列 / 互斥锁 / 任务
  * @note   必须在 osKernelInitialize() 之后、osKernelStart() 之前调用。
  *         **顺序很重要：先建同步对象，再建任务**——任务一跑起来就会用它们。
  * @retval 0  成功
  * @retval -1 有对象创建失败（内存不够 / 配置不对）
  */
int app_tasks_init(void);

/**
  * @brief  app_tasks_init() 的结果（0 成功 / -1 失败）
  * @note   main.c 在 osKernelStart() 前检查它；由 freertos.c 记录。
  */
int app_tasks_init_result(void);

/**
  * @brief 读当前网络状态（线程安全：单字长 volatile 读写，无需加锁）
  */
net_state_t app_net_state(void);

/**
  * @brief  供调试：把一条记录格式化进缓冲区（带长度检查，绝不溢出）
  * @retval 写入的字符数；>=cap 表示被截断
  */
int app_fmt_record(char *buf, unsigned cap, const sensor_rec_t *rec);

#ifdef __cplusplus
}
#endif

#endif /* __APP_TASKS_H__ */