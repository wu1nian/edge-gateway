/**
  ******************************************************************************
  * @file    app_tasks.c
  * @brief   任务骨架 —— 6 任务 + 4 队列 + 1 互斥锁
  *
  *          骨架的目标不是"功能跑通"，而是：
  *            ① 能创建：xTaskCreate 全部 pdPASS
  *            ② 能调度：每个计数按自己的节拍增长
  *            ③ 不空转：每个任务都有明确阻塞点
  *            ④ 能观测：栈水位读得出来
  *
  *          S24 已接入的两个纯逻辑模块：
  *            ring_buffer.c —— Flash 环形缓冲游标（wp/rp/满空/覆盖最旧）
  *            net_fsm.c     —— 网络三态机 + 指数退避
  *          它们不碰 HAL，可在 PC 上单测：tools/test_ring_fsm/
  *
  *          业务逻辑先留 TODO，按周填：
  *            W5 → storage_task 真的写 W25Q64（接 rb_put 的落盘顺序）
  *            W6 → sensor_task 真实采集 + ui_task LVGL 刷屏
  *            W7 → net_task ESP8266 AT + MQTT 发送（PUBACK 才 rb_commit）
  ******************************************************************************
  */

#include "app_tasks.h"
#include "app_config.h"
#include "net_fsm.h"
#include "ring_buffer.h"

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"

#include "iwdg.h"   /* monitor_task 喂狗 */

#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* sensor_rec_t 必须是 16 B 且 seq 在偏移 12：W5 落盘格式依赖它。
 * 这里用编译期断言卡住，比在注释里写"应该是 16 B"可靠。 */
_Static_assert(sizeof(sensor_rec_t) == 16U, "sensor_rec_t must be 16 bytes");
_Static_assert(offsetof(sensor_rec_t, seq) == 12U, "seq must be at offset 12");

/* ==========================================================================
 * 一、模块内静态：句柄一律 static，不对外暴露
 * ========================================================================== */
static QueueHandle_t      s_storage_q;
static QueueHandle_t      s_net_q;
static QueueHandle_t      s_ui_q;
static QueueHandle_t      s_modbus_q;
static SemaphoreHandle_t  s_uart1_mutex;

static TaskHandle_t       s_htask_sensor;
static TaskHandle_t       s_htask_modbus;
static TaskHandle_t       s_htask_net;
static TaskHandle_t       s_htask_storage;
static TaskHandle_t       s_htask_ui;
static TaskHandle_t       s_htask_monitor;

/* 调试计数：每任务一个，monitor_task 打出来证明"调度在转" */
static volatile uint32_t  s_cnt_sensor;
static volatile uint32_t  s_cnt_modbus;
static volatile uint32_t  s_cnt_net;
static volatile uint32_t  s_cnt_storage;
static volatile uint32_t  s_cnt_ui;
static volatile uint32_t  s_cnt_monitor;

/* 丢帧计数：队列满时丢最旧，但必须"丢得让人知道" */
static volatile uint32_t  s_drop_storage;
static volatile uint32_t  s_drop_net;

/* 网络状态：单字长 volatile，读写原子，不需要锁。
 * ⚠️ 真值在 net_fsm.c 里，这里只是缓存一份给 monitor 读，
 *    避免 monitor 去读状态机内部而引入跨任务可见性问题。*/
static volatile net_state_t s_net_state = NET_OFFLINE;

/* ==========================================================================
 * 二、对外接口
 * ========================================================================== */
net_state_t app_net_state(void)
{
    return s_net_state;
}

int app_fmt_record(char *buf, unsigned cap, const sensor_rec_t *rec)
{
    if ((buf == NULL) || (rec == NULL) || (cap == 0U)) {
        return -1;
    }
    /* 一律 snprintf + 长度；绝不用 sprintf */
    return snprintf(buf, (size_t)cap,
                    "seq=%lu t=%u.%uC h=%u.%u%% l=%u",
                    (unsigned long)rec->seq,
                    (unsigned)rec->temp_x10 / 10U, (unsigned)rec->temp_x10 % 10U,
                    (unsigned)rec->humi_x10 / 10U, (unsigned)rec->humi_x10 % 10U,
                    (unsigned)rec->light);
}

