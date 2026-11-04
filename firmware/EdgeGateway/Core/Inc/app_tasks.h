/**
  ******************************************************************************
  * @file    app_tasks.h
  * @brief   任务骨架的对外接口
  *
  *          对外只开三个门：init、查 init 结果、读网络状态。
  *          其余（任务句柄、队列句柄、内部计数）全部 static 收在 app_tasks.c 里。
  ******************************************************************************
  */

#ifndef __APP_TASKS_H__
#define __APP_TASKS_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>   /* offsetof，给下面的 _Static_assert 用 */

/* ==========================================================================
 * 网络状态机（三态）
 *
 * ⚠️ 为什么不能用 bool is_online：
 *    "连着但还在补传历史数据"是一个真实的中间态。
 *    当 bool 处理 → 补传期间按普通节拍发，积压永远清不完；
 *    当离线处理   → 就不该发。所以三态是刚需，不是设计过度。
 *
 * 三态机本身在 net_fsm.h/.c 里（纯逻辑，可在 PC 上单测），
 * 这里只是把它包含进来，避免同一套枚举定义两份。
 * ========================================================================== */
#include "net_fsm.h"

/* ==========================================================================
 * 一条传感记录（W5 落盘格式与本结构对应；注意全整数，不用 float）
 * 25.3℃ → temp_x10 = 253，避免在 MCU 上做浮点格式化
 *
 * 布局（GCC, ARM EABI, 无 packed）：16 B，seq 在偏移 12。
 *   timestamp_ms 4 | temp 2 | humi 2 | light 2 | (隐式填充 2) | seq 4
 * ⚠️ 这里不写 _pad 字段：编译器自己会在 light 后面留 2 B 对齐，
 *    手写 _pad 只会得到同样的 16 B，属于自我安慰，还容易让人误以为靠它保证布局。
 *    真要跨编译器固定布局，得用 _Static_assert(sizeof(...)==16) 卡住，
 *    而不是靠一个填充字段。
 * ========================================================================== */
typedef struct {
    uint32_t timestamp_ms;   /* 系统启动后的 ms 数 */
    uint16_t temp_x10;       /* 温度 ×10 */
    uint16_t humi_x10;       /* 湿度 ×10 */
    uint16_t light;          /* 光照原始值 */
    uint32_t seq;            /* 自增序号，补传时用它确认顺序 */
} sensor_rec_t;

/* 把布局钉死在编译期：改字段/改顺序导致大小或偏移变化，立刻编译失败。
 * 这不是文档，是约束 —— W5 按固定 16 B 落盘，错一个字节整块数据就废了。
 * 断言消息故意用 ASCII：arm-none-eabi-gcc 会把中文断言串转义成 \377... 乱码。*/
_Static_assert(sizeof(sensor_rec_t) == 16U, "sensor_rec_t must be 16 bytes");
_Static_assert(offsetof(sensor_rec_t, seq) == 12U, "sensor_rec_t.seq must be at offset 12");

/**
  * @brief  一次性创建所有队列 / 互斥锁 / 任务
  * @note   必须在 osKernelInitialize() 之后、osKernelStart() 之前调用。
  *         顺序很重要：先建同步对象，再建任务——任务一跑起来就会用它们。
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