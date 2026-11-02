/**
 * @file mqtt_codec.h
 * @brief MQTT 3.1.1 报文编解码的纯协议层（W3 · S15）。
 *
 * 本文件只依赖 <stdint.h>，不依赖 HAL / FreeRTOS，PC 与 STM32 共用。
 * S15 实现 Remaining Length（RL）变长整数；S16/S17 实现 CONNECT 组帧；
 * S18 实现 PUBACK / SUBSCRIBE / PUBLISH 组帧；
 * S19 实现心跳 PINGREQ / DISCONNECT 与收包分类（CONNACK / SUBACK / PUBACK / PUBLISH / PINGRESP）。
 *
 * RL 规则（MQTT 3.1.1 §2.2.3）：
 *   - 每字节低 7 位是数据，最高位 bit7 是续标志；
 *   - bit7 = 1 表示后面还有字节，bit7 = 0 表示当前是最后一个字节；
 *   - 低位组在前（先输出 val 的最低 7 位）；
 *   - 最多 4 字节，最大可表示 268435455（0x0FFFFFFF）。
 *
 * 注意：RL 数的是「可变头 + 载荷」的总字节数，不包括固定头的首字节和 RL 自己。
 */

#ifndef MQTT_CODEC_H
#define MQTT_CODEC_H

#include <stdint.h>

/* ===================== Remaining Length 约束（MQTT 3.1.1 §2.2.3） ===================== */
#define MQTT_RL_MAX_BYTES 4u          /* 变长整数最多 4 字节 */
#define MQTT_RL_MAX_VALUE 268435455u  /* 0x0FFFFFFF：4 字节能表示的最大值 */

/* ===================== Remaining Length 编解码 ===================== */
/**
 * @brief 把 Remaining Length 数值编码成 MQTT 变长整数。
 *
 * 算法：
 *   1. 取 val 的低 7 位作为当前字节的数据部分；
 *   2. val 右移 7 位（等价于 val /= 128）；
 *   3. 如果右移后 val 仍大于 0，说明后面还有字节，把当前字节 bit7 置 1；
 *   4. 重复直到 val == 0。
 *
 * 示例：
 *   0     -> 00          （1 字节）
 *   127   -> 7F          （1 字节上限）
 *   128   -> 80 01       （低 7 位先出：0x80，再出 0x01）
 *   321   -> C1 02
 *   16383 -> FF 7F
 *   16384 -> 80 80 01
 *
 * @param val 待编码值，必须 <= MQTT_RL_MAX_VALUE
 * @param out 输出缓冲区，至少 MQTT_RL_MAX_BYTES 字节；不能为 NULL
 * @return 成功返回实际写入的字节数（1~4）；val 越界或 out 为 NULL 返回 -1
 */
int mqtt_rl_encode(uint32_t val, uint8_t *out);

/**
 * @brief 从缓冲区解码 MQTT Remaining Length 变长整数。
 *
 * 算法：
 *   1. 读一个字节，取低 7 位乘以当前权值（1, 128, 128^2, 128^3）；
 *   2. 累加到结果；
 *   3. 如果 bit7 = 0，结束；否则权值乘 128，继续读下一个字节；
 *   4. 最多读 4 字节。
 *
 * 示例：
 *   80 01       -> 128
 *   C1 02       -> 321
 *   FF FF FF 7F -> 268435455
 *
 * @param buf  输入缓冲区；调用方必须保证至少能读到终止字节（最多 4 字节）
 * @param used 输出：实际读取的字节数（1~4）；可为 NULL
 * @return 解码出的 Remaining Length 值
 *
 * @warning 本函数不做缓冲区长度检查——解析网络流时，调用方要先保证 buf 中有足够字节。
 * @warning 第 4 字节仍带续标志属于非法报文；本函数会停止读取并返回当前值，
 *          合法性由上层协议解析决定。
 */
uint32_t mqtt_rl_decode(const uint8_t *buf, int *used);

/* ===================== CONNECT 报文（MQTT 3.1.1 §3.1） ===================== */
#define MQTT_MSG_CONNECT                    0x10u                   /* 类型 1 << 4，低 4 位标志必须为 0 */
#define MQTT_PROTO_LEVEL_V311               0x04u                   /* 协议级别：3.1.1 = 0x04（3.1 是 0x03） */
#define MQTT_CONNECT_FLAG_CLEAN_SESSION     0x02u                   /* bit1 置 1：丢弃旧会话状态重新开始 */
#define MQTT_CONNECT_FLAG_WILL              0x04u                   /* bit2：载荷带遗嘱主题 + 遗嘱消息 */
#define MQTT_CONNECT_FLAG_USERNAME          0x80u                   /* bit7：载荷带用户名 */
#define MQTT_CONNECT_FLAG_PASSWORD          0x40u                   /* bit6：载荷带密码（规范要求 bit7 先为 1） */

