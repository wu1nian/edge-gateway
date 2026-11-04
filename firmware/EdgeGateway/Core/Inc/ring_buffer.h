/**
  ******************************************************************************
  * @file    ring_buffer.h
  * @brief   Flash 环形缓冲的逻辑层（W4·S24 定接口，W5 落实存储后端）
  *
  *          这一层只管【游标怎么动】，不管【数据存在哪】。
  *          wp/rp 是"记录序号"，不是字节地址——换页、换扇区时逻辑不用改。
  *
  *          完整数据流（见 docs/task-architecture.md §3）：
  *
  *            sensor_task ──storage_q──► storage_task ──rb_put──► [环形区]
  *                                                                  │
  *            net_task ──rb_peek──► 组 PUBLISH ──► ESP8266 ──► broker
  *                    └──── rb_commit ──── 收到 PUBACK 才推进 rp ◄─┘
  *
  *          🚨 两条铁律：
  *            ① 先写数据，再推进 wp —— 顺序反了，掉电会读到半条无效记录
  *            ② 只有收到 PUBACK 才推进 rp —— 发出去就推进，一旦发失败这条就永久丢了
  ******************************************************************************
  */

#ifndef __RING_BUFFER_H__
#define __RING_BUFFER_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* 环形区容量上限：按 W5 分区规划给 4096 条。
 * 1 条/s → 约 68 分钟的历史，够扛一次长时间断网。 */
#ifndef RB_CAPACITY
#define RB_CAPACITY      4096U
#endif

/* 一条记录在 Flash 里占 32 B（W5 定稿）：
 * timestamp(4) + temp(2) + humi(2) + light(2) + seq(4) + crc(2) + 预留(16)
 * ⚠️ 内存里的 sensor_rec_t 是 16 B，落盘时补 CRC 和预留——别把两者搞混。*/
#define RB_RECORD_BYTES    32U

/* 游标类型：记录序号，不是字节地址 */
typedef uint32_t rb_seq_t;

/**
  * @brief 环形缓冲的运行状态（只读快照，给 monitor 打印用）
  */
typedef struct {
    rb_seq_t wp;          /* 写指针：下一条要写的序号 */
    rb_seq_t rp;          /* 读指针：下一条待发的序号 */
    uint32_t count;       /* 当前待发条数 = wp - rp */
    uint32_t overflow;    /* 因缓冲满而被覆盖的记录总数 */
    uint32_t commits;     /* 收到 PUBACK 后成功推进的次数 */
} rb_stat_t;

/**
  * @brief  初始化游标（从元数据区恢复）
  * @note   W4 阶段只在 RAM 里建逻辑对象；W5 接 W25Q64 时把 wp/rp 写回元数据区。
  * @retval 0 成功；-1 参数非法（如容量为 0）
  */
int rb_init(rb_seq_t initial_wp, rb_seq_t initial_rp);

/**
  * @brief 还有几条待发（wp - rp）
  */
uint32_t rb_pending(void);

/**
  * @brief 缓冲是否为空（wp == rp）
  */
int rb_is_empty(void);

/**
  * @brief 缓冲是否已满（(wp + 1) % N == rp）
  * @note  留一格不用，用来区分"空"和"满"
  */
int rb_is_full(void);

/**
  * @brief  写入完成通知：由 storage_task 在【数据已落盘之后】调用
  * @note   🚨 调用时机是关键：必须先把 32 B 写进 Flash，再调这个推进 wp。
  *         如果先推进 wp 再写数据，掉电瞬间 wp 会指向一条没写完的记录。
  * @retval 0  成功，待发数 +1
  * @retval 1  成功，但缓冲已满，覆盖了最旧一条（overflow +1，rp 已前移）
  * @retval -1 参数/状态异常
  */
int rb_put(void);

/**
  * @brief  取待发记录的序号（不推进 rp）
  * @param  out_seq 输出：最旧一条待发记录的序号
  * @retval 0  成功
  * @retval -1 缓冲为空
  */
int rb_peek(rb_seq_t *out_seq);

/**
  * @brief  PUBACK 到达后推进 rp —— 数据的"发成功"判定点
  * @note   只有 broker 明确确认了才调它。发送函数返回成功 ≠ 对方收到。
  * @retval 0  成功
  * @retval -1 缓冲为空，无可推进项
  */
int rb_commit(void);

/**
  * @brief 读统计快照
  */
void rb_stat(rb_stat_t *out);

/**
  * @brief  把序列号换算成 Flash 里的字节偏移（W5 用）
  * @note   序号是逻辑游标，物理地址 = rb_seq_to_offset(seq)。两者解耦，
  *         换页/换扇区时只改这个函数，游标逻辑不动。
  */
uint32_t rb_seq_to_offset(rb_seq_t seq);

#ifdef __cplusplus
}
#endif

#endif /* __RING_BUFFER_H__ */