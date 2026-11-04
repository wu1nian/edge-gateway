/**
  ******************************************************************************
  * @file    ring_buffer.c
  * @brief   Flash 环形缓冲的逻辑层实现 —— 游标规则全在这里
  *
  *          预留一格（N 格只能装 N-1 条），用来区分空和满：
  *            空：wp == rp
  *            满：(wp + 1) % N == rp
  *          如果装满 N 条，满和空的判据就撞在一起了，只能再加一个标志位，
  *          反而更容易出错。牺牲一格换判据干净，值得。
  *
  *          ⚠️ 游标不单调。wp / rp 都保存成区间 [0, N) 的位置，每次移动
  *          都对 RB_CAPACITY 取模。这一点是踩坑之后改的：
  *
  *          最初写的是"单调递增的 uint32，靠自然溢出回绕"，于是
  *          pending = wp - rp，看起来很干净。但记录序号 seq 是要写进
  *          Flash、还要跟着数据一起发出去的，它一旦被推到 N 以上，
  *          用它算偏移就得多做一次取模，越界风险藏在两处。
  *
  *          统一成"游标恒在 [0, N)"之后：
  *            pending = ((wp % N) - (rp % N)) % N
  *          多一次取模，但游标本身永远合法，rb_seq_to_offset() 不用再防溢出。
  *
  *          🚨 覆盖最旧那条时 wp 和 rp 一起前进，所以满了以后
  *             pending 恒为 N-1，不会越算越大。
  ******************************************************************************
  */

#include "ring_buffer.h"

#include <stddef.h>

/* 游标状态。W5 接 W25Q64 元数据区后，这三个值要从 Flash 恢复。
 * 单写者（storage_task 写 wp、net_task 写 rp）→ 不需要锁，见 docs §2。*/
static rb_seq_t s_wp;
static rb_seq_t s_rp;
static uint32_t s_overflow;
static uint32_t s_commits;
static int      s_inited;

/* rb_put 返回码：区分"正常写入"和"满了覆盖最旧" */
enum {
    RB_PUT_OK       = 0,
    RB_PUT_OVERWRIT = 1,
    RB_PUT_ERR      = -1
};

int rb_init(rb_seq_t initial_wp, rb_seq_t initial_rp)
{
    /* 容量为 1 时留不出那一格，判满判空无法区分 → 直接拒绝 */
    if (RB_CAPACITY < 2U) {
        return -1;
    }

    /* 恢复出来的游标先对容量取模，保证落在合法范围。
     * W5 读元数据区时可能因为掉电半写而拿到越界值，这里兜一层。*/
    s_wp       = initial_wp % RB_CAPACITY;
    s_rp       = initial_rp % RB_CAPACITY;
    s_overflow = 0U;
    s_commits  = 0U;
    s_inited   = 1;
    return 0;
}

uint32_t rb_pending(void)
{
    if (!s_inited) {
        return 0U;
    }
    /* 🚨 不能直接写 s_wp - s_rp：
     *    wp 绕回 0 而 rp 还在 1 时，0 - 1 会回绕成 0xFFFFFFFF。
     *    所以先各自对 N 取模，再用无符号减法拿到 0..N-1 范围的差值。*/
    return (uint32_t)((s_wp % RB_CAPACITY) - (s_rp % RB_CAPACITY)) % RB_CAPACITY;
}

int rb_is_empty(void)
{
    return (s_wp == s_rp) ? 1 : 0;
}

int rb_is_full(void)
{
    if (!s_inited) {
        return 0;
    }
    return (((s_wp + 1U) % RB_CAPACITY) == (s_rp % RB_CAPACITY)) ? 1 : 0;
}

int rb_put(void)
{
    int ret = RB_PUT_OK;

    if (!s_inited) {
        return RB_PUT_ERR;
    }

    /* 满了：丢掉最旧一条（rp 前移），记一次溢出。
     * 丢最旧而不是拒绝写入 —— 新数据比老数据有价值，
     * 而且拒绝写入会让 storage_task 阻塞，进而堵住 storage_q。*/
    if (rb_is_full()) {
        s_rp = (s_rp + 1U) % RB_CAPACITY;
        s_overflow++;
        ret = RB_PUT_OVERWRIT;
    }

    /* 🚨 这一步必须在数据已经写进 Flash 之后调用。
     *    放在写数据之前，掉电时 wp 会指向一条没写完的记录。*/
    s_wp = (s_wp + 1U) % RB_CAPACITY;
    return ret;
}

int rb_peek(rb_seq_t *out_seq)
{
    if ((out_seq == NULL) || (!s_inited)) {
        return -1;
    }
    if (rb_is_empty()) {
        return -1;
    }
    *out_seq = s_rp;
    return 0;
}

int rb_commit(void)
{
    if (!s_inited) {
        return -1;
    }
    if (rb_is_empty()) {
        return -1;
    }
    /* 🚨 只有收到 PUBACK 才走到这里。
     *    发送函数返回成功 ≠ broker 收到。中断在这里，rp 停在原处，
     *    重连后这条会被重发（QoS1 的"至少一次"语义）。*/
    s_rp = (s_rp + 1U) % RB_CAPACITY;
    s_commits++;
    return 0;
}

void rb_stat(rb_stat_t *out)
{
    if (out == NULL) {
        return;
    }
    out->wp       = s_wp;
    out->rp       = s_rp;
    out->count    = rb_pending();
    out->overflow = s_overflow;
    out->commits  = s_commits;
}

uint32_t rb_seq_to_offset(rb_seq_t seq)
{
    /* 序号 → 记录槽 → 字节偏移。换页/换扇区时只改这一个函数 */
    return (seq % RB_CAPACITY) * RB_RECORD_BYTES;
}