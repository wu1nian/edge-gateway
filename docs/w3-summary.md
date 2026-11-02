# W3 阶段总结：MQTT 3.1.1 手写与真 broker 验证

周期是 S15 到 S21，七个单元，10 月 31 日做完，11 月 2 日又整体复核了一遍。
对应提交 `3a08f08`。

一句话概括：MQTT 报文层从零手写完成，30 条向量和三家真实 broker 全部通过，
这一层不依赖 HAL 也不依赖 FreeRTOS，PC 和 STM32 共用同一份源码。

---

## 1. 这一周做了什么

W3 的目标不是「能用上 MQTT」，而是「能徒手拼出报文，并讲清每个字节为什么是这个值」。
所以整周没有引入任何 MQTT 客户端库——不用 Paho，不用 libemqtt，也没走 ESP8266
的 AT 透传，全部按 OASIS MQTT 3.1.1 规范自己实现。

| 单元 | 内容 | 产出 |
|:--|:--|:--|
| S15 | 报文三段结构 + Remaining Length | `mqtt_rl_encode` / `mqtt_rl_decode` |
| S16 | CONNECT 固定头 + 可变头 | `mqtt_build_connect` 前半 |
| S17 | CONNECT 载荷（client_id / user / pass / will） | `mqtt_build_connect` 完整 |
| S18 | SUBSCRIBE + PUBLISH（QoS 0/1） | `mqtt_build_subscribe` / `mqtt_build_publish` / `mqtt_build_puback` |
| S19 | 心跳 + 收包分类 | `mqtt_pingreq` / `mqtt_disconnect` / `mqtt_classify` / `mqtt_connack_reason` / `mqtt_keepalive_due` |
| S20 | 真 broker 验证 | `tools/mqtt_broker_check/` |
| S21 | 阶段验收 + 复盘 | 本文，以及 `docs/mqtt.md` 里的移植边界说明 |

---

## 2. 代码交付物

### 2.1 固件侧

| 文件 | 行数 | 说明 |
|:--|--:|:--|
| `Core/Src/mqtt_codec.c` | 692 | 全部编解码实现，只 include 了 `<stddef.h>` 和 `<string.h>`，没有任何 HAL / FreeRTOS 调用 |
| `Core/Inc/mqtt_codec.h` | 296 | 对外接口和常量 |

对外接口，下面这段是从 `Core/Inc/mqtt_codec.h` 原文抄的：

```c
/* Remaining Length（§2.2.3） */
int      mqtt_rl_encode(uint32_t val, uint8_t *out);
uint32_t mqtt_rl_decode(const uint8_t *buf, int *used);

/* 组帧 */
int      mqtt_build_connect(const mqtt_connect_t *c, uint8_t *out, uint16_t cap);
int      mqtt_build_connect_head(const char *client_id, uint16_t keepalive_s,
                                 uint8_t *out, uint16_t cap);
int      mqtt_build_subscribe(const char *topic, uint8_t qos, uint16_t pkt_id,
                              uint8_t *out, uint16_t cap);
int      mqtt_build_publish(const char *topic, const uint8_t *msg, uint16_t mlen,
                            uint8_t qos, uint16_t pkt_id, uint8_t *out, uint16_t cap);
int      mqtt_build_puback(uint16_t pkt_id, uint8_t *out, uint16_t cap);
int      mqtt_pingreq(uint8_t *out, uint16_t cap);
int      mqtt_disconnect(uint8_t *out, uint16_t cap);

/* 收包与状态 */
mqtt_pkt_t mqtt_classify(const uint8_t *buf, uint16_t len,
                         uint16_t *body_off, uint16_t *body_len);
const char *mqtt_connack_reason(uint8_t code);
int      mqtt_keepalive_due(uint32_t now_ms, uint32_t last_tx_ms, uint16_t keepalive_s);
```

两个关键常量：

```c
#define MQTT_RL_MAX_BYTES 4u          /* 变长整数最多 4 字节 */
#define MQTT_RL_MAX_VALUE 268435455u  /* 0x0FFFFFFF：4 字节能表示的最大值 */
```

