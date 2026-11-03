/**
  ******************************************************************************
  * @file    app_config.h
  * @brief   任务架构的全部"数字"集中在这里
  *
  *          优先级 / 栈深度 / 队列深度 / 节拍不散落在各文件里，改一个数只有一处。
  *
  *          ⚠️ 栈深度单位是 word（4 字节），不是 byte —— xTaskCreate 的要求。
  *          ⚠️ FreeRTOS 里优先级数字越大越高（和部分 RTOS 相反）。
  *          ⚠️ 依据是截止期，不是重要性。详见 docs/task-architecture.md §1。
  ******************************************************************************
  */

#ifndef __APP_CONFIG_H__
#define __APP_CONFIG_H__

#ifdef __cplusplus
extern "C" {
#endif

/* ==========================================================================
 * 一、任务优先级
 * 每个都必须有一句"依据"，不能拍脑袋给 Normal。
 * ========================================================================== */
#define PRIO_MODBUS     4U   /* 硬截止期：从站响应窗口 200ms，超时要重发，最紧的一根弦 */
#define PRIO_NET        3U   /* 软截止期：KeepAlive 半周期 30s 内必须发心跳，否则被踢 */
#define PRIO_SENSOR     2U   /* 1s 周期，无硬截止期（数据带时间戳，晚到不影响正确性） */
#define PRIO_STORAGE    2U   /* 与 sensor 同优先级：吞吐型任务，靠时间片轮转公平分享 */
#define PRIO_UI         1U   /* 掉帧无业务后果，人眼容忍度最高 → 给低优先级 */
#define PRIO_MONITOR    0U   /* 纯观测，错过一轮无所谓，任何正事都能插队 */

/* ==========================================================================
 * 二、栈深度（单位：word = 4 字节）
 * ⚠️ 现在是【初值】，故意给宽裕。W5 / W6 / W7 各自填完逻辑后，
 *    用 uxTaskGetStackHighWaterMark() 实测再收紧。
 *    先给小了会 HardFault，很难查；给大了只浪费 RAM。
 * ========================================================================== */
#define STACK_MODBUS    256U   /* 1 KB  ：组帧 + CRC 查表 */
#define STACK_NET       512U   /* 2 KB  ：AT 缓冲 + MQTT 组帧 + printf */
#define STACK_SENSOR    256U   /* 1 KB  */
#define STACK_STORAGE   256U   /* 1 KB  */
#define STACK_UI        768U   /* 3 KB  ：LVGL 最吃栈，W6 填完要复测 */
#define STACK_MONITOR   160U   /* 640 B ：printf 格式化 + 6 个水位查询 */

/* ==========================================================================
 * 三、队列深度
 * 深度 = "你要扛多久的抖动"。用 uxQueueMessagesWaiting 观测实际值再收紧。
 * ========================================================================== */
#define QLEN_STORAGE    16U    /* 1 条/s 产、写一条 Flash ~5ms → 正常不积压；
                                  16 条用于扛 Flash 擦除卡 ~80ms 的抖动 */
#define QLEN_NET        16U    /* 与 storage 对齐，断网补传时从这里取 */
#define QLEN_UI          1U    /* 只要最新值 → 必须配 xQueueOverwrite，天然不阻塞生产者 */
#define QLEN_MODBUS      8U    /* 上位机轮询 + 本地采集，突发最多几条 */

/* ==========================================================================
 * 四、节拍（ms）
 * ========================================================================== */
#define PERIOD_SENSOR_MS     1000U
#define PERIOD_UI_MS           33U   /* ≈30fps */
#define PERIOD_MONITOR_MS   10000U   /* 也是喂狗间隔；IWDG 超时 20s，余量 2 倍 */
#define NET_RECV_TIMEOUT_MS   500U   /* net_task 等队列的超时，也是心跳检查节拍 */

/* ==========================================================================
 * 五、其它
 * ========================================================================== */
#define APP_LOCK_TIMEOUT_MS   100U   /* 拿 uart1_mutex 最多等这么久，拿不到就丢日志 */
#define APP_MQTT_KEEPALIVE_S   60U   /* MQTT 心跳周期（秒） */

#ifdef __cplusplus
}
#endif

#endif /* __APP_CONFIG_H__ */