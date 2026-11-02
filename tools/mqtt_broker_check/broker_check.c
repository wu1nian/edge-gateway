/**
 * @file broker_check.c
 * @brief W3 · S20 真 broker 验证：把 mqtt_codec 手拼的报文喂给真实 broker。
 *
 * 验证链路（正向）：
 *   TCP → CONNECT → CONNACK(0x00) → SUBSCRIBE → SUBACK
 *       → PUBLISH QoS1 → PUBACK → broker 回显 PUBLISH
 *       → PINGREQ → PINGRESP → DISCONNECT
 *
 * 反例（默认执行）：
 *   1. 协议级别 0x03：broker 应返回非 0 返回码或断开；
 *   2. RL 少 1 字节：broker 应等待/断开，不能当成功；
 *   3. CONNECT 标志 reserved 位置 1：broker 应拒绝；
 *   4. SUBSCRIBE 首字节 0x80：broker 应拒绝（不是「没反应」）。
 *
 * 编译运行见 build.bat / run.bat；默认连 127.0.0.1:1883。
 * 没有本地 Mosquitto 时，可显式指定公网 broker：
 *   broker_check.exe broker.emqx.io 1883
 *
 * 本工具只依赖 Winsock + mqtt_codec，不依赖 HAL / FreeRTOS。
 */

#define WIN32_LEAN_AND_MEAN
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600 /* MinGW 6.3 needs this to expose getaddrinfo */
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "mqtt_codec.h"

#define FRAME_CAP 512
#define IO_TIMEOUT_MS 3000
#define NEG_TIMEOUT_MS 1200

static void dump_hex(const uint8_t *buf, int len)
{
    for (int i = 0; i < len; ++i)
    {
        printf("%02X%s", buf[i], (i + 1 < len) ? " " : "");
    }
}

/* recv 的三种结果：>0 字节数；-1 对端关闭/错误；-2 超时。 */
static int recv_exact(SOCKET s, uint8_t *buf, int len)
{
    int got = 0;
    while (got < len)
    {
        int n = recv(s, (char *)buf + got, len - got, 0);
        if (n == 0)
        {
            return -1;
        }
        if (n == SOCKET_ERROR)
        {
            return (WSAGetLastError() == WSAETIMEDOUT) ? -2 : -1;
        }
        got += n;
    }
    return got;
}

static int send_all(SOCKET s, const uint8_t *buf, int len)
{
    int sent = 0;
    while (sent < len)
    {
        int n = send(s, (const char *)buf + sent, len - sent, 0);
        if (n == SOCKET_ERROR || n == 0)
        {
            return -1;
        }
        sent += n;
    }
    return 0;
}

/**
 * @brief 从 TCP 字节流收一整帧（首字节 + RL + 剩余）。
 * @return >0 整帧长度；-1 对端关闭/错误；-2 超时；-3 帧过长或 RL 非法。
 */
static int recv_frame(SOCKET s, uint8_t *buf, int cap)
{
    int n = recv_exact(s, buf, 1);
    if (n <= 0)
    {
        return n;
    }

    int pos = 1;
    uint32_t rl = 0u;
    uint32_t mult = 1u;
    int rl_bytes = 0;

    for (;;)
    {
        uint8_t b = 0u;
        n = recv_exact(s, &b, 1);
        if (n <= 0)
        {
            return n;
        }
        if (pos >= cap)
        {
            return -3;
        }
        buf[pos++] = b;
        rl += (uint32_t)(b & 0x7Fu) * mult;
        ++rl_bytes;

        if ((b & 0x80u) == 0u)
        {
            break;
        }
        if (rl_bytes >= (int)MQTT_RL_MAX_BYTES)
        {
            return -3;
        }
        mult *= 128u;
    }

    if ((uint32_t)pos + rl > (uint32_t)cap)
    {
        return -3;
    }
    if (rl > 0u)
    {
        n = recv_exact(s, &buf[pos], (int)rl);
        if (n <= 0)
        {
            return n;
        }
        pos += (int)rl;
    }
    return pos;
}

