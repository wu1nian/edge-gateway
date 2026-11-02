/**
 * @file test_mqtt_codec.c
 * @brief S15–S19 编解码 PC 自测。
 *
 * 验证：
 *   1. RL 编码结果逐字节与笔记 16 §七的哨兵向量一致；
 *   2. RL 解码能把编码结果还原成原值，used 字节数正确；
 *   3. CONNECT 整帧与笔记 17 §四的 18 字节哨兵一致；
 *   4. CONNECT 载荷扩展（用户名/密码/遗嘱）与笔记 18 §二一致；
 *   5. SUBSCRIBE / PUBLISH / PUBACK 与笔记 19 哨兵一致；
 *   6. PINGREQ / DISCONNECT 与笔记 20 哨兵一致，收包分类能认出 5 类回包；
 *   7. 越界值、空指针、容量不足、半帧等异常路径被拒绝。
 *
 * 编译运行：
 *   build.bat
 *   run.bat
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "mqtt_codec.h"

typedef struct
{
    uint32_t value;                    /* 待编码的 Remaining Length 数值 */
    uint8_t encoded[MQTT_RL_MAX_BYTES]; /* 期望的编码字节 */
    int len;                           /* 期望占用的字节数 */
} rl_vector_t;

/* 笔记 16 §七的 6 个哨兵向量 + 4 字节上限 */
static const rl_vector_t k_vectors[] = {
    /* 边界：0 和 1 字节上限 */
    {0u, {0x00u}, 1},
    {127u, {0x7Fu}, 1},

    /* 跨字节：128 是最容易写反的向量（正确是 80 01，不是 01 80） */
    {128u, {0x80u, 0x01u}, 2},

    /* 2 字节普通值 */
    {321u, {0xC1u, 0x02u}, 2},

    /* 2 字节上限 */
    {16383u, {0xFFu, 0x7Fu}, 2},

    /* 3 字节起点 */
    {16384u, {0x80u, 0x80u, 0x01u}, 3},

    /* 4 字节上限：0x0FFFFFFF */
    {MQTT_RL_MAX_VALUE, {0xFFu, 0xFFu, 0xFFu, 0x7Fu}, 4},
};

/* 按十六进制打印，方便和笔记里的期望值逐字节对照。 */
static void dump_hex(const uint8_t *buf, int len)
{
    for (int i = 0; i < len; ++i)
    {
        printf("%02X%s", buf[i], (i + 1 < len) ? " " : "");
    }
}

/* 笔记 17 §四哨兵：client_id = "gw01"、keepalive = 60 → 18 字节整帧。 */
static const uint8_t k_connect_gw01_60[] = {
    0x10u, 0x10u,                     /* 首字节 + RL = 16 */
    0x00u, 0x04u, 'M', 'Q', 'T', 'T', /* 协议名 */
    0x04u,                            /* 协议级别 3.1.1 */
    0x02u,                            /* 连接标志：只有 Clean Session */
    0x00u, 0x3Cu,                     /* KeepAlive = 60，大端 */
    0x00u, 0x04u, 'g', 'w', '0', '1', /* ClientID */
};