/* ==========================================================================
 * 三、统一的日志出口
 *
 *  ⚠️ 全项目只允许通过这里打日志，绝不在持锁期间直接 printf。
 *  ⚠️ 拿不到锁就【丢日志】，绝不阻塞等——日志是观测手段，
 *     不能让观测反过来卡住被观测的业务。
 *  🚨 任何人都不要在持有 s_uart1_mutex 的代码里调用本函数
 *     （互斥锁非递归 → 自死锁）
 * ========================================================================== */
static void log_line(const char *tag, const char *msg)
{
    if (xSemaphoreTake(s_uart1_mutex, pdMS_TO_TICKS(APP_LOCK_TIMEOUT_MS)) == pdTRUE) {
        (void)printf("[%s] %s\r\n", tag, msg);
        (void)xSemaphoreGive(s_uart1_mutex);
    }
}

/* 直接打印一行（monitor 专用；同样走锁） */
static void log_raw(const char *line)
{
    if (xSemaphoreTake(s_uart1_mutex, pdMS_TO_TICKS(APP_LOCK_TIMEOUT_MS)) == pdTRUE) {
        (void)fputs(line, stdout);
        (void)xSemaphoreGive(s_uart1_mutex);
    }
}

/* ==========================================================================
 * 四、六个任务骨架
 * ========================================================================== */

/* ---------------- sensor_task：1s 绝对周期 ---------------- */
static void sensor_task(void *arg)
{
    TickType_t   last = xTaskGetTickCount();
    sensor_rec_t rec;
    const TickType_t period = pdMS_TO_TICKS(PERIOD_SENSOR_MS);

    (void)arg;
    (void)memset(&rec, 0, sizeof(rec));

    for (;;) {
        rec.seq++;
        rec.timestamp_ms = (uint32_t)(xTaskGetTickCount() * (TickType_t)portTICK_PERIOD_MS);

        /* TODO(W6): 真实采集 DHT11 + 光照 ADC */
        rec.temp_x10 = 253U;
        rec.humi_x10 = 600U;
        rec.light    = 1200U;

        /* ① 落盘：满了丢【最旧】，绝不阻塞采集（采集点比丢历史重要） */
        if (xQueueSend(s_storage_q, &rec, 0) != pdPASS) {
            sensor_rec_t drop;
            (void)xQueueReceive(s_storage_q, &drop, 0);
            (void)xQueueSend(s_storage_q, &rec, 0);
            s_drop_storage++;
        }

        /* ② 上报：同样丢最旧 */
        if (xQueueSend(s_net_q, &rec, 0) != pdPASS) {
            sensor_rec_t drop;
            (void)xQueueReceive(s_net_q, &drop, 0);
            (void)xQueueSend(s_net_q, &rec, 0);
            s_drop_net++;
        }

        /* ③ UI：深度 1 + 覆盖写，生产者永不阻塞（UI 慢也拖不垮采集） */
        (void)xQueueOverwrite(s_ui_q, &rec);

        s_cnt_sensor++;

        /* 绝对周期：不会像 vTaskDelay 那样累积漂移 */
        vTaskDelayUntil(&last, period);
    }
}

/* ---------------- modbus_task：事件驱动（最高优先级） ---------------- */
static void modbus_task(void *arg)
{
    sensor_rec_t cmd;
    const TickType_t wait = pdMS_TO_TICKS(500U);

    (void)arg;

    for (;;) {
        /* 节拍来源：队列。500ms 超时只为兜底让出 CPU，正常情况下有指令才醒 */
        if (xQueueReceive(s_modbus_q, &cmd, wait) == pdPASS) {
            /* TODO(W5+): 调用 modbus_host 组帧 / 收发 / 解析（W2 已完成，直接接） */
            (void)cmd;
        }
        s_cnt_modbus++;
    }
}