static SOCKET tcp_connect(const char *host, const char *port, char *err, size_t errlen)
{
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    struct addrinfo *it = NULL;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    int rc = getaddrinfo(host, port, &hints, &res);
    if (rc != 0)
    {
        snprintf(err, errlen, "getaddrinfo(%s:%s) failed: %d", host, port, rc);
        return INVALID_SOCKET;
    }

    SOCKET s = INVALID_SOCKET;
    for (it = res; it != NULL; it = it->ai_next)
    {
        s = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (s == INVALID_SOCKET)
        {
            continue;
        }
        if (connect(s, it->ai_addr, (int)it->ai_addrlen) == 0)
        {
            break;
        }
        closesocket(s);
        s = INVALID_SOCKET;
    }
    freeaddrinfo(res);

    if (s == INVALID_SOCKET)
    {
        snprintf(err, errlen, "connect(%s:%s) failed", host, port);
        return INVALID_SOCKET;
    }

    DWORD timeout = IO_TIMEOUT_MS;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));
    BOOL nodelay = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&nodelay, sizeof(nodelay));

    return s;
}

/* CONNACK 期望：body = 会话标志(1) + 返回码(1)。 */
static int is_connack_ok(const uint8_t *frame, int len, uint8_t *ret_code)
{
    uint16_t off = 0u;
    uint16_t blen = 0u;
    if (mqtt_classify(frame, (uint16_t)len, &off, &blen) != MQTT_PKT_CONNACK || blen != 2u)
    {
        return 0;
    }
    if (ret_code != NULL)
    {
        *ret_code = frame[off + 1u];
    }
    return frame[off + 1u] == 0x00u;
}

/* 收到的是不是「我们订的那个主题 + 那段载荷」。 */
static int is_expected_publish(const uint8_t *frame, int len, const char *topic,
                               const char *payload)
{
    uint16_t off = 0u;
    uint16_t blen = 0u;
    if (mqtt_classify(frame, (uint16_t)len, &off, &blen) != MQTT_PKT_PUBLISH)
    {
        return 0;
    }

    const uint8_t *body = frame + off;
    if (blen < 2u)
    {
        return 0;
    }
    uint16_t tlen = (uint16_t)(((uint16_t)body[0] << 8) | body[1]);
    if ((uint32_t)tlen + 2u > blen)
    {
        return 0;
    }
    if (tlen != (uint16_t)strlen(topic) || memcmp(body + 2, topic, tlen) != 0)
    {
        return 0;
    }

    int pos = 2 + (int)tlen;
    uint8_t qos = (uint8_t)((frame[0] >> 1) & 0x03u);
    if (qos > 0u)
    {
        pos += 2; /* QoS>0 时主题后面还有报文标识符 */
    }
    if (pos > (int)blen)
    {
        return 0;
    }
    int plen = (int)blen - pos;
    return (plen == (int)strlen(payload)) && (memcmp(body + pos, payload, (size_t)plen) == 0);
}