/* CONNECT 四类校验：哨兵逐字节、只改 KeepAlive、容量不足、空指针。 */
static int test_connect(void)
{
    int failed = 0;
    uint8_t out[32];

    /* ① 哨兵：整帧 18 字节必须逐字节对得上 */
    memset(out, 0, sizeof(out));
    int n = mqtt_build_connect_head("gw01", 60u, out, (uint16_t)sizeof(out));
    int ok = (n == (int)sizeof(k_connect_gw01_60)) &&
             (memcmp(out, k_connect_gw01_60, sizeof(k_connect_gw01_60)) == 0);
    if (!ok)
    {
        ++failed;
    }
    printf("[%s] CONNECT gw01/60  -> ", ok ? "PASS" : "FAIL");
    dump_hex(out, (n > 0) ? n : 0);
    printf("  (%d B)\n", n);

    /* ② 只把 KeepAlive 改成 120 → 仅下标 10、11 变 00 78，其余必须不变 */
    uint8_t expect[sizeof(k_connect_gw01_60)];
    memcpy(expect, k_connect_gw01_60, sizeof(expect));
    expect[10] = 0x00u; /* 120 = 0x0078，高字节 */
    expect[11] = 0x78u; /* 低字节 */

    memset(out, 0, sizeof(out));
    n = mqtt_build_connect_head("gw01", 120u, out, (uint16_t)sizeof(out));
    ok = (n == (int)sizeof(expect)) && (memcmp(out, expect, sizeof(expect)) == 0);
    if (!ok)
    {
        ++failed;
    }
    printf("[%s] CONNECT gw01/120 -> ", ok ? "PASS" : "FAIL");
    dump_hex(out, (n > 0) ? n : 0);
    printf("  (%d B)\n", n);

    /* ③ 容量少 1 字节必须拒绝，防止写越界（"gw01"/60 需要 18 B，这里只给 17 B） */
    if (mqtt_build_connect_head("gw01", 60u, out, 17u) != -1)
    {
        printf("[FAIL] CONNECT cap 少 1 字节 应返回 -1\n");
        ++failed;
    }

    /* ④ 空指针必须拒绝 */
    if (mqtt_build_connect_head(NULL, 60u, out, (uint16_t)sizeof(out)) != -1)
    {
        printf("[FAIL] CONNECT(client_id=NULL) 应返回 -1\n");
        ++failed;
    }
    if (mqtt_build_connect_head("gw01", 60u, NULL, 32u) != -1)
    {
        printf("[FAIL] CONNECT(out=NULL) 应返回 -1\n");
        ++failed;
    }

    return failed;
}

/* 笔记 18 §二哨兵：client_id="gw01"、keepalive=60，分别加用户名/密码、加遗嘱。 */
static const uint8_t k_connect_user_pass[] = {
    0x10u, 0x16u,                            /* 首字节 + RL = 22 */
    0x00u, 0x04u, 'M', 'Q', 'T', 'T',        /* 协议名 */
    0x04u, 0xC2u,                            /* 级别 3.1.1 + 标志：用户名|密码|Clean */
    0x00u, 0x3Cu,                            /* KeepAlive = 60 */
    0x00u, 0x04u, 'g', 'w', '0', '1',        /* ClientID */
    0x00u, 0x01u, 'u',                       /* Username = "u" */
    0x00u, 0x01u, 'p',                       /* Password = "p" */
};

static const uint8_t k_connect_will[] = {
    0x10u, 0x18u,                            /* 首字节 + RL = 24 */
    0x00u, 0x04u, 'M', 'Q', 'T', 'T',        /* 协议名 */
    0x04u, 0x06u,                            /* 级别 3.1.1 + 标志：遗嘱|Clean */
    0x00u, 0x3Cu,                            /* KeepAlive = 60 */
    0x00u, 0x04u, 'g', 'w', '0', '1',        /* ClientID */
    0x00u, 0x02u, 'w', 't',                  /* Will Topic = "wt" */
    0x00u, 0x02u, 'w', 'm',                  /* Will Message = "wm" */
};

