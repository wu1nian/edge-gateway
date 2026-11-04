/**
  ******************************************************************************
  * @file    test_ring_fsm.c
  * @brief   W4 纯逻辑模块的 PC 单元测试
  *
  *          被测模块（都不碰 HAL / FreeRTOS，能在 Windows 上用 gcc 直接编）：
  *            ring_buffer.c —— 环形缓冲游标
  *            net_fsm.c     —— 网络三态机 + 指数退避
  *            rec_codec.c   —— 32 B Flash 记录序列化
  *            puback.c      —— PUBACK 解析
  *            net_tx.c      —— 窗口=1 的发送编排
  *
  *          和 W2 的 Modbus、W3 的 MQTT 一个套路：测的是固件里那份真 .c，
  *          不是抄一份到测试目录。
  *
  *          编译：  build.bat
  *          运行：  test_ring_fsm.exe
  ******************************************************************************
  */

#include "ring_buffer.h"
#include "net_fsm.h"
#include "rec_codec.h"
#include "puback.h"
#include "net_tx.h"

#include <stdio.h>
#include <string.h>

static int g_pass;
static int g_fail;

#define CHECK(cond, msg)                                                  \
    do {                                                                  \
        if (cond) {                                                       \
            g_pass++;                                                     \
            printf("  PASS  %s\n", msg);                                  \
        } else {                                                          \
            g_fail++;                                                     \
            printf("  FAIL  %s   (%s:%d)\n", msg, __FILE__, __LINE__);    \
        }                                                                 \
    } while (0)

/* ============================ 环形缓冲 ============================ */

static void test_ring_basic(void)
{
    printf("[ring] 空 / 写一条 / 提交\n");

    CHECK(rb_init(0U, 0U) == 0, "init(0,0) 返回 0");
    CHECK(rb_is_empty() == 1, "初始为空");
    CHECK(rb_pending() == 0U, "初始待发 0 条");
    CHECK(rb_is_full() == 0, "初始不满");

    CHECK(rb_put() == 0, "put 第 1 条返回 OK");
    CHECK(rb_pending() == 1U, "待发变 1 条");
    CHECK(rb_is_empty() == 0, "不再为空");

    CHECK(rb_commit() == 0, "commit 返回 0");
    CHECK(rb_pending() == 0U, "提交后待发归 0");
    CHECK(rb_is_empty() == 1, "提交后回到空");
}

static void test_ring_peek_does_not_advance(void)
{
    rb_seq_t seq = 0xDEADBEEF;

    printf("[ring] peek 不能推进 rp\n");

    (void)rb_init(0U, 0U);
    (void)rb_put();
    (void)rb_put();

    CHECK(rb_peek(&seq) == 0, "peek 成功");
    CHECK(rb_pending() == 2U, "peek 之后待发仍是 2 条");

    /* 取两次必须拿到同一个序号：peek 是只读的 */
    {
        rb_seq_t again = 0U;
        (void)rb_peek(&again);
        CHECK(again == seq, "连续两次 peek 拿到同一序号");
    }
}

static void test_ring_empty_commit(void)
{
    printf("[ring] 空缓冲提交必须失败\n");

    (void)rb_init(0U, 0U);
    CHECK(rb_commit() == -1, "空缓冲 commit 返回 -1");
    CHECK(rb_pending() == 0U, "失败提交不影响待发数");

    {
        rb_seq_t seq = 0U;
        CHECK(rb_peek(&seq) == -1, "空缓冲 peek 返回 -1");
    }
}