static int positive_test(const char *host, const char *port, char *err, size_t errlen)
{
    int failed = 0;
    uint8_t frame[FRAME_CAP];
    uint8_t rx[FRAME_CAP];
    char client_id[32];

    snprintf(client_id, sizeof(client_id), "gw01chk-%lu", (unsigned long)GetCurrentProcessId());

    SOCKET s = tcp_connect(host, port, err, errlen);
    if (s == INVALID_SOCKET)
    {
        printf("[FAIL] TCP connect: %s\n", err);
        return 1;
    }
    printf("[PASS] TCP connect -> %s:%s\n", host, port);

    /* ① CONNECT → CONNACK 0x00 */
    int n = mqtt_build_connect_head(client_id, 60u, frame, (uint16_t)sizeof(frame));
    uint8_t code = 0xFFu;
    int got = 0;
    if (n > 0 && send_all(s, frame, n) == 0)
    {
        int rn = recv_frame(s, rx, (int)sizeof(rx));
        got = (rn > 0) ? is_connack_ok(rx, rn, &code) : 0;
        printf("[%s] CONNECT -> ", got ? "PASS" : "FAIL");
        if (rn > 0)
        {
            dump_hex(rx, rn);
            printf("  (ret=0x%02X, client_id=%s)\n", code, client_id);
        }
        else
        {
            printf("recv=%d\n", rn);
        }
    }
    if (!got)
    {
        ++failed;
        closesocket(s);
        return failed;
    }

    /* ② SUBSCRIBE → SUBACK 0x00（先订阅，才能验证 broker 回显） */
    n = mqtt_build_subscribe("gw01/data", 0u, 1u, frame, (uint16_t)sizeof(frame));
    got = 0;
    if (n > 0 && send_all(s, frame, n) == 0)
    {
        int rn = recv_frame(s, rx, (int)sizeof(rx));
        uint16_t off = 0u;
        uint16_t blen = 0u;
        mqtt_pkt_t type = (rn > 0) ? mqtt_classify(rx, (uint16_t)rn, &off, &blen) : MQTT_PKT_UNKNOWN;
        got = (type == MQTT_PKT_SUBACK) && (blen == 3u) && (rx[off] == 0x00u) &&
              (rx[off + 1u] == 0x01u) && (rx[off + 2u] == 0x00u);
        printf("[%s] SUBSCRIBE -> ", got ? "PASS" : "FAIL");
        if (rn > 0)
        {
            dump_hex(rx, rn);
            printf("  (%d B)\n", rn);
        }
        else
        {
            printf("recv=%d\n", rn);
        }
    }
    if (!got)
    {
        ++failed;
        closesocket(s);
        return failed;
    }

    /* ③ PUBLISH QoS1 → PUBACK；同时应收到 broker 回显的 PUBLISH */
    static const char k_payload[] = "{\"t\":25.3}";
    n = mqtt_build_publish("gw01/data", (const uint8_t *)k_payload,
                           (uint16_t)(sizeof(k_payload) - 1u), 1u, 1u,
                           frame, (uint16_t)sizeof(frame));
    int got_puback = 0;
    int got_echo = 0;
    if (n > 0 && send_all(s, frame, n) == 0)
    {
        for (int i = 0; i < 8 && !(got_puback && got_echo); ++i)
        {
            int rn = recv_frame(s, rx, (int)sizeof(rx));
            if (rn <= 0)
            {
                break;
            }
            uint16_t off = 0u;
            uint16_t blen = 0u;
            mqtt_pkt_t type = mqtt_classify(rx, (uint16_t)rn, &off, &blen);
            if (type == MQTT_PKT_PUBACK && blen == 2u && rx[off] == 0x00u && rx[off + 1u] == 0x01u)
            {
                got_puback = 1;
                printf("[PASS] PUBLISH qos1 -> PUBACK ");
                dump_hex(rx, rn);
                printf("\n");
            }
            else if (type == MQTT_PKT_PUBLISH && is_expected_publish(rx, rn, "gw01/data", k_payload))
            {
                got_echo = 1;
                printf("[PASS] broker echo -> PUBLISH topic=gw01/data payload=%s\n", k_payload);
            }
        }
    }
    if (!got_puback)
    {
        printf("[FAIL] PUBLISH qos1 -> no PUBACK\n");
        ++failed;
    }
    if (!got_echo)
    {
        printf("[FAIL] broker echo -> no PUBLISH\n");
        ++failed;
    }

    /*
     * ④ PINGREQ → PINGRESP（跳过公网主题上可能混进来的无关 PUBLISH）
     *
     * 注意：③ 已在同一主题上产生过回显 PUBLISH。公网 broker 的推送时序可能
     * 让它迟到，或同时有别的客户端在本主题上发言。若这些帧还排在接收队列里，
     * 本循环会先读到它们；等队列被读空后，若 PINGREQ 的应答还没跟上，就会
     * 落到 SO_RCVTIMEO（3 s）超时，表现为「PINGREQ -> recv=-2」。
     *
     * 处理：发完 PINGREQ 后先用非阻塞读把「已在队列里排队」的旧帧读掉，
     * 之后等待的帧才是对本次 PINGREQ 的新应答。
     */
    n = mqtt_pingreq(frame, (uint16_t)sizeof(frame));
    got = 0;
    int rn = 0;
    if (n > 0 && send_all(s, frame, n) == 0)
    {
        /* 先非阻塞地清掉发送前就已经在队列里的帧（最多 8 帧，防死循环）。 */
        u_long nonblock = 1;
        ioctlsocket(s, FIONBIO, &nonblock);
        for (int drained = 0; drained < 8; ++drained)
        {
            int d = recv_frame(s, rx, (int)sizeof(rx));
            if (d <= 0)
            {
                break; /* -1 关闭 / -2 队列空，都停 */
            }
        }
        nonblock = 0;
        ioctlsocket(s, FIONBIO, &nonblock);

        for (int i = 0; i < 8 && !got; ++i)
        {
            rn = recv_frame(s, rx, (int)sizeof(rx));
            if (rn <= 0)
            {
                break;
            }
            uint16_t off = 0u;
            uint16_t blen = 0u;
            if (mqtt_classify(rx, (uint16_t)rn, &off, &blen) == MQTT_PKT_PINGRESP)
            {
                got = 1;
                printf("[PASS] PINGREQ -> ");
                dump_hex(rx, rn);
                printf("  (%d B)\n", rn);
            }
        }
    }
    if (!got)
    {
        printf("[FAIL] PINGREQ -> recv=%d\n", rn);
        ++failed;
    }

    /* ⑤ DISCONNECT 是单向报文，发完即关。 */
    n = mqtt_disconnect(frame, (uint16_t)sizeof(frame));
    if (n > 0 && send_all(s, frame, n) == 0)
    {
        printf("[PASS] DISCONNECT sent ");
        dump_hex(frame, n);
        printf("\n");
    }
    else
    {
        printf("[FAIL] DISCONNECT send\n");
        ++failed;
    }

    closesocket(s);
    return failed;
}