/* 载荷扩展：哨兵逐字节 + 标志位与字段不匹配时的拒绝。 */
static int test_connect_payload(void)
{
    int failed = 0;
    uint8_t out[64];

    /* ① 用户名 + 密码：标志 0xC2，总长 24 */
    const mqtt_connect_t c_user = {"gw01", NULL, NULL, "u", "p", 60u};
    memset(out, 0, sizeof(out));
    int n = mqtt_build_connect(&c_user, out, (uint16_t)sizeof(out));
    int ok = (n == (int)sizeof(k_connect_user_pass)) &&
             (memcmp(out, k_connect_user_pass, sizeof(k_connect_user_pass)) == 0);
    if (!ok)
    {
        ++failed;
    }
    printf("[%s] CONNECT +user/pass -> ", ok ? "PASS" : "FAIL");
    dump_hex(out, (n > 0) ? n : 0);
    printf("  (%d B)\n", n);

    /* ② 遗嘱：标志 0x06，总长 26，且遗嘱两格必须相邻且在用户名之前 */
    const mqtt_connect_t c_will = {"gw01", "wt", "wm", NULL, NULL, 60u};
    memset(out, 0, sizeof(out));
    n = mqtt_build_connect(&c_will, out, (uint16_t)sizeof(out));
    ok = (n == (int)sizeof(k_connect_will)) &&
         (memcmp(out, k_connect_will, sizeof(k_connect_will)) == 0);
    if (!ok)
    {
        ++failed;
    }
    printf("[%s] CONNECT +will      -> ", ok ? "PASS" : "FAIL");
    dump_hex(out, (n > 0) ? n : 0);
    printf("  (%d B)\n", n);

    /* ③ 有密码却没用户名：规范禁止，必须拒绝 */
    const mqtt_connect_t c_pw_only = {"gw01", NULL, NULL, NULL, "p", 60u};
    if (mqtt_build_connect(&c_pw_only, out, (uint16_t)sizeof(out)) != -1)
    {
        printf("[FAIL] password 无 username 应返回 -1\n");
        ++failed;
    }

    /* ④ 遗嘱主题/消息只给一个：成对约束被破坏，必须拒绝 */
    const mqtt_connect_t c_will_half = {"gw01", "wt", NULL, NULL, NULL, 60u};
    if (mqtt_build_connect(&c_will_half, out, (uint16_t)sizeof(out)) != -1)
    {
        printf("[FAIL] 遗嘱只给 topic 应返回 -1\n");
        ++failed;
    }

    /* ⑤ 容量少 1 字节（需要 24，只给 23） */
    if (mqtt_build_connect(&c_user, out, 23u) != -1)
    {
        printf("[FAIL] 载荷扩展帧 cap 少 1 字节 应返回 -1\n");
        ++failed;
    }

    /* ⑥ 空结构体指针 */
    if (mqtt_build_connect(NULL, out, (uint16_t)sizeof(out)) != -1)
    {
        printf("[FAIL] connect(NULL) 应返回 -1\n");
        ++failed;
    }

    return failed;
}

/* 笔记 19 §一/§三哨兵：topic = "gw01/data"、payload = {"t":25.3}、pkt_id = 1。 */
static const uint8_t k_payload[] = {
    '{', '"', 't', '"', ':', '2', '5', '.', '3', '}',
};

/* SUBSCRIBE：82 0E 00 01 00 09 <topic> 00（16 B） */
static const uint8_t k_subscribe[] = {
    0x82u, 0x0Eu,
    0x00u, 0x01u,
    0x00u, 0x09u, 'g', 'w', '0', '1', '/', 'd', 'a', 't', 'a',
    0x00u,
};

/* PUBLISH QoS0：30 15 00 09 <topic> <payload>（23 B） */
static const uint8_t k_publish_qos0[] = {
    0x30u, 0x15u,
    0x00u, 0x09u, 'g', 'w', '0', '1', '/', 'd', 'a', 't', 'a',
    '{', '"', 't', '"', ':', '2', '5', '.', '3', '}',
};

/* PUBLISH QoS1：32 17 00 09 <topic> 00 01 <payload>（25 B） */
static const uint8_t k_publish_qos1[] = {
    0x32u, 0x17u,
    0x00u, 0x09u, 'g', 'w', '0', '1', '/', 'd', 'a', 't', 'a',
    0x00u, 0x01u,
    '{', '"', 't', '"', ':', '2', '5', '.', '3', '}',
};