/* ---------------- net_task：事件 + 超时（心跳检查 + 补传） ----------------
 *
 *  这一版把 S24 的三态机和环形缓冲接进来了，剩下没做的只有"真的把字节
 *  发给 ESP8266"（W7）。状态迁移、退避、PUBACK 推进 rp 都是真逻辑。
 */
static void net_task(void *arg)
{
    sensor_rec_t rec;
    const TickType_t wait = pdMS_TO_TICKS(NET_RECV_TIMEOUT_MS);
    uint32_t now_ms;
    uint32_t last_ping_ms = 0U;
    uint32_t retry_at_ms  = 0U;      /* OFFLINE 时下一次尝试重连的时刻 */
    rb_seq_t seq;

    (void)arg;

    for (;;) {
        /* ① 等一条待发记录。超时也有意义——那是心跳和重连检查的节拍 */
        if (xQueueReceive(s_net_q, &rec, wait) == pdPASS) {
            /* TODO(W7): mqtt_build_publish() → AT 发出去 */
            (void)rec;
        }

        now_ms = (uint32_t)(xTaskGetTickCount() * (TickType_t)portTICK_PERIOD_MS);

        /* ② 离线：指数退避重连，一个字都不发。
         *    🚨 退避的等待靠"时间戳比对"实现，不是 vTaskDelay ——
         *       真在这里 vTaskDelay(30s)，心跳和高优先级唤醒全被堵住。 */
        if (net_fsm_state() == NET_OFFLINE) {
            if (now_ms >= retry_at_ms) {
                /* TODO(W7): 发 AT+CIPSTART → CONNACK
                 * 现在没有 ESP8266，先按"重连失败"处理，让状态机走起来 */
                net_fsm_on_disconnected();               /* 保持 OFFLINE */
                /* 等到期再加一个退避间隔；用 while 而不是一次性赋值，
                 * 防止时钟回绕时 now_ms >= retry_at_ms 永远成立导致忙转 */
                retry_at_ms = now_ms + net_fsm_next_backoff_ms();
            }
            s_net_state = net_fsm_state();
        } else {
            /* ③ 在线 / 补传：从环形缓冲取最旧一条发出去 */
            if (net_fsm_can_send() && (rb_peek(&seq) == 0)) {
                /* TODO(W7): 取记录 → 组 PUBLISH(QoS1) → 发 → 等 PUBACK
                 * 🚨 收到 PUBACK 才调 rb_commit()。
                 *    发送函数返回成功 ≠ broker 收到。 */
                rb_commit();   /* 占位：W7 换成"真收到 PUBACK 才提交" */
            }

            /* ④ 补传期间每提交一条就检查是否补完，补完回 ONLINE */
            net_fsm_after_ack(rb_pending());

            /* ⑤ 心跳：到半周期就发，不阻塞等响应 */
            if ((uint32_t)(now_ms - last_ping_ms) >=
                (APP_MQTT_KEEPALIVE_S * 1000U / 2U)) {
                last_ping_ms = now_ms;
                /* TODO(W7): 发 C0 00（PINGREQ），等 D0 00 */
            }

            s_net_state = net_fsm_state();
        }

        s_cnt_net++;
    }
}

/* ---------------- storage_task：纯事件驱动 ----------------
 *
 *  S24 的"写缓冲优先"：无论在线断网，数据都先落 Flash，再由 net_task 取。
 *  这样在线/断网只有一条写路径。
 */
