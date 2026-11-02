/**
 * @file mqtt_codec.c
 * @brief MQTT 3.1.1 纯协议层实现（W3 · S15–S19）。
 *
 * 本文件是纯协议代码：只做位运算和数组读写，不碰串口、DMA、RTOS。
 * 因此同一份代码既能在 STM32 上编译，也能在 PC 上跑自测。
 */

#include "mqtt_codec.h"

#include <stddef.h> /* NULL */
#include <string.h> /* strlen / memcpy */

/**
 * @brief 把 Remaining Length 数值编码成 MQTT 变长整数。
 *
 * 核心循环按「低 7 位先出」的顺序拆字节。手算 321（2 字节）：
 *   321 % 128 = 65 → 低 7 位得 0x41，再置续标志 → 0xC1
 *   321 / 128 = 2  → 最后一个字节 → 0x02
 *   结果 = C1 02
 *
 * @param val 待编码值，必须 ≤ MQTT_RL_MAX_VALUE（0x0FFFFFFF）
 * @param out 输出缓冲区，至少 MQTT_RL_MAX_BYTES（4）字节；不能为 NULL
 * @return 实际写入的字节数（1~4）；参数非法返回 -1
 */
int mqtt_rl_encode(uint32_t val, uint8_t *out)
{
    /* 防御：输出缓冲区不能为空；4 字节最多表示 0x0FFFFFFF，超出无法编码。 */
    if (out == NULL || val > MQTT_RL_MAX_VALUE)
    {
        return -1;
    }

    int n = 0; /* 已写入的字节数，同时也是输出下标 */
    do
    {
        /* 取最低 7 位：例如 321 % 128 = 65 = 0x41。 */
        uint8_t b = (uint8_t)(val % 128u);

        /* 右移 7 位：321 / 128 = 2，剩下的 2 还要继续编码。 */
        val /= 128u;

        /* 还有剩余数据 → 当前字节 bit7 置 1，告诉接收方“后面还有”。 */
        if (val > 0u)
        {
            b |= 0x80u;
        }

        out[n++] = b;
    } while (val > 0u); /* val 归零说明所有 7 位组都已输出 */

    return n;
}

/**
 * @brief 从缓冲区解码 MQTT Remaining Length 变长整数。
 *
 * 解码是编码的逆过程：每读一个字节，取低 7 位乘以当前权值，
 * 再把权值乘 128 继续处理下一个字节。
 *
 * @param buf  输入缓冲区；调用方必须保证至少能读到终止字节（最多 4 字节）
 * @param used 输出：实际读取的字节数（1~4）；可为 NULL
 * @return 解码出的 Remaining Length；buf 为 NULL 时返回 0（used 也置 0）
 */
uint32_t mqtt_rl_decode(const uint8_t *buf, int *used)
{
    /* 先给 used 一个确定值，避免调用方在异常路径读到未初始化数据。 */
    if (used != NULL)
    {
        *used = 0;
    }
    if (buf == NULL)
    {
        return 0u;
    }

    uint32_t val = 0u;  /* 解码结果 */
    uint32_t mult = 1u; /* 当前 7 位组的权值：1, 128, 16384, 2097152 */
    int n = 0;          /* 已读取的字节数 */

    for (;;)
    {
        uint8_t b = buf[n++];

        /* 取低 7 位，乘权值后累加；第一字节是最低 7 位，所以权值为 1。 */
        val += (uint32_t)(b & 0x7Fu) * mult;

        /* bit7 = 0 → 当前字节是最后一个，解码结束。 */
        if ((b & 0x80u) == 0u)
        {
            break;
        }

        /* bit7 = 1 → 后面还有字节，权值进一位（×128）。 */
        mult *= 128u;

        /* 规范最多 4 字节；第 4 字节仍带续标志属于非法报文。 */
        if (n >= (int)MQTT_RL_MAX_BYTES)
        {
            break;
        }
    }

    if (used != NULL)
    {
        *used = n;
    }
    return val;
}

/**
 * @brief 写一个「2 字节大端长度前缀 + 内容」的字符串字段。
 *
 * @param dst 目标缓冲区；调用方须保证容量 ≥ 2 + len
 * @param s   字符串内容；len > 0 时不能为 NULL
 * @param len 字符串字节数（不含 '\0'）
 * @return 写入的总字节数（2 + len）
 */