/* SUBSCRIBE / PUBLISH / PUBACK 的组帧与负向用例。 */
static int test_pubsub(void)
{
    int failed = 0;
    uint8_t out[64];
    int n = 0;
    int ok = 0;

    /* ① SUBSCRIBE 哨兵（16 B） */
    memset(out, 0, sizeof(out));
    n = mqtt_build_subscribe("gw01/data", 0u, 1u, out, (uint16_t)sizeof(out));
    ok = (n == (int)sizeof(k_subscribe)) &&
         (memcmp(out, k_subscribe, sizeof(k_subscribe)) == 0);
    if (!ok)
    {
        ++failed;
    }
    printf("[%s] SUBSCRIBE qos0       -> ", ok ? "PASS" : "FAIL");
    dump_hex(out, (n > 0) ? n : 0);
    printf("  (%d B)\n", n);

    /* ② PUBLISH QoS0 哨兵（23 B） */
    memset(out, 0, sizeof(out));
    n = mqtt_build_publish("gw01/data", k_payload, (uint16_t)sizeof(k_payload), 0u, 0u,
                           out, (uint16_t)sizeof(out));
    ok = (n == (int)sizeof(k_publish_qos0)) &&
         (memcmp(out, k_publish_qos0, sizeof(k_publish_qos0)) == 0);
    if (!ok)
    {
        ++failed;
    }
    printf("[%s] PUBLISH  qos0       -> ", ok ? "PASS" : "FAIL");
    dump_hex(out, (n > 0) ? n : 0);
    printf("  (%d B)\n", n);

    /* ③ PUBLISH QoS1 哨兵（25 B，比 QoS0 多 00 01） */
    memset(out, 0, sizeof(out));
    n = mqtt_build_publish("gw01/data", k_payload, (uint16_t)sizeof(k_payload), 1u, 1u,
                           out, (uint16_t)sizeof(out));
    ok = (n == (int)sizeof(k_publish_qos1)) &&
         (memcmp(out, k_publish_qos1, sizeof(k_publish_qos1)) == 0);
    if (!ok)
    {
        ++failed;
    }
    printf("[%s] PUBLISH  qos1       -> ", ok ? "PASS" : "FAIL");
    dump_hex(out, (n > 0) ? n : 0);
    printf("  (%d B)\n", n);

    /* ④ PUBACK 哨兵（4 B 定长） */
    memset(out, 0, sizeof(out));
    n = mqtt_build_puback(1u, out, (uint16_t)sizeof(out));
    ok = (n == 4) && (out[0] == 0x40u) && (out[1] == 0x02u) && (out[2] == 0x00u) &&
         (out[3] == 0x01u);
    if (!ok)
    {
        ++failed;
    }
    printf("[%s] PUBACK  id=1        -> ", ok ? "PASS" : "FAIL");
    dump_hex(out, (n > 0) ? n : 0);
    printf("  (%d B)\n", n);

    /* ⑤ 负向：QoS2 不支持 */
    if (mqtt_build_subscribe("a", 2u, 1u, out, (uint16_t)sizeof(out)) != -1 ||
        mqtt_build_publish("a", k_payload, 1u, 2u, 1u, out, (uint16_t)sizeof(out)) != -1)
    {
        printf("[FAIL] QoS2 应返回 -1\n");
        ++failed;
    }

    /* ⑥ 负向：报文标识符为 0（SUBSCRIBE 恒需要，PUBLISH 仅 QoS>0 需要） */
    if (mqtt_build_subscribe("a", 0u, 0u, out, (uint16_t)sizeof(out)) != -1 ||
        mqtt_build_publish("a", k_payload, 1u, 1u, 0u, out, (uint16_t)sizeof(out)) != -1 ||
        mqtt_build_puback(0u, out, (uint16_t)sizeof(out)) != -1)
    {
        printf("[FAIL] pkt_id=0 应返回 -1\n");
        ++failed;
    }

    /* ⑦ 负向：载荷长度非 0 但缓冲区为 NULL */
    if (mqtt_build_publish("a", NULL, 1u, 0u, 0u, out, (uint16_t)sizeof(out)) != -1)
    {
        printf("[FAIL] publish(msg=NULL, mlen>0) 应返回 -1\n");
        ++failed;
    }

    /* ⑧ 负向：容量少 1 字节（QoS0 需要 23，只给 22） */
    if (mqtt_build_publish("gw01/data", k_payload, (uint16_t)sizeof(k_payload), 0u, 0u,
                           out, 22u) != -1)
    {
        printf("[FAIL] publish cap 少 1 字节 应返回 -1\n");
        ++failed;
    }

    return failed;
}