static void test_ring_full_and_overwrite(void)
{
    uint32_t i;
    int rc = 0;

    printf("[ring] 满判定 + 覆盖最旧\n");

    (void)rb_init(0U, 0U);

    /* 容量 N 只能装 N-1 条（留一格区分空满） */
    for (i = 0U; i < (RB_CAPACITY - 1U); i++) {
        rc = rb_put();
        if (rc != 0) {
            break;
        }
    }
    CHECK(rc == 0, "写到 N-1 条全是 OK");
    CHECK(rb_pending() == (RB_CAPACITY - 1U), "待发 = N-1");
    CHECK(rb_is_full() == 1, "此刻判定为满");

    /* 再写一条：覆盖最旧，返回 1 */
    rc = rb_put();
    CHECK(rc == 1, "第 N 条返回 OVERWRITE(1)");

    /* 覆盖时 rp 和 wp 一起前进一格，所以待发数【不变】——依然是 N-1。
     * 这正是"丢弃最旧"的语义：总量没有变多，只是最老那条被挤掉了。*/
    CHECK(rb_pending() == (RB_CAPACITY - 1U), "覆盖后待发仍为 N-1（总量不变）");

    {
        rb_stat_t st;
        rb_stat(&st);
        CHECK(st.overflow == 1U, "overflow 计到 1");
        /* 序号也验证一下：起点是 0，写满 N-1 条后 wp=4095、rp=0；
         * 覆盖一次后 rp 前进到 1，wp 前进到 4096 → 对 4096 取模回绕成 0。
         * 待发数 = wp - rp 用无符号减法，0 - 1 自然回绕成 4095。*/
        CHECK(st.rp == 1U, "覆盖后 rp 前进一格（最旧被丢）");
        CHECK(st.wp == 0U, "覆盖后 wp 绕回 0（4095 + 1 对 N 取模）");
        CHECK(st.count == (RB_CAPACITY - 1U), "回绕后 count 用无符号减法算，结果正确");
    }
}

static void test_ring_wraparound(void)
{
    uint32_t i;
    rb_stat_t st;

    printf("[ring] 游标回绕\n");

    /* 从接近 N 的位置起步，逼游标绕过 0 */
    (void)rb_init(RB_CAPACITY + RB_CAPACITY - 3U, RB_CAPACITY + RB_CAPACITY - 3U);

    for (i = 0U; i < 10U; i++) {
        (void)rb_put();
        (void)rb_commit();
    }
    rb_stat(&st);

    CHECK(st.count == 0U, "绕一圈之后待发仍为 0");
    CHECK(st.commits == 10U, "commits 记为 10");
    CHECK(rb_is_empty() == 1, "回绕后判空正确");
}

static void test_ring_offset(void)
{
    printf("[ring] 序号 → 字节偏移\n");

    CHECK(rb_seq_to_offset(0U) == 0U, "seq 0 → offset 0");
    CHECK(rb_seq_to_offset(1U) == RB_RECORD_BYTES, "seq 1 → 32 B");
    CHECK(rb_seq_to_offset(RB_CAPACITY) == 0U, "绕回容量处 → 0");
    CHECK(rb_seq_to_offset(RB_CAPACITY + 1U) == RB_RECORD_BYTES,
          "绕回后继续按记录步进");
}

/* ============================ 网络三态机 ============================ */

static void test_fsm_paths(void)
{
    printf("[fsm] 三条路径\n");

    net_fsm_init();
    CHECK(net_fsm_state() == NET_OFFLINE, "初始 OFFLINE");
    CHECK(net_fsm_can_send() == 0, "OFFLINE 不许发");

    /* 路径 A：连上且无积压 → ONLINE */
    net_fsm_on_connected(0U);
    CHECK(net_fsm_state() == NET_ONLINE, "无积压 → ONLINE");
    CHECK(net_fsm_can_send() == 1, "ONLINE 可以发");

    /* 路径 B：断开 → OFFLINE */
    net_fsm_on_disconnected();
    CHECK(net_fsm_state() == NET_OFFLINE, "断开 → OFFLINE");
    CHECK(net_fsm_can_send() == 0, "断开后不许发");

    /* 路径 C：连上但有积压 → BACKFILL */
    net_fsm_on_connected(1800U);
    CHECK(net_fsm_state() == NET_BACKFILL, "有积压 → BACKFILL");
    CHECK(net_fsm_can_send() == 1, "BACKFILL 也可以发");

    /* 补过程中没补完 */
    net_fsm_after_ack(500U);
    CHECK(net_fsm_state() == NET_BACKFILL, "没补完仍是 BACKFILL");

    /* 补完 → 回 ONLINE */
    net_fsm_after_ack(0U);
    CHECK(net_fsm_state() == NET_ONLINE, "补完 → ONLINE");
}