/* 反例统一判定：没有「成功」证据就算通过（不同 broker 可能回非 0 码或直接断开）。 */
static int negative_connect_case(const char *host, const char *port, const char *name,
                                 const char *client_id,
                                 int mutate_offset, uint8_t mutate_value,
                                 int expect_no_response, char *err, size_t errlen)
{
    uint8_t frame[FRAME_CAP];
    uint8_t rx[FRAME_CAP];
    SOCKET s = tcp_connect(host, port, err, errlen);
    if (s == INVALID_SOCKET)
    {
        printf("[FAIL] %s: TCP connect failed (%s)\n", name, err);
        return 1;
    }

    int n = mqtt_build_connect_head(client_id, 60u, frame, (uint16_t)sizeof(frame));
    if (n <= 0)
    {
        closesocket(s);
        printf("[FAIL] %s: build CONNECT failed\n", name);
        return 1;
    }
    if (mutate_offset > 0 && mutate_offset < n)
    {
        frame[mutate_offset] = mutate_value;
    }

    DWORD timeout = NEG_TIMEOUT_MS;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));

    int pass = 0;
    if (send_all(s, frame, n) == 0)
    {
        int rn = recv_frame(s, rx, (int)sizeof(rx));
        if (rn == -2)
        {
            /* 超时：broker 在等更多字节，正是 RL 少 1 字节时的预期。 */
            pass = expect_no_response;
            printf("[%s] %s -> no response in %d ms (timeout)\n",
                   pass ? "PASS" : "FAIL", name, NEG_TIMEOUT_MS);
        }
        else if (rn == -1)
        {
            pass = 1; /* 对端主动断开：也是拒绝 */
            printf("[PASS] %s -> connection closed\n", name);
        }
        else if (rn > 0)
        {
            uint16_t off = 0u;
            uint16_t blen = 0u;
            mqtt_pkt_t type = mqtt_classify(rx, (uint16_t)rn, &off, &blen);
            if (type == MQTT_PKT_CONNACK && blen == 2u && rx[off + 1u] != 0x00u)
            {
                pass = 1;
                printf("[PASS] %s -> CONNACK ret=0x%02X (rejected)\n", name, rx[off + 1u]);
            }
            else
            {
                printf("[FAIL] %s -> unexpected frame: ", name);
                dump_hex(rx, rn);
                printf("\n");
            }
        }
        else
        {
            printf("[FAIL] %s -> recv_frame=%d\n", name, rn);
        }
    }

    closesocket(s);
    return pass ? 0 : 1;
}