static uint16_t mqtt_put_str(uint8_t *dst, const char *s, uint16_t len)
{
    dst[0] = (uint8_t)(len >> 8);
    dst[1] = (uint8_t)(len & 0xFFu);
    if (len > 0u)
    {
        memcpy(&dst[2], s, (size_t)len);
    }
    return (uint16_t)(2u + len);
}

/**
 * @brief 组一条完整 CONNECT（固定头 + 可变头 + 最多五格载荷，W3 · S17）。
 *
 * 载荷顺序是规范 §3.1.3 的硬规定：
 *   ClientID → Will Topic → Will Message → Username → Password
 * 哪几格出现由连接标志位决定；这里从「指针是否为 NULL」反推标志位，
 * 保证「标志位说有」和「载荷里真有」永远一致。
 *
 * @param c   连接参数；c 与 c->client_id 不能为 NULL
 * @param out 输出缓冲区，不能为 NULL
 * @param cap out 的容量（字节）
 * @return 成功返回整帧字节数；参数非法、标志与字段不匹配、容量不足均返回 -1
 */
int mqtt_build_connect(const mqtt_connect_t *c, uint8_t *out, uint16_t cap)
{
    if (c == NULL || c->client_id == NULL || out == NULL)
    {
        return -1;
    }

    /* ── 第一步：决定连接标志字节（开关位必须和载荷字段成对） ── */
    uint8_t flags = MQTT_CONNECT_FLAG_CLEAN_SESSION; /* 本项目恒开干净会话 */

    /* 遗嘱主题和遗嘱消息必须同时给，只给一个属于调用方参数错误。 */
    if ((c->will_topic == NULL) != (c->will_msg == NULL))
    {
        return -1;
    }
    if (c->will_topic != NULL)
    {
        flags |= MQTT_CONNECT_FLAG_WILL;
    }
    if (c->username != NULL)
    {
        flags |= MQTT_CONNECT_FLAG_USERNAME;
    }
    if (c->password != NULL)
    {
        /* 规范 §3.1.2.3：Username Flag = 0 时 Password Flag 必须为 0。 */
        if (c->username == NULL)
        {
            return -1;
        }
        flags |= MQTT_CONNECT_FLAG_PASSWORD;
    }

    /* ── 第二步：把所有字符串长度一次算清（每格都是 2 B 长度 + 内容） ── */
    size_t n = strlen(c->client_id);
    if (n > 0xFFFFu)
    {
        return -1;
    }
    uint16_t idlen = (uint16_t)n;

    uint16_t wtlen = 0; /* 遗嘱主题长度 */
    uint16_t wmlen = 0; /* 遗嘱消息长度 */
    uint16_t ulen = 0;  /* 用户名长度 */
    uint16_t plen = 0;  /* 密码长度 */

    if (flags & MQTT_CONNECT_FLAG_WILL)
    {
        n = strlen(c->will_topic);
        if (n > 0xFFFFu)
        {
            return -1;
        }
        wtlen = (uint16_t)n;

        n = strlen(c->will_msg);
        if (n > 0xFFFFu)
        {
            return -1;
        }
        wmlen = (uint16_t)n;
    }
    if (flags & MQTT_CONNECT_FLAG_USERNAME)
    {
        n = strlen(c->username);
        if (n > 0xFFFFu)
        {
            return -1;
        }
        ulen = (uint16_t)n;
    }
    if (flags & MQTT_CONNECT_FLAG_PASSWORD)
    {
        n = strlen(c->password);
        if (n > 0xFFFFu)
        {
            return -1;
        }
        plen = (uint16_t)n;
    }

    /* 可变头固定 10 B：协议名 6（长度前缀 2 + "MQTT" 4）+ 级别 1 + 标志 1 + 保活 2 */
    uint32_t rem = 10u + 2u + (uint32_t)idlen;
    if (flags & MQTT_CONNECT_FLAG_WILL)
    {
        rem += 2u + (uint32_t)wtlen + 2u + (uint32_t)wmlen;
    }
    if (flags & MQTT_CONNECT_FLAG_USERNAME)
    {
        rem += 2u + (uint32_t)ulen;
    }
    if (flags & MQTT_CONNECT_FLAG_PASSWORD)
    {
        rem += 2u + (uint32_t)plen;
    }

    /* ── 第三步：编码 RL 并检查容量（先查再写，否则越界） ── */
    uint8_t rl[MQTT_RL_MAX_BYTES];
    int rl_n = mqtt_rl_encode(rem, rl);
    if (rl_n < 0)
    {
        return -1;
    }

    uint32_t total = 1u + (uint32_t)rl_n + rem; /* 首字节 + RL + 可变头 + 载荷 */
    if (total > (uint32_t)cap)
    {
        return -1;
    }

    /* ── 第四步：固定头 ── */
    uint16_t p = 0;
    out[p++] = MQTT_MSG_CONNECT; /* 0x10：类型 1 + 标志 0 */
    memcpy(&out[p], rl, (size_t)rl_n);
    p += (uint16_t)rl_n;

    /* ── 第五步：可变头 10 B ── */
    out[p++] = 0x00u; /* 协议名长度前缀（大端）= 4，即后面 "MQTT" 的字符数 */
    out[p++] = 0x04u;
    out[p++] = 'M';
    out[p++] = 'Q';
    out[p++] = 'T';
    out[p++] = 'T';
    out[p++] = MQTT_PROTO_LEVEL_V311;              /* 0x04 = 3.1.1 */
    out[p++] = flags;                              /* 连接标志：由第一步推导 */
    out[p++] = (uint8_t)(c->keepalive_s >> 8);     /* KeepAlive 高字节（大端） */
    out[p++] = (uint8_t)(c->keepalive_s & 0xFFu);  /* KeepAlive 低字节 */

    /* ── 第六步：载荷按规范固定顺序落盘 ── */
    p += mqtt_put_str(&out[p], c->client_id, idlen); /* 第 1 格：ClientID（恒有） */
    if (flags & MQTT_CONNECT_FLAG_WILL)
    {
        p += mqtt_put_str(&out[p], c->will_topic, wtlen); /* 第 2 格：Will Topic */
        p += mqtt_put_str(&out[p], c->will_msg, wmlen);   /* 第 3 格：Will Message */
    }
    if (flags & MQTT_CONNECT_FLAG_USERNAME)
    {
        p += mqtt_put_str(&out[p], c->username, ulen); /* 第 4 格：Username */
    }
    if (flags & MQTT_CONNECT_FLAG_PASSWORD)
    {
        p += mqtt_put_str(&out[p], c->password, plen); /* 第 5 格：Password */
    }

    return (int)p;
}