把协议层单独摘出来是有目的的。`mqtt_codec.c` 里没有任何 `HAL_*` 调用，没有
`osDelay` / `xQueue*` / `vTask*`，也没有 `printf`，只用到 `NULL`、`strlen`、`memcpy`。
所以它能被 Windows 上的 gcc 直接编译，拿去喂真实 broker。协议错在 PC 上查，
比在板子上查快得多。

### 2.2 验证工具

| 目录 | 作用 | 依赖 |
|:--|:--|:--|
| `tools/mqtt_test/` | 协议层单元测试，30 条向量 | 只要 gcc |
| `tools/mqtt_broker_check/` | Winsock 直连真 broker，跑正例和反例 | Winsock，外加一台 broker |

两个工具的 `build.bat` 都直接引用 `firmware/` 里的 `mqtt_codec.c` 源码路径，
所以不存在「测试用一份、固件用另一份」的分叉。编译参数统一是
`gcc -std=c11 -Wall -Wextra -Werror -O2`，零警告。

---

## 3. 验证证据

### 3.1 协议层单元测试：30 条全过

```bat
cd tools\mqtt_test
build.bat
run.bat
```

```text
[PASS]          0 -> 00  (decode=0, used=1)
[PASS]        127 -> 7F  (decode=127, used=1)
[PASS]        128 -> 80 01  (decode=128, used=2)
[PASS]        321 -> C1 02  (decode=321, used=2)
[PASS]      16383 -> FF 7F  (decode=16383, used=2)
[PASS]      16384 -> 80 80 01  (decode=16384, used=3)
[PASS]  268435455 -> FF FF FF 7F  (decode=268435455, used=4)
[PASS] CONNECT gw01/60  -> 10 10 00 04 4D 51 54 54 04 02 00 3C 00 04 67 77 30 31  (18 B)
[PASS] CONNECT gw01/120 -> 10 10 00 04 4D 51 54 54 04 02 00 78 00 04 67 77 30 31  (18 B)
[PASS] CONNECT +user/pass -> 10 16 00 04 4D 51 54 54 04 C2 00 3C 00 04 67 77 30 31 00 01 75 00 01 70  (24 B)
[PASS] CONNECT +will      -> 10 18 00 04 4D 51 54 54 04 06 00 3C 00 04 67 77 30 31 00 02 77 74 00 02 77 6D  (26 B)
[PASS] SUBSCRIBE qos0       -> 82 0E 00 01 00 09 67 77 30 31 2F 64 61 74 61 00  (16 B)
[PASS] PUBLISH  qos0       -> 30 15 00 09 67 77 30 31 2F 64 61 74 61 7B 22 74 22 3A 32 35 2E 33 7D  (23 B)
[PASS] PUBLISH  qos1       -> 32 17 00 09 67 77 30 31 2F 64 61 74 61 00 01 7B 22 74 22 3A 32 35 2E 33 7D  (25 B)
[PASS] PUBACK  id=1        -> 40 02 00 01  (4 B)
[PASS] PINGREQ             -> C0 00  (2 B)
[PASS] DISCONNECT          -> E0 00  (2 B)
[PASS] RX CONNACK ok           -> type=1 off=2 len=2
[PASS] RX CONNACK 0x05         -> type=1 off=2 len=2
[PASS] RX SUBACK               -> type=2 off=2 len=3
[PASS] RX PUBACK               -> type=4 off=2 len=2
[PASS] RX PINGRESP             -> type=5 off=2 len=0
[PASS] RX PUBLISH qos0         -> type=3 off=2 len=21
[PASS] RX PUBLISH qos1         -> type=3 off=2 len=23
[PASS] RX SUBACK 半帧        -> type=0 off=0 len=0
[PASS] RX RL 半帧            -> type=0 off=0 len=0
[PASS] RX 1 字节             -> type=0 off=0 len=0
[PASS] RX NULL buf             -> type=0 off=0 len=0
[PASS] CONNACK 返回码映射   -> 7 项
[PASS] 心跳到期判断          -> 6 项

RESULT: ALL PASS
```