static int negative_subscribe_case(const char *host, const char *port, char *err, size_t errlen)
{
    uint8_t frame[FRAME_CAP];
    uint8_t rx[FRAME_CAP];
    SOCKET s = tcp_connect(host, port, err, errlen);
    if (s == INVALID_SOCKET)
    {
        printf("[FAIL] SUBSCRIBE 0x80: TCP connect failed (%s)\n", err);
        return 1;
    }

    int n = mqtt_build_connect_head("gw04", 60u, frame, (uint16_t)sizeof(frame));
    if (n <= 0 || send_all(s, frame, n) != 0 || recv_frame(s, rx, (int)sizeof(rx)) <= 0)
    {
        closesocket(s);
        printf("[FAIL] SUBSCRIBE 0x80: CONNECT handshake failed\n");
        return 1;
    }

    n = mqtt_build_subscribe("gw01/data", 0u, 1u, frame, (uint16_t)sizeof(frame));
    if (n <= 0)
    {
        closesocket(s);
        printf("[FAIL] SUBSCRIBE 0x80: build SUBSCRIBE failed\n");
        return 1;
    }
    frame[0] = 0x80u; /* 故意把强制低 4 位 2 改成 0 */

    DWORD timeout = NEG_TIMEOUT_MS;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));

    int pass = 0;
    if (send_all(s, frame, n) == 0)
    {
        int rn = recv_frame(s, rx, (int)sizeof(rx));
        if (rn == -1)
        {
            pass = 1;
            printf("[PASS] SUBSCRIBE 0x80 -> connection closed\n");
        }
        else if (rn == -2)
        {
            pass = 1;
            printf("[PASS] SUBSCRIBE 0x80 -> no SUBACK in %d ms\n", NEG_TIMEOUT_MS);
        }
        else if (rn > 0)
        {
            printf("[FAIL] SUBSCRIBE 0x80 -> unexpected frame: ");
            dump_hex(rx, rn);
            printf("\n");
        }
        else
        {
            printf("[FAIL] SUBSCRIBE 0x80 -> recv_frame=%d\n", rn);
        }
    }

    closesocket(s);
    return pass ? 0 : 1;
}

int main(int argc, char **argv)
{
    const char *host = "127.0.0.1";
    const char *port = "1883";
    int positive_only = 0;
    int positional = 0;

    for (int i = 1; i < argc; ++i)
    {
        if (strcmp(argv[i], "--positive-only") == 0)
        {
            positive_only = 1;
            continue;
        }
        if (positional == 0)
        {
            host = argv[i];
        }
        else if (positional == 1)
        {
            port = argv[i];
        }
        ++positional;
    }

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
    {
        printf("WSAStartup failed\n");
        return 2;
    }

    printf("== MQTT real-broker check: %s:%s ==\n", host, port);

    char err[256] = {0};
    int failed = positive_test(host, port, err, sizeof(err));

    if (!positive_only)
    {
        printf("\n-- negative cases --\n");
        /* CONNECT 帧结构：0x10, RL, 00 04 MQTT, 级别(8), 标志(9), ... */
        failed += negative_connect_case(host, port, "protocol level 0x03", "gw01",
                                        8, 0x03u, 0, err, sizeof(err));
        failed += negative_connect_case(host, port, "RL short by 1", "gw02",
                                        1, 0x0Fu, 1, err, sizeof(err));
        failed += negative_connect_case(host, port, "reserved bit set", "gw03",
                                        9, 0x03u, 0, err, sizeof(err));
        failed += negative_subscribe_case(host, port, err, sizeof(err));
    }

    WSACleanup();
    printf("\nRESULT: %s\n", (failed == 0) ? "ALL PASS" : "HAS FAILURES");
    return (failed == 0) ? 0 : 1;
}