/*
 * 连接标志字节的 8 个位（规范 §3.1.2.3），S16 只用 bit1，S17 用到 bit2/bit6/bit7：
 *   bit7 Username Flag   载荷里有用户名
 *   bit6 Password Flag   载荷里有密码（依赖 bit7 = 1）
 *   bit5 Will Retain     遗嘱消息要保留（本项目恒 0）
 *   bit4-3 Will QoS      遗嘱 QoS（仅 bit2 = 1 时有意义，本项目恒 00）
 *   bit2 Will Flag       带遗嘱
 *   bit1 Clean Session   干净会话
 *   bit0 Reserved        规范强制 = 0
 * 所以最简连接标志 = 0x02；带用户名密码 = 0xC2；只带遗嘱 = 0x06。
 */

/**
 * @brief CONNECT 的可选载荷字段。指针为 NULL 表示「不带这一格」。
 *
 * 字段顺序不是结构体顺序，而是规范 §3.1.3 的固定顺序：
 *   ClientID → Will Topic → Will Message → Username → Password
 */
typedef struct
{
    const char *client_id;  /* 必填：Broker 用来识别客户端，如 "gw01" */
    const char *will_topic; /* 可选：遗嘱主题，必须与 will_msg 同时给出 */
    const char *will_msg;   /* 可选：遗嘱消息，必须与 will_topic 同时给出 */
    const char *username;   /* 可选：用户名 */
    const char *password;   /* 可选：密码，按 C 字符串处理（不支持二进制 '\0'） */
    uint16_t keepalive_s;   /* 心跳周期（秒）；0 = 不启用心跳 */
} mqtt_connect_t;

/**
 * @brief 组一条完整 CONNECT（固定头 + 可变头 + 最多五格载荷）。
 *
 * 载荷顺序由规范 §3.1.3 固定，不能换：
 *   ClientID → Will Topic → Will Message → Username → Password
 * 哪几格出现由连接标志位决定，本函数从结构体指针自动推导：
 *   will_topic / will_msg 都非 NULL → bit2 Will Flag（两格成对出现）
 *   username 非 NULL               → bit7 Username Flag
 *   password 非 NULL               → bit6 Password Flag（要求 username 也非 NULL）
 *
 * 因为每个字段都是「2 字节长度 + 内容」，总长可以先算出来，
 * 所以本实现「先算 rem → 再查 cap → 最后直接写 out」，不需要临时大缓冲区。
 *
 * 三种哨兵（client_id = "gw01"、keepalive = 60）：
 *   只 ClientID              flags = 0x02   → 18 B
 *   + username "u" / pass "p" flags = 0xC2  → 24 B
 *   + will "wt" / "wm"        flags = 0x06  → 26 B
 *
 * @param c   连接参数；c 与 c->client_id 不能为 NULL
 * @param out 输出缓冲区，不能为 NULL
 * @param cap out 的容量（字节）
 * @return 成功返回整帧字节数；参数非法、标志与字段不匹配、容量不足均返回 -1
 *
 * @warning 密码按 C 字符串处理（strlen），不支持含 '\0' 的二进制密码。
 * @warning 遗嘱的 Will QoS / Will Retain 恒为 0（规范允许），本项目不设遗嘱等级。
 */
int mqtt_build_connect(const mqtt_connect_t *c, uint8_t *out, uint16_t cap);

/**
 * @brief 只带 ClientID 的 CONNECT 便捷封装（S16 的接口，保留不变）。
 *
 * 等价于 mqtt_build_connect() 且 will_* / username / password 全为 NULL，
 * 输出与 S16 的 18 字节哨兵逐字节一致。
 *
 * @param client_id   客户端标识，不能为 NULL；长度不能超过 65535
 * @param keepalive_s 心跳周期（秒），按大端写入；0 表示不启用心跳
 * @param out         输出缓冲区，不能为 NULL
 * @param cap         out 的容量（字节）
 * @return 成功返回整帧字节数；参数非法或容量不足返回 -1
 */