/**
 * @brief 只带 ClientID 的便捷封装（S16 接口；输出与 18 字节哨兵一致）。
 *
 * @param client_id   客户端 ID，UTF-8 字符串，不能为 NULL
 * @param keepalive_s 心跳周期（秒），大端写入；0 = 不启用
 * @param out         输出缓冲区首地址，不能为 NULL
 * @param cap         out 的容量（字节），不够则返回 -1
 * @return 成功返回整帧字节数；参数非法或容量不足返回 -1
 */
int mqtt_build_connect_head(const char *client_id,
                            uint16_t keepalive_s,
                            uint8_t *out,
                            uint16_t cap)
{
    /* 五格载荷只填第 1 格，其余指针为 NULL → 标志位只剩 0x02。 */
    mqtt_connect_t c = {client_id, NULL, NULL, NULL, NULL, keepalive_s};
    return mqtt_build_connect(&c, out, cap);
}

/**
 * @brief 组一条 SUBSCRIBE（W3 · S18）。
 *
 * 帧结构：0x82 + RL + 报文标识符(2) + 主题长度(2) + 主题 + 请求 QoS(1)。
 * 注意首字节低 4 位是规范强制的 2，不是 0；写成 0x80 属于协议错误。
 *
 * @param topic  主题过滤器（如 "gw01/data"），不能为 NULL；长度 ≤ 65535
 * @param qos    请求的 QoS，只支持 0 或 1
 * @param pkt_id 报文标识符，不能为 0
 * @param out    输出缓冲区，不能为 NULL
 * @param cap    out 的容量（字节）
 * @return 成功返回整帧字节数；参数非法或容量不足返回 -1
 */