/* 笔记 20 §一：心跳三兄弟里的两个「客户端发」的报文，都是 2 字节。 */
static const uint8_t k_pingreq[] = {0xC0u, 0x00u};
static const uint8_t k_disconnect[] = {0xE0u, 0x00u};

/* 笔记 20 §二：Broker 回包哨兵。 */
static const uint8_t k_connack_ok[] = {0x20u, 0x02u, 0x00u, 0x00u};
static const uint8_t k_connack_unauth[] = {0x20u, 0x02u, 0x00u, 0x05u};
static const uint8_t k_suback[] = {0x90u, 0x03u, 0x00u, 0x01u, 0x00u};
static const uint8_t k_puback[] = {0x40u, 0x02u, 0x00u, 0x01u};
static const uint8_t k_pingresp[] = {0xD0u, 0x00u};

/* 半帧用例：SUBACK 声明 RL=3，但只到了 2 字节正文；RL 本身也只到了一半。 */
static const uint8_t k_suback_half[] = {0x90u, 0x03u, 0x00u, 0x01u};
static const uint8_t k_rl_half[] = {0x90u, 0x80u};

typedef struct
{
    uint8_t code;      /* CONNACK 返回码 */
    const char *want;  /* 期望的中文说明 */
} connack_reason_t;

/* 规范 §3.2.2.3 的 6 个返回码 + 1 个未知码。 */
static const connack_reason_t k_reasons[] = {
    {0x00u, "连接成功"},
    {0x01u, "协议版本不支持"},
    {0x02u, "ClientID 被拒"},
    {0x03u, "服务不可用"},
    {0x04u, "用户名或密码错误"},
    {0x05u, "未授权"},
    {0x06u, "未知返回码"},
};

/* 分类断言：类型、可变头起点、RL 值三项都对才算 PASS。 */
static int expect_packet(const char *name, const uint8_t *buf, uint16_t len,
                         mqtt_pkt_t want, uint16_t want_off, uint16_t want_len)
{
    uint16_t off = 0xFFFFu;
    uint16_t blen = 0xFFFFu;
    mqtt_pkt_t got = mqtt_classify(buf, len, &off, &blen);
    int ok = (got == want) && (off == want_off) && (blen == want_len);
    printf("[%s] RX %-20s -> type=%d off=%u len=%u\n", ok ? "PASS" : "FAIL", name,
           (int)got, (unsigned)off, (unsigned)blen);
    return ok ? 0 : 1;
}