int mqtt_build_connect_head(const char *client_id, uint16_t keepalive_s,
                            uint8_t *out, uint16_t cap);

/* ===================== SUBSCRIBE / PUBLISH / PUBACK（MQTT 3.1.1 §3.8 / §3.3 / §3.4） ===================== */
#define MQTT_MSG_SUBSCRIBE   0x82u /* 类型 8 << 4 | 低 4 位强制为 2（§3.8.1） */
#define MQTT_MSG_PUBLISH     0x30u /* 类型 3 << 4 | QoS0，DUP=0、RETAIN=0 */
#define MQTT_MSG_PUBLISH_QOS1 0x32u /* 类型 3 << 4 | QoS1 */
#define MQTT_MSG_PUBACK      0x40u /* 类型 4 << 4 | 低 4 位 0 */

/**
 * @brief 组一条 SUBSCRIBE（一次订阅一个主题）。
 *
 * 帧结构：0x82 + RL + 报文标识符(2) + 主题长度(2) + 主题 + 请求 QoS(1)。
 * 首字节低 4 位是规范强制的 2，写成 0x80 属于协议错误（§3.8.1）。
 *
 * @param topic  主题过滤器（如 "gw01/data"），不能为 NULL；长度 ≤ 65535
 * @param qos    请求的 QoS，只支持 0 或 1（QoS2 需要额外握手，本项目不支持）
 * @param pkt_id 报文标识符，不能为 0（§2.3.1）
 * @param out    输出缓冲区，不能为 NULL
 * @param cap    out 的容量（字节）
 * @return 成功返回整帧字节数；参数非法或容量不足返回 -1
 */
int mqtt_build_subscribe(const char *topic, uint8_t qos, uint16_t pkt_id,
                         uint8_t *out, uint16_t cap);

/**
 * @brief 组一条 PUBLISH（QoS 0/1）。
 *
 * 帧结构：首字节(0x30/0x32) + RL + 主题长度(2) + 主题 + [标识符(2)] + 载荷。
 * QoS0 用 0x30 且无标识符；QoS1 用 0x32 且带 2 字节标识符。
 * 载荷是二进制且没有独立长度字段，长度由 RL 反推，所以 RL 必须精确。
 *
 * @param topic  主题名，不能为 NULL；长度 ≤ 65535
 * @param msg    载荷缓冲区；mlen > 0 时不能为 NULL，mlen == 0 时可为 NULL
 * @param mlen   载荷字节数（二进制，可含 '\0'）
 * @param qos    0 或 1（QoS2 不支持）
 * @param pkt_id 报文标识符；QoS1 时必须非 0，QoS0 时忽略
 * @param out    输出缓冲区，不能为 NULL
 * @param cap    out 的容量（字节）
 * @return 成功返回整帧字节数；参数非法或容量不足返回 -1
 */
int mqtt_build_publish(const char *topic, const uint8_t *msg, uint16_t mlen,
                       uint8_t qos, uint16_t pkt_id, uint8_t *out, uint16_t cap);

/**
 * @brief 组一条 PUBACK（QoS1 收到 PUBLISH 后的确认）。
 *
 * RL 恒为 2（只有标识符），整帧固定 4 字节：40 02 <ID 高> <ID 低>。
 *
 * @param pkt_id 被确认报文的标识符，不能为 0
 * @param out    输出缓冲区，不能为 NULL
 * @param cap    out 的容量，至少 4 字节
 * @return 成功固定返回 4；参数非法或容量不足返回 -1
 */
int mqtt_build_puback(uint16_t pkt_id, uint8_t *out, uint16_t cap);

/* ===================== 心跳与收包解析（MQTT 3.1.1 §3.12 / §3.13 / §3.14 / §3.2） ===================== */
#define MQTT_MSG_CONNACK    0x20u /* 类型 2 << 4：Broker 对 CONNECT 的应答 */
#define MQTT_MSG_SUBACK     0x90u /* 类型 9 << 4：Broker 对 SUBSCRIBE 的应答 */
#define MQTT_MSG_PINGREQ    0xC0u /* 类型 12 << 4 | 标志 0：客户端保活请求 */
#define MQTT_MSG_PINGRESP   0xD0u /* 类型 13 << 4 | 标志 0：Broker 保活应答 */
#define MQTT_MSG_DISCONNECT 0xE0u /* 类型 14 << 4 | 标志 0：客户端主动下线 */