int mqtt_build_subscribe(const char *topic, uint8_t qos, uint16_t pkt_id,
                         uint8_t *out, uint16_t cap)
{
    if (topic == NULL || out == NULL)
    {
        return -1;
    }
    if (qos > 1u)
    {
        return -1; /* 本项目只实现 QoS0/1；QoS2 需要额外握手，不支持 */
    }
    if (pkt_id == 0u)
    {
        return -1; /* 规范 §2.3.1：报文标识符不能为 0 */
    }

    size_t tn = strlen(topic);
    if (tn > 0xFFFFu)
    {
        return -1;
    }
    uint16_t tlen = (uint16_t)tn;

    /* RL = 报文标识符(2) + 主题长度前缀(2) + 主题 + QoS(1) */
    uint32_t rem = 2u + 2u + (uint32_t)tlen + 1u;

    uint8_t rl[MQTT_RL_MAX_BYTES];
    int rl_n = mqtt_rl_encode(rem, rl);
    if (rl_n < 0)
    {
        return -1;
    }

    uint32_t total = 1u + (uint32_t)rl_n + rem;
    if (total > (uint32_t)cap)
    {
        return -1;
    }

    uint16_t p = 0;
    out[p++] = MQTT_MSG_SUBSCRIBE; /* 0x82：类型 8 + 强制标志 2 */
    memcpy(&out[p], rl, (size_t)rl_n);
    p += (uint16_t)rl_n;
    out[p++] = (uint8_t)(pkt_id >> 8); /* 报文标识符，大端 */
    out[p++] = (uint8_t)(pkt_id & 0xFFu);
    out[p++] = (uint8_t)(tlen >> 8); /* 主题长度前缀，大端 */
    out[p++] = (uint8_t)(tlen & 0xFFu);
    memcpy(&out[p], topic, (size_t)tlen);
    p += tlen;
    out[p++] = qos; /* 本主题请求的 QoS */

    return (int)p;
}

/**
 * @brief 组一条 PUBLISH（W3 · S18，QoS 0/1）。
 *
 * QoS0 与 QoS1 只差三处：首字节 0x30/0x32、是否带 2 字节标识符、RL 大小。
 * 载荷是二进制且没有自己的长度字段，其长度由 RL 反推，所以 RL 必须精确。
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
                       uint8_t qos, uint16_t pkt_id, uint8_t *out, uint16_t cap)
{
    if (topic == NULL || out == NULL)
    {
        return -1;
    }
    if (qos > 1u)
    {
        return -1; /* QoS2 不支持 */
    }
    if (msg == NULL && mlen > 0u)
    {
        return -1; /* 有载荷长度却没给缓冲区 */
    }
    if (qos > 0u && pkt_id == 0u)
    {
        return -1; /* QoS1 必须带非 0 的报文标识符 */
    }

    size_t tn = strlen(topic);
    if (tn > 0xFFFFu)
    {
        return -1;
    }
    uint16_t tlen = (uint16_t)tn;

    /* RL = 主题长度前缀(2) + 主题 + [标识符(2)，仅 QoS>0] + 载荷 */
    uint32_t rem = 2u + (uint32_t)tlen + ((qos > 0u) ? 2u : 0u) + (uint32_t)mlen;

    uint8_t rl[MQTT_RL_MAX_BYTES];
    int rl_n = mqtt_rl_encode(rem, rl);
    if (rl_n < 0)
    {
        return -1;
    }

    uint32_t total = 1u + (uint32_t)rl_n + rem;
    if (total > (uint32_t)cap)
    {
        return -1;
    }

    uint16_t p = 0;
    out[p++] = (qos > 0u) ? MQTT_MSG_PUBLISH_QOS1 : MQTT_MSG_PUBLISH; /* 0x32 / 0x30 */
    memcpy(&out[p], rl, (size_t)rl_n);
    p += (uint16_t)rl_n;
    out[p++] = (uint8_t)(tlen >> 8); /* 主题长度前缀，大端 */
    out[p++] = (uint8_t)(tlen & 0xFFu);
    memcpy(&out[p], topic, (size_t)tlen);
    p += tlen;
    if (qos > 0u)
    {
        out[p++] = (uint8_t)(pkt_id >> 8); /* 标识符只在 QoS>0 时出现 */
        out[p++] = (uint8_t)(pkt_id & 0xFFu);
    }
    if (mlen > 0u)
    {
        memcpy(&out[p], msg, (size_t)mlen); /* 载荷原样拷贝，不加 '\0' */
        p += mlen;
    }

    return (int)p;
}