static void test_fsm_after_ack_ignores_online(void)
{
    printf("[fsm] ONLINE 下 after_ack 不应改动状态\n");

    net_fsm_init();
    net_fsm_on_connected(0U);
    CHECK(net_fsm_state() == NET_ONLINE, "先到 ONLINE");

    net_fsm_after_ack(5U);   /* ONLINE 状态下积压忽然不为 0 */
    CHECK(net_fsm_state() == NET_ONLINE,
          "ONLINE 不因 after_ack 变 BACKFILL（积压检查在连接时做）");
}

static void test_fsm_backoff(void)
{
    uint32_t b1, b2, b3, b4, b5, b6, b7;

    printf("[fsm] 指数退避 1→2→4→8→16→30 封顶\n");

    net_fsm_init();
    CHECK(net_fsm_backoff_ms() == 1000U, "初始 1s");

    b1 = net_fsm_next_backoff_ms();
    b2 = net_fsm_next_backoff_ms();
    b3 = net_fsm_next_backoff_ms();
    b4 = net_fsm_next_backoff_ms();
    b5 = net_fsm_next_backoff_ms();
    b6 = net_fsm_next_backoff_ms();
    b7 = net_fsm_next_backoff_ms();

    CHECK(b1 == 1000U,   "第 1 次 1000ms");
    CHECK(b2 == 2000U,   "第 2 次 2000ms");
    CHECK(b3 == 4000U,   "第 3 次 4000ms");
    CHECK(b4 == 8000U,   "第 4 次 8000ms");
    CHECK(b5 == 16000U,  "第 5 次 16000ms");
    CHECK(b6 == 30000U || b6 == 32000U - 2000U, "第 6 次到达封顶");
    CHECK(b7 == 30000U,  "第 7 次仍在 30000ms 封顶");
    CHECK(net_fsm_backoff_ms() == 30000U, "封顶后不再增长");
}

static void test_fsm_backoff_reset(void)
{
    printf("[fsm] 连上后退避复位\n");

    net_fsm_init();
    (void)net_fsm_next_backoff_ms();
    (void)net_fsm_next_backoff_ms();
    CHECK(net_fsm_backoff_ms() == 4000U, "退避已涨到 4s");

    net_fsm_on_connected(0U);
    CHECK(net_fsm_backoff_ms() == 1000U, "连接成功后复位成 1s");
}

static void test_fsm_names(void)
{
    printf("[fsm] 状态名\n");

    CHECK(strcmp(net_fsm_state_name(NET_OFFLINE), "OFFLINE") == 0, "OFFLINE 名字");
    CHECK(strcmp(net_fsm_state_name(NET_ONLINE), "ONLINE") == 0, "ONLINE 名字");
    CHECK(strcmp(net_fsm_state_name(NET_BACKFILL), "BACKFILL") == 0, "BACKFILL 名字");
    CHECK(strcmp(net_fsm_state_name((net_state_t)99), "?") == 0, "非法值返回 ?");
}

/* ============================ 联调场景 ============================ */

static void test_scenario_offline_30min(void)
{
    uint32_t i;
    rb_stat_t st;

    printf("[场景] 断网 30 分钟攒 1800 条，重连后补传\n");

    (void)rb_init(0U, 0U);
    net_fsm_init();

    /* 30 分钟 × 60 条/分钟 = 1800 条，全部写进环形缓冲 */
    for (i = 0U; i < 1800U; i++) {
        (void)rb_put();
    }
    rb_stat(&st);
    CHECK(st.count == 1800U, "攒下 1800 条");
    CHECK(st.overflow == 0U, "容量 4096，没溢出");

    /* 断网期间 OFFLINE，不许发 */
    net_fsm_on_disconnected();
    CHECK(net_fsm_can_send() == 0, "断网期间不许发");

    /* 重连：有积压 → BACKFILL */
    net_fsm_on_connected(rb_pending());
    CHECK(net_fsm_state() == NET_BACKFILL, "重连进 BACKFILL");

    /* 逐条发、逐条收 PUBACK，rp 才推进（窗口 = 1） */
    for (i = 0U; i < 1800U; i++) {
        (void)rb_peek(&(rb_seq_t){0});
        (void)rb_commit();
        net_fsm_after_ack(rb_pending());
    }

    rb_stat(&st);
    CHECK(st.count == 0U, "补完积压清零");
    CHECK(st.commits == 1800U, "commits = 1800");
    CHECK(net_fsm_state() == NET_ONLINE, "补完回到 ONLINE");
}