/* 笔记 20 §一/§二/§三：心跳组帧、收包分类、CONNACK 映射、心跳到期判断。 */
static int test_heartbeat_and_rx(void)
{
    int failed = 0;
    uint8_t out[8];

    /* ① PINGREQ 哨兵：C0 00 */
    memset(out, 0, sizeof(out));
    int n = mqtt_pingreq(out, (uint16_t)sizeof(out));
    int ok = (n == 2) && (memcmp(out, k_pingreq, sizeof(k_pingreq)) == 0);
    if (!ok)
    {
        ++failed;
    }
    printf("[%s] PINGREQ             -> ", ok ? "PASS" : "FAIL");
    dump_hex(out, (n > 0) ? n : 0);
    printf("  (%d B)\n", n);

    /* ② DISCONNECT 哨兵：E0 00 */
    memset(out, 0, sizeof(out));
    n = mqtt_disconnect(out, (uint16_t)sizeof(out));
    ok = (n == 2) && (memcmp(out, k_disconnect, sizeof(k_disconnect)) == 0);
    if (!ok)
    {
        ++failed;
    }
    printf("[%s] DISCONNECT          -> ", ok ? "PASS" : "FAIL");
    dump_hex(out, (n > 0) ? n : 0);
    printf("  (%d B)\n", n);

    /* ③ 容量不足 / 空指针必须拒绝 */
    if (mqtt_pingreq(out, 1u) != -1 || mqtt_pingreq(NULL, 8u) != -1 ||
        mqtt_disconnect(out, 1u) != -1 || mqtt_disconnect(NULL, 8u) != -1)
    {
        printf("[FAIL] PINGREQ/DISCONNECT 参数非法应返回 -1\n");
        ++failed;
    }

    /* ④ 5 类回包分类：body_off 一律是 2（首字节 + 1 字节 RL），body_len 就是 RL 值 */
    failed += expect_packet("CONNACK ok", k_connack_ok, (uint16_t)sizeof(k_connack_ok),
                            MQTT_PKT_CONNACK, 2u, 2u);
    failed += expect_packet("CONNACK 0x05", k_connack_unauth, (uint16_t)sizeof(k_connack_unauth),
                            MQTT_PKT_CONNACK, 2u, 2u);
    failed += expect_packet("SUBACK", k_suback, (uint16_t)sizeof(k_suback),
                            MQTT_PKT_SUBACK, 2u, 3u);
    failed += expect_packet("PUBACK", k_puback, (uint16_t)sizeof(k_puback),
                            MQTT_PKT_PUBACK, 2u, 2u);
    failed += expect_packet("PINGRESP", k_pingresp, (uint16_t)sizeof(k_pingresp),
                            MQTT_PKT_PINGRESP, 2u, 0u);
    failed += expect_packet("PUBLISH qos0", k_publish_qos0, (uint16_t)sizeof(k_publish_qos0),
                            MQTT_PKT_PUBLISH, 2u, 0x15u);
    failed += expect_packet("PUBLISH qos1", k_publish_qos1, (uint16_t)sizeof(k_publish_qos1),
                            MQTT_PKT_PUBLISH, 2u, 0x17u);

    /* ⑤ 半帧 / 短包 / 空指针：必须返回 UNKNOWN，不能越界读 */
    failed += expect_packet("SUBACK 半帧", k_suback_half, (uint16_t)sizeof(k_suback_half),
                            MQTT_PKT_UNKNOWN, 0u, 0u);
    failed += expect_packet("RL 半帧", k_rl_half, (uint16_t)sizeof(k_rl_half),
                            MQTT_PKT_UNKNOWN, 0u, 0u);
    failed += expect_packet("1 字节", k_pingresp, 1u, MQTT_PKT_UNKNOWN, 0u, 0u);
    failed += expect_packet("NULL buf", NULL, 2u, MQTT_PKT_UNKNOWN, 0u, 0u);

    /* ⑥ body_off / body_len 允许传 NULL，只取类型 */
    if (mqtt_classify(k_connack_ok, (uint16_t)sizeof(k_connack_ok), NULL, NULL) !=
        MQTT_PKT_CONNACK)
    {
        printf("[FAIL] classify(NULL, NULL) 应只返回类型\n");
        ++failed;
    }

    /* ⑦ CONNACK 6 个返回码 + 未知码的映射 */
    int reason_count = (int)(sizeof(k_reasons) / sizeof(k_reasons[0]));
    ok = 1;
    for (int i = 0; i < reason_count; ++i)
    {
        if (strcmp(mqtt_connack_reason(k_reasons[i].code), k_reasons[i].want) != 0)
        {
            printf("[FAIL] CONNACK 0x%02X -> %s\n", k_reasons[i].code,
                   mqtt_connack_reason(k_reasons[i].code));
            ok = 0;
        }
    }
    if (!ok)
    {
        ++failed;
    }
    printf("[%s] CONNACK 返回码映射   -> %d 项\n", ok ? "PASS" : "FAIL", reason_count);

    /* ⑧ 心跳到期：KeepAlive/2 触发，且正确处理 tick 回绕 */
    typedef struct
    {
        uint32_t now;
        uint32_t last;
        uint16_t keepalive;
        int want;
    } keepalive_case_t;

    static const keepalive_case_t k_cases[] = {
        {0u, 0u, 60u, 0},                              /* 刚发包，未到期 */
        {29999u, 0u, 60u, 0},                          /* 差 1 ms，仍未到期 */
        {30000u, 0u, 60u, 1},                          /* 正好到 KeepAlive/2 */
        {30000u, 0u, 0u, 0},                           /* KeepAlive=0：不启用心跳 */
        {0x00000100u, 0xFFFFFF00u, 60u, 0},            /* 回绕后仅过 0x200 ms */
        {0x00008000u, 0xFFFFFF00u, 60u, 1},            /* 回绕后已过 0x8100 ms */
    };

    int case_count = (int)(sizeof(k_cases) / sizeof(k_cases[0]));
    ok = 1;
    for (int i = 0; i < case_count; ++i)
    {
        int due = mqtt_keepalive_due(k_cases[i].now, k_cases[i].last, k_cases[i].keepalive);
        if (due != k_cases[i].want)
        {
            printf("[FAIL] keepalive_due(now=%u, last=%u, ka=%u) = %d，期望 %d\n",
                   (unsigned)k_cases[i].now, (unsigned)k_cases[i].last,
                   (unsigned)k_cases[i].keepalive, due, k_cases[i].want);
            ok = 0;
        }
    }
    if (!ok)
    {
        ++failed;
    }
    printf("[%s] 心跳到期判断          -> %d 项\n", ok ? "PASS" : "FAIL", case_count);

    return failed;
}