/**
 * @brief 组一条 PUBACK（W3 · S18）。
 *
 * RL 恒为 2（只有 2 字节标识符），所以整帧固定 4 字节：
 *   40 02 <ID 高> <ID 低>
 *
 * @param pkt_id 被确认报文的标识符，不能为 0
 * @param out    输出缓冲区，不能为 NULL
 * @param cap    out 的容量，至少 4 字节
 * @return 成功固定返回 4；参数非法或容量不足返回 -1
 */
int mqtt_build_puback(uint16_t pkt_id, uint8_t *out, uint16_t cap)
{
    if (out == NULL || pkt_id == 0u || cap < 4u)
    {
        return -1;
    }

    out[0] = MQTT_MSG_PUBACK; /* 0x40：类型 4 + 标志 0 */
    out[1] = 0x02u;           /* RL = 2：后面只有标识符 */
    out[2] = (uint8_t)(pkt_id >> 8);
    out[3] = (uint8_t)(pkt_id & 0xFFu);

    return 4;
}

/**
 * @brief 带长度检查的 RL 解码（收包路径专用）。
 *
 * mqtt_rl_decode() 按「调用方保证能读到终止字节」设计，适合已知完整的报文；
 * 但收包时缓冲区可能只有半个 RL（TCP 只到了一部分），直接调用会越界。
 * 本函数最多读 avail 字节，且第 4 字节仍带续标志视为非法。
 *
 * @param buf      输入缓冲区；不能为 NULL
 * @param avail    可读字节数
 * @param out_val  输出：解码结果；不能为 NULL
 * @param out_used 输出：实际读取字节数（1~4）；可为 NULL
 * @return 成功返回 0；数据不足 / 非法 / 参数错误返回 -1
 */
static int mqtt_rl_decode_checked(const uint8_t *buf, uint16_t avail,
                                  uint32_t *out_val, int *out_used)
{
    if (buf == NULL || out_val == NULL || avail == 0u)
    {
        return -1;
    }

    uint32_t v = 0u;
    uint32_t mult = 1u;
    uint16_t i = 0u;

    for (;;)
    {
        /* 读之前先确认还有字节，且没有超过 4 字节上限。 */
        if (i >= avail || i >= (uint16_t)MQTT_RL_MAX_BYTES)
        {
            return -1;
        }

        uint8_t b = buf[i];
        v += (uint32_t)(b & 0x7Fu) * mult;
        ++i;

        if ((b & 0x80u) == 0u)
        {
            break; /* bit7 = 0：终止字节，解码完成 */
        }

        /* 第 4 字节仍带续标志属于非法报文。 */
        if (i >= (uint16_t)MQTT_RL_MAX_BYTES)
        {
            return -1;
        }
        mult *= 128u;
    }

    *out_val = v;
    if (out_used != NULL)
    {
        *out_used = (int)i;
    }
    return 0;
}

/**
 * @brief 组一条 PINGREQ（W3 · S19）。
 *
 * 固定头 + RL = 0，两字节写死：C0 00。
 *
 * @param out 输出缓冲区，不能为 NULL
 * @param cap out 的容量，至少 2 字节
 * @return 成功固定返回 2；参数非法或容量不足返回 -1
 */
int mqtt_pingreq(uint8_t *out, uint16_t cap)
{
    if (out == NULL || cap < 2u)
    {
        return -1;
    }
    out[0] = MQTT_MSG_PINGREQ; /* 0xC0：类型 12 + 标志 0 */
    out[1] = 0x00u;            /* RL = 0：没有可变头和载荷 */
    return 2;
}

/**
 * @brief 组一条 DISCONNECT（W3 · S19）。
 *
 * 固定头 + RL = 0，两字节写死：E0 00。
 * 主动发它 = 正常下线，Broker 不触发遗嘱。
 *
 * @param out 输出缓冲区，不能为 NULL
 * @param cap out 的容量，至少 2 字节
 * @return 成功固定返回 2；参数非法或容量不足返回 -1
 */
int mqtt_disconnect(uint8_t *out, uint16_t cap)
{
    if (out == NULL || cap < 2u)
    {
        return -1;
    }
    out[0] = MQTT_MSG_DISCONNECT; /* 0xE0：类型 14 + 标志 0 */
    out[1] = 0x00u;               /* RL = 0 */
    return 2;
}