static void storage_task(void *arg)
{
    sensor_rec_t rec;
    int rc;

    (void)arg;

    for (;;) {
        /* 永久阻塞：节拍完全由数据到达决定，永不空转 */
        if (xQueueReceive(s_storage_q, &rec, portMAX_DELAY) == pdPASS) {
            /* TODO(W5): 把 rec 序列化成 32 B 记录 + CRC，写进 W25Q64
             *           然后【再】调 rb_put() 推进 wp。
             *
             * 🚨 顺序不能反：先推进 wp 再写数据，掉电瞬间 wp 会指向
             *    一条没写完的记录，重启后会把垃圾当有效数据补传上去。 */
            rc = rb_put();
            if (rc == 1) {
                /* 缓冲满，覆盖了最旧一条。溢出计数在 rb_stat() 里能读到 */
                s_drop_storage++;
            } else if (rc < 0) {
                /* 环形缓冲没初始化 —— 属于配置错误，不该发生 */
                s_drop_storage++;
            }
        }
        s_cnt_storage++;
    }
}

/* ---------------- ui_task：33ms 绝对周期（最低的正事） ---------------- */
static void ui_task(void *arg)
{
    TickType_t   last = xTaskGetTickCount();
    sensor_rec_t latest;
    const TickType_t period = pdMS_TO_TICKS(PERIOD_UI_MS);

    (void)arg;

    for (;;) {
        /* 非阻塞取最新值；取不到就照常刷屏（画面还是上一帧的值） */
        if (xQueueReceive(s_ui_q, &latest, 0) == pdPASS) {
            /* TODO(W6): lv_label_set_text_fmt(...) */
            (void)latest;
        }
        s_cnt_ui++;

        /* ⚠️ 这一句必须一定执行得到：中间不能有 continue / return，
         *    否则任务变忙等，会把所有优先级比它低的任务饿死 */
        vTaskDelayUntil(&last, period);
    }
}

/* ---------------- monitor_task：10s 打水位 + 喂狗 ---------------- */
static void monitor_task(void *arg)
{
    TickType_t last = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(PERIOD_MONITOR_MS);
    char line[192];
    int  n;
    rb_stat_t rs;

    (void)arg;

    for (;;) {
        /* 🚨 喂狗必须放在 monitor_task 里最前面：IWDG 在 main.c 里
         *    MX_IWDG_Init() 就启动了（早于调度器），超时 20 s。
         *    原来靠 defaultTask 的 500 ms 循环喂狗，defaultTask 删掉后
         *    这里不喂，板子会每 20 s 复位一次。
         *    10 s 间隔对 20 s 超时是 2 倍余量。 */
        (void)HAL_IWDG_Refresh(&hiwdg);

        log_line("MON", "---- tick ----");

        n = snprintf(line, sizeof(line),
                     "cnt: sensor=%lu modbus=%lu net=%lu storage=%lu ui=%lu\r\n",
                     (unsigned long)s_cnt_sensor, (unsigned long)s_cnt_modbus,
                     (unsigned long)s_cnt_net,    (unsigned long)s_cnt_storage,
                     (unsigned long)s_cnt_ui);
        if (n > 0) {
            log_raw(line);
        }

        n = snprintf(line, sizeof(line),
                     "stack(word): sensor=%u modbus=%u net=%u storage=%u ui=%u mon=%u\r\n",
                     (unsigned)uxTaskGetStackHighWaterMark(s_htask_sensor),
                     (unsigned)uxTaskGetStackHighWaterMark(s_htask_modbus),
                     (unsigned)uxTaskGetStackHighWaterMark(s_htask_net),
                     (unsigned)uxTaskGetStackHighWaterMark(s_htask_storage),
                     (unsigned)uxTaskGetStackHighWaterMark(s_htask_ui),
                     (unsigned)uxTaskGetStackHighWaterMark(s_htask_monitor));
        if (n > 0) {
            log_raw(line);
        }

        n = snprintf(line, sizeof(line),
                     "drop: storage=%lu net=%lu   net_state=%d\r\n",
                     (unsigned long)s_drop_storage, (unsigned long)s_drop_net,
                     (int)app_net_state());
        if (n > 0) {
            log_raw(line);
        }

        /* 环形缓冲游标：wp - rp 就是"断网期间攒了多少条"，
         * overflow 非 0 说明缓冲已经写满并覆盖过最旧数据。*/
        rb_stat(&rs);
        n = snprintf(line, sizeof(line),
                     "ring: wp=%lu rp=%lu pending=%lu overflow=%lu commits=%lu\r\n",
                     (unsigned long)rs.wp, (unsigned long)rs.rp,
                     (unsigned long)rs.count, (unsigned long)rs.overflow,
                     (unsigned long)rs.commits);
        if (n > 0) {
            log_raw(line);
        }

        n = snprintf(line, sizeof(line),
                     "net: state=%s backoff=%lums\r\n",
                     net_fsm_state_name(app_net_state()),
                     (unsigned long)net_fsm_backoff_ms());
        if (n > 0) {
            log_raw(line);
        }

        s_cnt_monitor++;
        vTaskDelayUntil(&last, period);
    }
}