static void test_scenario_disconnect_mid_backfill(void)
{
    uint32_t i;
    rb_stat_t st;

    printf("[场景] 补传中途再断网，rp 停在原处\n");

    (void)rb_init(0U, 0U);
    net_fsm_init();

    for (i = 0U; i < 100U; i++) {
        (void)rb_put();
    }

    net_fsm_on_connected(rb_pending());
    CHECK(net_fsm_state() == NET_BACKFILL, "进 BACKFILL");

    /* 只补 40 条就断了 */
    for (i = 0U; i < 40U; i++) {
        (void)rb_commit();
    }
    net_fsm_on_disconnected();

    rb_stat(&st);
    CHECK(st.count == 60U, "剩下 60 条没发");
    CHECK(net_fsm_state() == NET_OFFLINE, "状态回 OFFLINE");

    /* 再连上：仍有积压 → 还是 BACKFILL，接着补剩下的 */
    net_fsm_on_connected(rb_pending());
    CHECK(net_fsm_state() == NET_BACKFILL, "重连仍是 BACKFILL");

    for (i = 0U; i < 60U; i++) {
        (void)rb_commit();
        net_fsm_after_ack(rb_pending());
    }
    CHECK(net_fsm_state() == NET_ONLINE, "补完回 ONLINE");
    CHECK(rb_pending() == 0U, "一条不剩");
}

/* ============================ rec_codec ============================ */

static void test_rec_codec_roundtrip(void)
{
    sensor_rec_t src;
    sensor_rec_t dst;
    uint8_t buf[RB_RECORD_BYTES];
    int i;

    printf("[rec_codec] 编码-解码往返，边界值不许失真\n");

    src.timestamp_ms = 0xDEADBEEFU;
    src.temp_x10     = 0xFFFFU;   /* 负数温度也走这里（-0.1℃ = 0xFFFF） */
    src.humi_x10     = 0x8000U;
    src.light        = 0x0001U;
    src.seq          = 0xA5C3F00DU;   /* 高低 16 位都非 0，能抓出拆位写反 */

    memset(buf, 0xAA, sizeof(buf));
    CHECK(rec_encode(&src, buf) == 0, "encode 返回 0");
    CHECK(rec_decode(buf, &dst) == 0, "decode 返回 0");

    CHECK(dst.timestamp_ms == src.timestamp_ms, "timestamp 往返一致");
    CHECK(dst.temp_x10 == src.temp_x10, "temp 往返一致（0xFFFF）");
    CHECK(dst.humi_x10 == src.humi_x10, "humi 往返一致（0x8000）");
    CHECK(dst.light == src.light, "light 往返一致");
    CHECK(dst.seq == src.seq, "seq 往返一致（大端拆位正确）");

    /* 预留区必须被清零，不能把调用方栈上的垃圾写进 Flash */
    for (i = 0; i < (int)RB_RECORD_BYTES; i++) {
        if ((i >= (int)REC_OFF_RSV_A) && (i < (int)REC_OFF_RSV_B)) {
            CHECK(buf[i] == 0x00U, "预留区 A 被清零");
            break;
        }
    }
}