/**
 * @brief 收包分类：解析首字节与 RL，判断报文类型和帧边界（W3 · S19）。
 *
 * 两步走：
 *   ① 分帧：首字节高 4 位取类型；解 RL 得到「可变头 + 载荷」长度；
 *   ② 分发：调用方按 body_off 直接读各类报文的字段。
 *
 * 若 off + rl > len，说明这一帧还没收全，返回 UNKNOWN，
 * 不能按半帧去读字段，否则会读到上一帧的残留。
 *
 * @param buf      收到的字节流；不能为 NULL
 * @param len      有效字节数
 * @param body_off 输出：可变头起点；可为 NULL
 * @param body_len 输出：RL 值；可为 NULL
 * @return 报文类型；不认识、没收全或参数非法返回 MQTT_PKT_UNKNOWN
 */
mqtt_pkt_t mqtt_classify(const uint8_t *buf, uint16_t len,
                         uint16_t *body_off, uint16_t *body_len)
{
    /* 先给输出参数一个确定值，异常路径下调用方不会读到未初始化数据。 */
    if (body_off != NULL)
    {
        *body_off = 0u;
    }
    if (body_len != NULL)
    {
        *body_len = 0u;
    }

    /* 最短报文是 PINGRESP / DISCONNECT 这类 2 字节报文。 */
    if (buf == NULL || len < 2u)
    {
        return MQTT_PKT_UNKNOWN;
    }

    uint8_t type = (uint8_t)(buf[0] >> 4); /* 高 4 位 = 报文类型，低 4 位是标志 */

    uint32_t rl = 0u;
    int used = 0;
    if (mqtt_rl_decode_checked(&buf[1], (uint16_t)(len - 1u), &rl, &used) != 0)
    {
        return MQTT_PKT_UNKNOWN; /* RL 本身还没收全，或超过 4 字节 */
    }

    uint32_t off = 1u + (uint32_t)used; /* 可变头起点 */
    if (off + rl > (uint32_t)len)
    {
        return MQTT_PKT_UNKNOWN; /* 帧没收全：len < 1 + RL 字节数 + RL */
    }

    if (body_off != NULL)
    {
        *body_off = (uint16_t)off;
    }
    if (body_len != NULL)
    {
        *body_len = (uint16_t)rl;
    }

    switch (type)
    {
        case 0x2u: return MQTT_PKT_CONNACK;  /* 20 */
        case 0x9u: return MQTT_PKT_SUBACK;   /* 90 */
        case 0x3u: return MQTT_PKT_PUBLISH;  /* 30 / 32 */
        case 0x4u: return MQTT_PKT_PUBACK;   /* 40 */
        case 0xDu: return MQTT_PKT_PINGRESP; /* D0 */
        default:   return MQTT_PKT_UNKNOWN;  /* PINGREQ 等客户端报文不在这里收 */
    }
}

/**
 * @brief CONNACK 返回码 → 人话（规范 §3.2.2.3，W3 · S19）。
 *
 * @param code CONNACK 可变头第 2 字节
 * @return 静态字符串，永不返回 NULL
 */
const char *mqtt_connack_reason(uint8_t code)
{
    switch (code)
    {
        case 0x00u: return "连接成功";
        case 0x01u: return "协议版本不支持";
        case 0x02u: return "ClientID 被拒";
        case 0x03u: return "服务不可用";
        case 0x04u: return "用户名或密码错误";
        case 0x05u: return "未授权";
        default:    return "未知返回码";
    }
}

/**
 * @brief 心跳到期判断（W3 · S19）。
 *
 * 用「记录上次发包时刻 + 周期性检查」替代 HAL_Delay 死等：
 * 间隔取 KeepAlive 的一半，留出抖动余量；无符号减法天然处理 tick 回绕。
 *
 * @param now_ms      当前毫秒 tick
 * @param last_tx_ms  上次成功发包时的毫秒 tick
 * @param keepalive_s KeepAlive（秒）；0 = 不启用心跳
 * @return 1 = 该发 PINGREQ；0 = 还不用发
 */
int mqtt_keepalive_due(uint32_t now_ms, uint32_t last_tx_ms, uint16_t keepalive_s)
{
    if (keepalive_s == 0u)
    {
        return 0;
    }

    /* keepalive_s ≤ 65535，乘 1000 不会溢出 uint32。 */
    uint32_t interval = ((uint32_t)keepalive_s * 1000u) / 2u;
    return ((uint32_t)(now_ms - last_tx_ms) >= interval) ? 1 : 0;
}