30 条的分布是：RL 编解码 7 条，组帧 10 条，收包分类 11 条，另外两条是多值断言
（返回码映射 7 项、心跳判断 6 项）。

最后四条「半帧 / 1 字节 / NULL」是故意喂不完整数据，期望 `mqtt_classify` 返回
`type=0`，表示还没凑够一帧。这一组针对的是 TCP 是字节流这件事——一次 `recv`
拿到的既可能是半帧，也可能是两帧。

### 3.2 真 broker：三家实现，结论一致

```bat
cd tools\mqtt_broker_check
build.bat
broker_check.exe 127.0.0.1 1883
```

本机 Mosquitto 2.1.2，正向 7 项：

```text
== MQTT real-broker check: 127.0.0.1:1883 ==
[PASS] TCP connect -> 127.0.0.1:1883
[PASS] CONNECT -> 20 02 00 00  (ret=0x00, client_id=gw01chk-72528)
[PASS] SUBSCRIBE -> 90 03 00 01 00  (5 B)
[PASS] broker echo -> PUBLISH topic=gw01/data payload={"t":25.3}
[PASS] PUBLISH qos1 -> PUBACK 40 02 00 01
[PASS] PINGREQ -> D0 00  (2 B)
[PASS] DISCONNECT sent E0 00

-- negative cases --
[PASS] protocol level 0x03 -> CONNACK ret=0x01 (rejected)
[PASS] RL short by 1 -> connection closed
[PASS] reserved bit set -> connection closed
[PASS] SUBSCRIBE 0x80 -> connection closed

RESULT: ALL PASS
```

公网 HiveMQ 连测 5 次，每次都是 `RESULT: ALL PASS`。公网 EMQX 是首轮测的，
那时候本机还没装 Mosquitto，结果同样是 `RESULT: ALL PASS`。

为什么要找三家。只过一家，存在「碰巧被这一家接受」的可能——broker 实现得宽松，
你发错点什么它也照收。Mosquitto、HiveMQ、EMQX 是三份互相独立的实现，对同一批
字节判定相同，才能说明报文是照着规范对的，不是照着某一家的宽容度对的。

### 3.3 反例：确认 broker 真的在判

正向用例只能证明「我以为对的它接受了」。如果这个 broker 对什么都点头，正向测试
其实什么都没测到。所以另外开了四条连接，每条都故意改坏一个字节：

| # | 改动 | 期望 | 实际 |
|:--:|:--|:--|:--|
| 1 | CONNECT 协议级别 `04 → 03` | 拒绝 | `CONNACK ret=0x01` |
| 2 | CONNECT 的 RL 改小 1（`0x10 → 0x0F`） | 不当成功 | `connection closed` |
| 3 | CONNECT 标志 reserved 位置 1（`0x02 → 0x03`） | 拒绝 | `connection closed` |
| 4 | SUBSCRIBE 首字节 `0x82 → 0x80` | 不能回 SUBACK | `connection closed` |

四条全部被拒，但拒绝方式不一样：第一条收到了非 0 的 CONNACK 返回码，其余三条
是直接断 TCP。这个差异由 broker 的具体实现决定，不同 broker 处理方式不同，
所以验收只要求「没有成功证据」，不强求某种特定的失败形式。

### 3.4 固件编译

```bat
cd firmware\EdgeGateway
cmake --preset Debug
cmake --build build\Debug
```

```text
[48/48] Linking C executable EdgeGateway.elf
Memory region         Used Size  Region Size  %age Used
             RAM:       21584 B       128 KB     16.47%
           FLASH:       41412 B       512 KB      7.90%
```

0 error 0 warning。W3 对固件的增量只有 `mqtt_codec.c` 和 `mqtt_codec.h` 两个
新文件，外加 `CMakeLists.txt` 里加了一行。

---

## 4. 实现边界：哪些是自己推的，哪些只是参考

这一节是想把话说清楚。参考项目 `zgd-dev/-V2.0` 的坑 4 就是「宣称手写 MQTT，
实际是移植 libemqtt」——我不想重复那个错误。