static void test_rec_codec_layout(void)
{
    sensor_rec_t src;
    uint8_t buf[RB_RECORD_BYTES];

    printf("[rec_codec] 字节偏移是契约，不能被编译器改\n");

    src.timestamp_ms = 0x11223344U;
    src.temp_x10     = 0x5566U;
    src.humi_x10     = 0x7788U;
    src.light        = 0x99AAU;
    src.seq          = 0xBBCCDDEEU;
    (void)rec_encode(&src, buf);

    /* 全部小端，手写死偏移。这些数字就是 W5 读 Flash 时的假设。*/
    CHECK(buf[0] == 0x44U && buf[3] == 0x11U, "timestamp 小端在偏移 0");
    CHECK(buf[4] == 0x66U && buf[5] == 0x55U, "temp 小端在偏移 4");
    CHECK(buf[6] == 0x88U && buf[7] == 0x77U, "humi 小端在偏移 6");
    CHECK(buf[8] == 0xAAU && buf[9] == 0x99U, "light 小端在偏移 8");
    CHECK(buf[10] == 0xCCU && buf[11] == 0xBBU, "seq 高 16 位小端在偏移 10");
    CHECK(buf[30] == 0xEEU && buf[31] == 0xDDU, "seq 低 16 位在偏移 30");
    CHECK(buf[12] == 0x52U && buf[15] == 0x47U, "magic 小端在偏移 12");
}

static void test_rec_codec_blank_and_corrupt(void)
{
    sensor_rec_t src;
    sensor_rec_t dst;
    uint8_t buf[RB_RECORD_BYTES];
    uint8_t blank[RB_RECORD_BYTES];

    printf("[rec_codec] 空槽 / 坏块必须能分开\n");

    /* 新芯片擦除后全是 0xFF */
    memset(blank, 0xFF, sizeof(blank));
    CHECK(rec_is_blank(blank) == 1, "0xFF 区判为空槽");
    CHECK(rec_decode(blank, &dst) == -2, "空槽返回 -2（不是坏块）");

    src.timestamp_ms = 1U; src.temp_x10 = 2U; src.humi_x10 = 3U;
    src.light = 4U; src.seq = 5U;
    (void)rec_encode(&src, buf);
    CHECK(rec_is_blank(buf) == 0, "编好的记录不判为空");

    /* 翻一个数据位：magic 还在，CRC 必须发现 */
    buf[REC_OFF_TEMP] ^= 0x01U;
    CHECK(rec_decode(buf, &dst) == -3, "单 bit 翻转被 CRC 抓到，返回 -3");

    /* 参数非法 */
    CHECK(rec_encode(NULL, buf) == -1, "encode(NULL) 返回 -1");
    CHECK(rec_decode(buf, NULL) == -1, "decode(NULL) 返回 -1");
}

/* ============================ puback ============================ */

static void test_puback_parse(void)
{
    const uint8_t good[]   = { 0x40U, 0x02U, 0x00U, 0x07U };
    const uint8_t other[]  = { 0x40U, 0x02U, 0x00U, 0x08U };
    const uint8_t notpub[] = { 0x30U, 0x02U, 0x00U, 0x07U };
    const uint8_t badrl[]  = { 0x40U, 0x03U, 0x00U, 0x07U };
    uint16_t used = 0U;

    printf("[puback] 40 02 xx xx 的解析\n");

    CHECK(puback_parse(good, 4U, 7U, &used) == PUBACK_OK, "完整帧 + id 匹配 → OK");
    CHECK(used == 4U, "used = 4");

    CHECK(puback_parse(other, 4U, 7U, &used) == PUBACK_ID_MISMATCH,
          "id 对不上 → ID_MISMATCH");
    CHECK(puback_parse(notpub, 4U, 7U, &used) == PUBACK_NOT_FOUND,
          "首字节 0x30 → NOT_FOUND");
    CHECK(puback_parse(badrl, 4U, 7U, &used) == PUBACK_NOT_FOUND,
          "RL 不是 2 → NOT_FOUND");

    /* 半包：需要继续收，不是错误 */
    CHECK(puback_parse(good, 0U, 7U, &used) == PUBACK_INCOMPLETE, "0 字节 → INCOMPLETE");
    CHECK(puback_parse(good, 1U, 7U, &used) == PUBACK_INCOMPLETE, "只有首字节 → INCOMPLETE");
    CHECK(puback_parse(good, 3U, 7U, &used) == PUBACK_INCOMPLETE, "差一个字节 → INCOMPLETE");

    /* 粘包：前面还有别的字节时，调用方要先切帧，本函数只看 buf[0] */
    CHECK(puback_result_name(PUBACK_OK) != NULL, "结果码有人话");
}

/* ============================ puback 累积器 ============================ */