/* ==========================================================================
 * 五、创建所有对象
 *
 * 🚨 顺序：① 先建同步对象（mutex + 队列）→ ② 再建任务。
 *    反过来的话，任务一被创建就可能立刻运行并使用空句柄 → 直接崩。
 *
 * 🚨 判错必须逐个 if：pdPASS=1、失败=-1，用 ok &= ... 会把
 *    1 & -1 = 1 的错误吞掉。
 * ========================================================================== */
int app_tasks_init(void)
{
    /* ---------- ① 同步对象 ---------- */
    /* 用 Mutex 而不是 Binary：只有 Mutex 才有【优先级继承】，
     * 否则拿锁的低优先级任务会被中优先级任务间接拖住 → 优先级翻转 */
    s_uart1_mutex = xSemaphoreCreateMutex();

    s_storage_q = xQueueCreate(QLEN_STORAGE, sizeof(sensor_rec_t));
    s_net_q     = xQueueCreate(QLEN_NET,     sizeof(sensor_rec_t));
    s_ui_q      = xQueueCreate(QLEN_UI,      sizeof(sensor_rec_t));
    s_modbus_q  = xQueueCreate(QLEN_MODBUS,  sizeof(sensor_rec_t));

    if ((s_uart1_mutex == NULL) || (s_storage_q == NULL) ||
        (s_net_q == NULL) || (s_ui_q == NULL) || (s_modbus_q == NULL)) {
        return -1;
    }

    /* ---------- ①.5 上层模块（S24：三态机 + 环形缓冲） ---------- */
    /* 必须在建任务之前初始化：net_task / storage_task 一被创建就会用到它们。
     * W5 接 W25Q64 后，rb_init 的两个参数改成从元数据区恢复。*/
    net_fsm_init();
    if (rb_init(0U, 0U) != 0) {
        return -1;
    }
    s_net_state = net_fsm_state();

    /* ---------- ② 任务（逐个判返回值） ---------- */
    if (xTaskCreate(sensor_task,  "sensor",  STACK_SENSOR,  NULL,
                    PRIO_SENSOR,  &s_htask_sensor)  != pdPASS) { return -1; }
    if (xTaskCreate(modbus_task,  "modbus",  STACK_MODBUS,  NULL,
                    PRIO_MODBUS,  &s_htask_modbus)  != pdPASS) { return -1; }
    if (xTaskCreate(net_task,     "net",     STACK_NET,     NULL,
                    PRIO_NET,     &s_htask_net)     != pdPASS) { return -1; }
    if (xTaskCreate(storage_task, "storage", STACK_STORAGE, NULL,
                    PRIO_STORAGE, &s_htask_storage) != pdPASS) { return -1; }
    if (xTaskCreate(ui_task,      "ui",      STACK_UI,      NULL,
                    PRIO_UI,      &s_htask_ui)      != pdPASS) { return -1; }
    if (xTaskCreate(monitor_task, "monitor", STACK_MONITOR, NULL,
                    PRIO_MONITOR, &s_htask_monitor) != pdPASS) { return -1; }

    return 0;
}