| 模块 | 来源 | 说明 |
|:--|:--|:--|
| Remaining Length 编解码 | 自写 | 按 OASIS §2.2.3 的「每组 7 位、最高位续、低位组在前」自己推，没搬任何库 |
| CONNECT 组装 | 自写 | 固定头 / 可变头 / 载荷三段字段顺序按 §3.1 排，连接标志位按位定义 `0x80` 到 `0x02` |
| SUBSCRIBE / PUBLISH / PUBACK | 自写 | 首字节 `0x82` / `0x30` / `0x32`，报文标识符和 QoS 规则按规范章节实现 |
| 收包分类 `mqtt_classify` | 自写 | 只判断「够不够一帧」，不一次解析全部字段 |
| 心跳到期 `mqtt_keepalive_due` | 自写 | 用无符号减法处理 tick 回绕，取 KeepAlive 的一半作发送周期 |
| 结构体字段命名 / 参数风格 | 参考 libemqtt 和 Mosquitto 的公开示例 | 只借鉴命名和调用形式，没有复制实现代码 |

面试里可以直接这么说：

> MQTT 的编解码是照着 OASIS 3.1.1 规范自己写的，`mqtt_codec.c` 是纯 C，
> 不依赖 HAL 和 FreeRTOS，PC 和 STM32 共用同一份源码。参考过 libemqtt
> 的字段命名和调用习惯，但没有移植它的实现，每一段字节都能对着规范讲出出处。

---

## 5. 几个关键设计决定

**Remaining Length 是低位组在前，最多 4 字节。**
这里很容易想当然：不是高位在前，而是每组取 7 位、最低的 7 位先发。`128` 编码
出来是 `80 01`，不是 `01 80`。我第一版写反了，靠向量测试当场抓到。上限
`0x0FFFFFFF` 用 `MQTT_RL_MAX_VALUE` 显式定义，避免有人传超范围的值进来。

**RL 只算可变头加载荷，不含固定头。**
这是我最早的误解——把 RL 当成整帧长度，结果发出去之后 broker 一直等剩下的字节，
现象就是「连上去了但毫无反应」，很容易怀疑到别的地方去。

**`mqtt_classify` 只判断边界，不做完整解析。**
串口和 TCP 都是流，一次读到的可能是半帧，也可能是两帧。所以收包分两步走：
先按固定头加 RL 判断够不够一帧，够了再交给上层解析。这样这个函数的逻辑简单到
可以穷举测，11 条向量就覆盖完了。

**心跳用无符号减法处理 tick 回绕。**
`now_ms - last_tx_ms >= period` 在 `uint32_t` 下天然正确，不需要额外写回绕分支。
发送周期取 KeepAlive 的一半，留一格余量，免得两边时钟有抖动时被 broker 先判超时。

**缓冲区一律带 `cap`，返回值区分成功、空间不足、参数非法。**
不返回裸长度，也不指望调用方自己算够不够。所有 `mqtt_build_*` 都是「先算需要
多少字节，再和 `cap` 比，最后才写」，写不下就不写，不会在缓冲区里留下半帧。

---

## 6. 踩过的坑

| # | 坑 | 现象 | 根因 | 修法 |
|:--:|:--|:--|:--|:--|
| 1 | RL 的语义搞错 | broker 连上后无响应 | 以为 RL 等于整帧长度 | RL 只算可变头加载荷 |
| 2 | RL 的 7 位组顺序写反 | `128` 解出来不是 `80 01` | 先发了高的 7 位 | 改成低位组在前，靠向量测试锁死 |
| 3 | 公网 broker 偶发 PINGREQ 失败 | `[FAIL] PINGREQ -> recv=-2` | 不是报文错。测试工具没清掉 PUBLISH 回显留下的迟到帧，读到空队列后撞上 3 秒 `SO_RCVTIMEO` | 发完 PINGREQ 先非阻塞地把旧帧读掉，再等新应答 |
| 4 | 一次 `recv` 不等于一帧 | 解析偶发错位 | 把 TCP 当成有消息边界的 | `mqtt_classify` 按 RL 分帧 |