static void test_puback_acc(void)
{
    puback_acc_t acc;
    const uint8_t noise[] = { 'O', 'K', '\r', '\n', '>', ' ' };
    uint8_t i;
    puback_result_t r = PUBACK_NOT_FOUND;

    printf("[puback_acc] 字节流里挑出 PUBACK，前面的垃圾不影响\n");

    /* 喂垃圾：每个字节都判 NOT_FOUND，累积器一直是空的 */
    puback_acc_reset(&acc);
    for (i = 0U; i < (uint8_t)sizeof(noise); i++) {
        r = puback_acc_feed(&acc, noise[i], 7U);
    }
    CHECK(r == PUBACK_NOT_FOUND, "垃圾字节 → NOT_FOUND");
    CHECK(acc.n == 0U, "累积器没被垃圾污染");

    /* 正常帧：前 3 个字节不够，第 4 个字节才出结果 */
    puback_acc_reset(&acc);
    CHECK(puback_acc_feed(&acc, 0x40U, 7U) == PUBACK_INCOMPLETE, "第 1 字节不够");
    CHECK(puback_acc_feed(&acc, 0x02U, 7U) == PUBACK_INCOMPLETE, "第 2 字节不够");
    CHECK(puback_acc_feed(&acc, 0x00U, 7U) == PUBACK_INCOMPLETE, "第 3 字节不够");
    CHECK(puback_acc_feed(&acc, 0x07U, 7U) == PUBACK_OK, "第 4 字节攒齐 → OK");
    CHECK(acc.n == 0U, "出结果后自动清空，可以接着喂");

    /* id 不匹配：要能从结果里看出来，不能当成 OK */
    puback_acc_reset(&acc);
    (void)puback_acc_feed(&acc, 0x40U, 7U);
    (void)puback_acc_feed(&acc, 0x02U, 7U);
    (void)puback_acc_feed(&acc, 0x00U, 7U);
    CHECK(puback_acc_feed(&acc, 0x08U, 7U) == PUBACK_ID_MISMATCH,
          "id=8 但等的是 7 → ID_MISMATCH");

    /* 帧被切成两片：模拟串口分两次收到 */
    puback_acc_reset(&acc);
    (void)puback_acc_feed(&acc, 0x40U, 7U);
    (void)puback_acc_feed(&acc, 0x02U, 7U);
    CHECK(acc.n == 2U, "半帧留在累积器里");
    CHECK(puback_acc_feed(&acc, 0x00U, 7U) == PUBACK_INCOMPLETE, "接着喂");
    CHECK(puback_acc_feed(&acc, 0x07U, 7U) == PUBACK_OK, "跨片也能拼成 OK");

    /* 两帧连续到达：中间不需要 reset */
    puback_acc_reset(&acc);
    (void)puback_acc_feed(&acc, 0x40U, 7U);
    (void)puback_acc_feed(&acc, 0x02U, 7U);
    (void)puback_acc_feed(&acc, 0x00U, 7U);
    CHECK(puback_acc_feed(&acc, 0x07U, 7U) == PUBACK_OK, "第一帧 OK");
    (void)puback_acc_feed(&acc, 0x40U, 9U);
    (void)puback_acc_feed(&acc, 0x02U, 9U);
    (void)puback_acc_feed(&acc, 0x00U, 9U);
    CHECK(puback_acc_feed(&acc, 0x09U, 9U) == PUBACK_OK, "紧接着的第二帧也 OK");
}

/* ============================ net_tx ============================ */

/* 假发送回调：记录被调了几次、每次都用了什么 id */
static int      g_send_calls;
static uint16_t g_send_last_id;
static int      g_send_rc;

static int fake_send(const uint8_t *payload, uint16_t len, uint16_t pkt_id)
{
    (void)payload;
    (void)len;
    g_send_calls++;
    g_send_last_id = pkt_id;
    return g_send_rc;
}