/**
 * @brief 收包分类结果。
 *
 * 只关心「这条报文是什么、可变头从哪里开始、有多长」；
 * 具体字段（CONNACK 返回码、PUBACK 标识符等）由上层按类型继续读。
 */
typedef enum
{
    MQTT_PKT_UNKNOWN = 0, /* 不认识、没收全、或解析失败 */
    MQTT_PKT_CONNACK,     /* 0x20：连接应答 */
    MQTT_PKT_SUBACK,      /* 0x90：订阅应答 */
    MQTT_PKT_PUBLISH,     /* 0x30 / 0x32：收到别人发布的消息 */
    MQTT_PKT_PUBACK,      /* 0x40：QoS1 发布被确认 */
    MQTT_PKT_PINGRESP,    /* 0xD0：心跳应答 */
} mqtt_pkt_t;

/**
 * @brief 组一条 PINGREQ（心跳请求，固定 2 字节 C0 00）。
 *
 * 客户端在 KeepAlive 周期内没有其他报文发出时，用它证明「我还活着」。
 * 周期建议取 KeepAlive / 2，避免卡在 1.5 × KeepAlive 的判定边界上。
 *
 * @param out 输出缓冲区，不能为 NULL
 * @param cap out 的容量，至少 2 字节
 * @return 成功固定返回 2；参数非法或容量不足返回 -1
 */
int mqtt_pingreq(uint8_t *out, uint16_t cap);

/**
 * @brief 组一条 DISCONNECT（主动下线，固定 2 字节 E0 00）。
 *
 * 主动发 DISCONNECT 表示「我是正常走的」，Broker 不会触发遗嘱；
 * 直接断 TCP 则要等 1.5 × KeepAlive 才被判掉线，并可能触发遗嘱。
 *
 * @param out 输出缓冲区，不能为 NULL
 * @param cap out 的容量，至少 2 字节
 * @return 成功固定返回 2；参数非法或容量不足返回 -1
 */
int mqtt_disconnect(uint8_t *out, uint16_t cap);

/**
 * @brief 收包第一步：看首字节类型 + 解 RL，判断这一帧是否完整。
 *
 * 分帧规则：首字节高 4 位是类型；紧跟的 RL 告诉「可变头 + 载荷」共多少字节。
 * 若 len < 1 + RL 字节数 + RL 值，说明这一帧还没收全，返回 MQTT_PKT_UNKNOWN，
 * 调用方应保留已收数据继续等（绝不能按半个报文去读字段）。
 *
 * @param buf      收到的字节流；不能为 NULL
 * @param len      本次缓冲区里的有效字节数
 * @param body_off 输出：可变头起点（= 1 + RL 占用字节数）；可为 NULL
 * @param body_len 输出：RL 值（可变头 + 载荷的字节数）；可为 NULL
 * @return 报文类型；不认识、没收全或参数非法返回 MQTT_PKT_UNKNOWN
 *
 * @note 允许缓冲区里同时有多帧（TCP 粘包）：本函数只解析第一帧，
 *       调用方按 body_off + body_len 自行前移即可。
 * @note RL 解码在本函数内部做了长度检查，不会越界读取。
 */
mqtt_pkt_t mqtt_classify(const uint8_t *buf, uint16_t len,
                         uint16_t *body_off, uint16_t *body_len);

/**
 * @brief 把 CONNACK 返回码翻译成一句人话（规范 §3.2.2.3）。
 *
 * @param code CONNACK 可变头第 2 字节（返回码）
 * @return 静态字符串，永不返回 NULL
 */
const char *mqtt_connack_reason(uint8_t code);

/**
 * @brief 心跳到期判断：距离上次发包是否已到 KeepAlive / 2。
 *
 * 纯逻辑函数，不读硬件时钟，方便用假 tick 在 PC 上自测。
 * 内部用无符号减法计算 elapsed，能正确处理 32 位 tick 回绕。
 *
 * @param now_ms      当前毫秒 tick
 * @param last_tx_ms  上次成功发包时的毫秒 tick
 * @param keepalive_s CONNECT 里承诺的 KeepAlive（秒）；0 表示不启用心跳
 * @return 1 = 该发 PINGREQ；0 = 还不用发
 */
int mqtt_keepalive_due(uint32_t now_ms, uint32_t last_tx_ms, uint16_t keepalive_s);

#endif /* MQTT_CODEC_H */