int main(void)
{
    int failed = 0;
    int count = (int)(sizeof(k_vectors) / sizeof(k_vectors[0]));

    for (int i = 0; i < count; ++i)
    {
        const rl_vector_t *v = &k_vectors[i];
        uint8_t out[MQTT_RL_MAX_BYTES] = {0};
        int used = 0;

        /* 先编码，再解码，做一次往返校验。 */
        int n = mqtt_rl_encode(v->value, out);
        uint32_t decoded = mqtt_rl_decode(v->encoded, &used);

        /* 四项都通过才算 PASS：编码字节数、编码内容、解码字节数、解码值。 */
        int ok = (n == v->len) &&
                 (memcmp(out, v->encoded, (size_t)v->len) == 0) &&
                 (used == v->len) &&
                 (decoded == v->value);
        if (!ok)
        {
            ++failed;
        }

        printf("[%s] %10u -> ", ok ? "PASS" : "FAIL", (unsigned)v->value);
        dump_hex(out, (n > 0) ? n : 0);
        printf("  (decode=%u, used=%d)\n", (unsigned)decoded, used);
    }

    /* RL 向量跑完，接着验证 CONNECT 组帧（S16）与载荷扩展（S17）。 */
    failed += test_connect();
    failed += test_connect_payload();
    failed += test_pubsub();
    failed += test_heartbeat_and_rx();

    /* 负向用例 1：超过 4 字节上限的值必须拒绝，防止静默截断。 */
    uint8_t dummy[MQTT_RL_MAX_BYTES] = {0};
    if (mqtt_rl_encode(MQTT_RL_MAX_VALUE + 1u, dummy) != -1)
    {
        printf("[FAIL] encode(MQTT_RL_MAX_VALUE + 1) 应返回 -1\n");
        ++failed;
    }

    /* 负向用例 2：输出缓冲区为空必须拒绝，防止空指针解引用。 */
    if (mqtt_rl_encode(0u, NULL) != -1)
    {
        printf("[FAIL] encode(NULL) 应返回 -1\n");
        ++failed;
    }

    printf("\nRESULT: %s\n", (failed == 0) ? "ALL PASS" : "HAS FAILURES");
    return (failed == 0) ? 0 : 1;
}