static void test_net_tx_window_one(void)
{
    uint16_t id = 0U;

    printf("[net_tx] 窗口=1：没确认就不许发下一条\n");

    (void)rb_init(0U, 0U);
    net_tx_init();
    g_send_calls = 0;
    g_send_rc = 0;

    /* 缓冲空：没得发 */
    CHECK(net_tx_begin(fake_send, &id) == NET_TX_NO_DATA, "空缓冲 → NO_DATA");

    (void)rb_put();
    (void)rb_put();

    CHECK(net_tx_begin(fake_send, &id) == NET_TX_STARTED, "有数据 → STARTED");
    CHECK(g_send_calls == 1, "回调被调 1 次");
    CHECK(net_tx_is_busy() == 1, "通道被占用");
    CHECK(rb_pending() == 2U, "🚨 发出去还没确认，rp 不许动（仍 2 条）");

    CHECK(net_tx_begin(fake_send, &id) == NET_TX_BUSY, "上一条未确认 → BUSY");
    CHECK(g_send_calls == 1, "BUSY 时不许碰回调");

    /* 发错 id 来模拟迟到的旧 ACK：不能提交 */
    CHECK(net_tx_on_puback((uint16_t)(net_tx_inflight_id() + 1U)) == 0,
          "id 不匹配 → 不提交");
    CHECK(rb_pending() == 2U, "错误 ACK 后积压不变");

    /* 正确的 ACK：这才是唯一提交点 */
    CHECK(net_tx_on_puback(net_tx_inflight_id()) == 1, "id 匹配 → 提交");
    CHECK(rb_pending() == 1U, "提交后剩 1 条");
    CHECK(net_tx_is_busy() == 0, "通道释放");

    CHECK(net_tx_begin(fake_send, &id) == NET_TX_STARTED, "可以发下一条了");
}

static void test_net_tx_timeout_keeps_record(void)
{
    uint16_t id = 0U;

    printf("[net_tx] 等 PUBACK 超时：只放窗口，不丢数据\n");

    (void)rb_init(0U, 0U);
    net_tx_init();
    g_send_rc = 0;

    (void)rb_put();
    CHECK(net_tx_begin(fake_send, &id) == NET_TX_STARTED, "发一条");
    net_tx_note_sent_at(1000U);

    CHECK(net_tx_check_timeout(1000U + NET_TX_ACK_TIMEOUT_MS - 1U) == 0,
          "差 1 ms 不算超时");
    CHECK(net_tx_is_busy() == 1, "仍在等");

    CHECK(net_tx_check_timeout(1000U + NET_TX_ACK_TIMEOUT_MS) == 1, "到点判超时");
    CHECK(net_tx_is_busy() == 0, "窗口已释放");
    CHECK(rb_pending() == 1U, "🚨 超时不提交，记录还在缓冲里等重发");

    /* 超时后再来的 ACK 已经没用了：没有在途消息 */
    CHECK(net_tx_on_puback(id) == 0, "超时后的迟到 ACK 被忽略");
    CHECK(rb_pending() == 1U, "积压依旧 1 条");

    /* 未调 note_sent_at 时不判超时（宁可晚发也不误判重发） */
    net_tx_init();
    (void)rb_put();
    CHECK(net_tx_begin(fake_send, &id) == NET_TX_STARTED, "重新发");
    CHECK(net_tx_check_timeout(0xFFFFFFFFU) == 0, "没记发送时刻 → 不判超时");
}

static void test_net_tx_send_fail(void)
{
    uint16_t id = 0U;

    printf("[net_tx] 发送当场失败：不进等待态、不动 rp\n");

    (void)rb_init(0U, 0U);
    net_tx_init();
    g_send_rc = -1;

    (void)rb_put();
    CHECK(net_tx_begin(fake_send, &id) == NET_TX_SEND_FAIL, "回调失败 → SEND_FAIL");
    CHECK(net_tx_is_busy() == 0, "不占着窗口");
    CHECK(rb_pending() == 1U, "记录还在，下次重试");

    CHECK(net_tx_begin(NULL, &id) == NET_TX_NO_NET, "回调为 NULL → NO_NET");
}