第 3 条值得单独说。遇到「偶发失败」时，正确的做法是先分层定位，别一上来就改
协议代码。我当时先用一段独立的 Python socket 只发 `C0 00`，收到 `D0 00`，
证明了字节本身是对的；然后再去查测试工具的接收逻辑，才发现是残留的迟到帧。
另外那段时间 Python 直连 `broker.emqx.io` 也经常 8 秒超时，说明是外部环境不稳定，
不是本项目代码的问题。这两件事分开确认之后，才敢下结论。

---

## 7. 验收清单

- [x] RL 编解码 7 个向量全过，编码和解码双向断言
- [x] CONNECT 18 字节逐字节一致，Mosquitto 回 `20 02 00 00`
- [x] SUBSCRIBE / SUBACK 组对正确，能解析 `90 03 00 01 00`
- [x] PUBLISH 的 QoS0 和 QoS1 两版都对，broker 回 PUBACK 并原样回显正文
- [x] 心跳 `C0 00` 换回 `D0 00`
- [x] 4 条反例被 broker 拒绝，且能解释每条为什么该被拒
- [x] 移植边界在 `docs/mqtt.md` 里写清
- [x] 固件编译 0 error 0 warning，RAM 16.47%、FLASH 7.90%
- [x] 三家独立 broker 交叉验证结论一致
- [x] 阶段复盘写完

---

## 8. 验证边界

已经证明的：

- `mqtt_codec` 生成的字节符合 MQTT 3.1.1 规范。依据是三家 broker 的判定、
  30 条向量和 4 条反例。
- `mqtt_codec` 是纯 C、可移植的，在 Windows x86 和 STM32 上都能编译。

还没证明的：

| 项 | 状态 | 什么时候补 |
|:--|:--|:--|
| 在 STM32 上真实收发 | 未验证 | 硬件到货后，W7 |
| 经 ESP8266 AT 透传连接云端 | 未验证 | W7 |
| 巴法云 `bemfa.com:9501` 实连 | 未验证 | W7 |
| RS485 链路和传感器真实读数 | 未验证 | 硬件到货后 |

这一阶段所有 broker 验证都在 PC 上完成：Windows gcc 编译 `mqtt_codec.c`，
通过 Winsock 连 broker。这能证明协议实现正确，不能证明 STM32 侧的串口、
ESP8266、网络链路正确。这是两件事，文档里也不打算混在一起写。

编译通过不等于跑通。

---

## 9. 和后几周的关系

```
W3                 W4                W5              W7
MQTT 报文层  →  RTOS 任务架构  →  Flash 分区  →  ESP8266 + 真云端
                                                    ↑
                                          复用 W4 队列、W5 断网缓存
```

W4 把这套 codec 接进任务体系，由 `net_task` 持有 MQTT 客户端状态机。W5 做断网
缓存，数据先落 Flash，重连之后补传。W7 经 ESP8266 透传真正上云，那时才能补上
「云端每 10 秒收到一条数据」这条阶段门禁。

---

## 10. 变更清单（`3a08f08`）

| 文件 | 变化 |
|:--|--:|
| `firmware/EdgeGateway/Core/Src/mqtt_codec.c` | +692 |
| `firmware/EdgeGateway/Core/Inc/mqtt_codec.h` | +296 |
| `tools/mqtt_broker_check/broker_check.c` | +624 |
| `tools/mqtt_test/test_mqtt_codec.c` | +571 |
| `docs/mqtt.md` | +128 |
| `tools/mqtt_broker_check/README.md` | +71 |
| `.clang-format` | +63 |
| `tools/mqtt_broker_check/run.bat` | +17 |
| `tools/mqtt_test/build.bat` | +16 |
| `tools/mqtt_broker_check/build.bat` | +16 |
| `tools/mqtt_broker_check/mosquitto.conf` | +13 |
| `tools/mqtt_test/run.bat` | +5 |
| `firmware/EdgeGateway/CMakeLists.txt` | +1 |
| `firmware/EdgeGateway/Core/Src/modbus_host.c` | +1 |
| `firmware/EdgeGateway/Core/Src/modbus_port_stm32.c` | +1 / −1 |

15 个文件，+2515 / −1。