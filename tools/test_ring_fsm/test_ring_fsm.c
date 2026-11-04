/**
  ******************************************************************************
  * @file    test_ring_fsm.c
  * @brief   ring_buffer.c + net_fsm.c 的 PC 单元测试
  *
  *          这两个模块都是纯逻辑（不碰 HAL / FreeRTOS），所以能在 Windows
  *          上用 gcc 直接编译测试 —— 和 W2 的 Modbus、W3 的 MQTT 一个套路。
  *
  *          编译：  build.bat
  *          运行：  test_ring_fsm.exe
  ******************************************************************************
  */

#include "ring_buffer.h"
#include "net_fsm.h"

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

int main(void)
{
    printf("==== ring_buffer + net_fsm 测试 ====\n\n");

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

    printf("\n------------------------------------\n");
    printf("PASS %d, FAIL %d\n", g_pass, g_fail);
    printf("RESULT: %s\n", (g_fail == 0) ? "ALL PASS" : "FAILED");

    return (g_fail == 0) ? 0 : 1;
}