static void test_net_tx_pktid_wrap(void)
{
    uint16_t id = 0U;
    int i;

    printf("[net_tx] 报文标识符绕回跳过 0\n");

    (void)rb_init(0U, 0U);
    net_tx_init();
    g_send_rc = 0;

    /* 连发 70000 次会绕回，但太慢；直接检查前两个 id 是 1、2
     *（0 是保留值，MQTT §2.3.1 明文禁止） */
    (void)rb_put();
    CHECK(net_tx_begin(fake_send, &id) == NET_TX_STARTED, "第一条");
    CHECK(id == NET_TX_PKTID_MIN, "第一个 id 是 1，不是 0");
    (void)net_tx_on_puback(id);

    (void)rb_put();
    CHECK(net_tx_begin(fake_send, &id) == NET_TX_STARTED, "第二条");
    CHECK(id == 2U, "id 递增到 2");

    /* 让 id 走到上界附近，确认绕回后不会给出 0 */
    (void)net_tx_on_puback(id);
    for (i = 0; i < 70000; i++) {
        (void)rb_put();
        if (net_tx_begin(fake_send, &id) != NET_TX_STARTED) {
            break;
        }
        if (id == 0U) {
            break;
        }
        (void)net_tx_on_puback(id);
    }
    CHECK(id != 0U, "绕回 70000 次也从未给出 id = 0");
}

static void test_scenario_backfill_with_puback(void)
{
    uint16_t id = 0U;
    uint32_t i;
    net_tx_stat_t st;

    printf("[场景] 断网攒 200 条 → 重连 → 逐条 PUBACK → 补完\n");

    (void)rb_init(0U, 0U);
    net_fsm_init();
    net_tx_init();
    g_send_rc = 0;

    /* 断网期间攒数据 */
    for (i = 0U; i < 200U; i++) {
        (void)rb_put();
    }
    CHECK(net_fsm_state() == NET_OFFLINE, "断网时是 OFFLINE");
    CHECK(net_fsm_can_send() == 0, "OFFLINE 不许发");

    /* 重连：有积压 → BACKFILL */
    net_fsm_on_connected(rb_pending());
    CHECK(net_fsm_state() == NET_BACKFILL, "有 200 条积压 → BACKFILL");

    /* 逐条发送 + 确认，中间穿插错误的 ACK */
    for (i = 0U; i < 200U; i++) {
        if (net_tx_begin(fake_send, &id) != NET_TX_STARTED) {
            CHECK(0, "补传中途 begin 失败");
            break;
        }
        /* 每 10 条先塞一个错误 id，确认不会误提交 */
        if ((i % 10U) == 0U) {
            (void)net_tx_on_puback((uint16_t)(id + 1U));
        }
        (void)net_tx_on_puback(id);
        net_fsm_after_ack(rb_pending());
    }

    CHECK(rb_pending() == 0U, "200 条一条不剩");
    CHECK(net_fsm_state() == NET_ONLINE, "补完自动回 ONLINE");

    net_tx_stat(&st);
    CHECK(st.start == 200U, "发出 200 条");
    CHECK(st.ack_ok == 200U, "确认 200 条");
    CHECK(st.ack_bad == 20U, "挡下 20 个错误 ACK");
}

int main(void)
{
    printf("==== ring_buffer + net_fsm + rec_codec + puback + net_tx 测试 ====\n\n");

    test_ring_basic();
    test_ring_peek_does_not_advance();
    test_ring_empty_commit();
    test_ring_full_and_overwrite();
    test_ring_wraparound();
    test_ring_offset();
    test_fsm_paths();
    test_fsm_after_ack_ignores_online();
    test_fsm_backoff();
    test_fsm_backoff_reset();
    test_fsm_names();
    test_scenario_offline_30min();
    test_scenario_disconnect_mid_backfill();

    test_rec_codec_roundtrip();
    test_rec_codec_layout();
    test_rec_codec_blank_and_corrupt();
    test_puback_parse();
    test_puback_acc();
    test_net_tx_window_one();
    test_net_tx_timeout_keeps_record();
    test_net_tx_send_fail();
    test_net_tx_pktid_wrap();
    test_scenario_backfill_with_puback();

    printf("\n------------------------------------\n");
    printf("PASS %d, FAIL %d\n", g_pass, g_fail);
    printf("RESULT: %s\n", (g_fail == 0) ? "ALL PASS" : "FAILED");

    return (g_fail == 0) ? 0 : 1;
}