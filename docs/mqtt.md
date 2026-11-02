# MQTT（W3）实现与验证记录

> 本文件是**过程记录**（每个单元的实际输出、工具修正）。
> 阶段性的完整总结（交付物清单、验收清单、踩坑表、验证边界、变更统计）见 [`docs/w3-summary.md`](w3-summary.md)。

## S20 · 真 broker 验证（2026-10-31）

工具：`tools/mqtt_broker_check/`

- 复用 `firmware/EdgeGateway/Core/Src/mqtt_codec.c`，不引入 MQTT 客户端库；
- Winsock TCP 直连 broker，手拼 CONNECT / SUBSCRIBE / PUBLISH / PINGREQ / DISCONNECT；
- 正向用例之外，跑 4 条故意改坏字节的反例。

首次验证时本机未安装 Mosquitto，因此用公网 EMQX 测试 broker 作为第三方裁判；
同日稍后在本机安装 Mosquitto 2.1.2 并复测，结论一致（见「本机 Mosquitto 复测」）。
本地 Mosquitto 的启动方式见 `tools/mqtt_broker_check/README.md` 与 `mosquitto.conf`。

### 命令

```bat
cd tools\mqtt_broker_check
build.bat
broker_check.exe broker.emqx.io 1883
```

### 实际输出

```text
== MQTT real-broker check: broker.emqx.io:1883 ==
[PASS] TCP connect -> broker.emqx.io:1883
[PASS] CONNECT -> 20 02 00 00  (ret=0x00, client_id=gw01chk-71908)
[PASS] SUBSCRIBE -> 90 03 00 01 00  (5 B)
[PASS] PUBLISH qos1 -> PUBACK 40 02 00 01
[PASS] broker echo -> PUBLISH topic=gw01/data payload={"t":25.3}
[PASS] PINGREQ -> D0 00  (2 B)
[PASS] DISCONNECT sent E0 00

-- negative cases --
[PASS] protocol level 0x03 -> CONNACK ret=0x01 (rejected)
[PASS] RL short by 1 -> connection closed
[PASS] reserved bit set -> connection closed
[PASS] SUBSCRIBE 0x80 -> connection closed

RESULT: ALL PASS
```

### 结论

- `mqtt_codec` 生成的报文被真实 broker 接受，CONNACK / SUBACK / PUBACK / PINGRESP 均为规范字节；
- QoS1 PUBLISH 的载荷经 broker 原样回显，说明主题、报文标识符、Remaining Length 全部正确；
- 4 条反例都被拒绝，说明之前「帧非法时 broker 不会假装成功」的假设成立；
- 失败路径的差异（回非 0 CONNACK vs 直接断 TCP）由 broker 实现决定，验收只要求「没有成功证据」。

### 本机 Mosquitto 复测（2026-10-31）

Mosquitto 2.1.2 装在本机并作为 Windows 服务运行，默认配置只监听
`127.0.0.1:1883` / `[::1]:1883` 且允许匿名。命令：

```bat
cd tools\mqtt_broker_check
broker_check.exe 127.0.0.1 1883
```

实际输出：

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

与 EMQX 的结论一致：协议级别错误时 Mosquitto 也回 `CONNACK ret=0x01`，
其余三条反例直接断开 TCP。说明 `mqtt_codec` 的字节在两类独立实现上
判定相同，不是「碰巧被某一家接受」。

## 实现边界：自写 vs 参考（W3 验收补记）

> 这一节是为了把话说明白：**哪些字节是我自己推出来的，哪些照着参考实现写的。**
> 原项目的坑 4 就是「说自己手写 MQTT，实际是移植 libemqtt」——这里不重复那个错误。

| 模块 | 来源 | 说明 |
|---|---|---|
| Remaining Length 编解码 | **自写** | 按 OASIS 规范 §2.2.3 的「每组 7 位 + 最高位续」规则自己推，未搬任何库 |
| CONNECT 组装 | **自写** | 固定头 / 可变头 / 载荷三段的字段顺序按规范 §3.1 排；连接标志位按位定义 `0x80`~`0x02` |
| SUBSCRIBE / PUBLISH / PUBACK | **自写** | 首字节 `0x82` / `0x30` / `0x32`，报文标识符与 QoS 规则按规范章节实现 |
| 收包分类 `mqtt_classify` | **自写** | 只做「够不够一帧」的边界判断，不一次解析全部字段 |
| 心跳到期 `mqtt_keepalive_due` | **自写** | 用无符号减法处理 tick 回绕，取 KeepAlive 的一半作发送周期 |
| 结构体字段命名 / 参数风格 | **参考** libemqtt 与 Mosquitto 的公开示例 | 只借鉴命名与调用形式，未复制实现代码 |

一句话版（面试可直接说）：

> 「MQTT 的编解码是我按 OASIS 3.1.1 规范自己写的，`mqtt_codec.c` 是纯 C，
> 不依赖 HAL/FreeRTOS，PC 和 STM32 共用同一份源码。参考过 libemqtt 的
> 字段命名和调用习惯，但没有移植它的实现——每一段字节都能对着规范讲出出处。」

## 2026-11-02 复验与工具修正

重新构建并跑了一遍（`gcc -Wall -Wextra -Werror` 零警告）：

- `tools/mqtt_test/test_mqtt_codec.exe`：29 条向量全部 PASS（RL 7 条、组帧 8 条、收包分类 8 条、返回码映射 7 项、心跳判断 6 项）；
- `tools/mqtt_broker_check` 打本机 Mosquitto：正向 6 项 + 反例 4 条，`RESULT: ALL PASS`；
- 打公网 HiveMQ：连续 5 次 `RESULT: ALL PASS`。

**修正一个测试工具的时序缺陷。** 复验时 `broker.emqx.io` 偶发
`[FAIL] PINGREQ -> recv=-2`。排查结论：

1. 先怀疑报文错——用一段独立的 Python socket 只发 `C0 00`，收到 `D0 00`，
   证明 **PINGREQ 的字节是对的**；
2. 再怀疑测试逻辑——PUBLISH 阶段订阅了同一主题并收到回显，这些帧可能迟到，
   排在接收队列里被 PINGREQ 阶段先读走，等队列读空后应答还没到，就撞上
   3 秒的 `SO_RCVTIMEO`；
3. 修法：发完 PINGREQ 后先非阻塞地把「已在队列里排队」的旧帧读掉，
   再等新应答。

修完后本地 Mosquitto 与公网 HiveMQ 均稳定全通过。`broker.emqx.io`
在那段时间本身不稳定（同一时段 Python 直连也 8 秒超时），属于外部环境，
不是本项目代码问题——**这条也记下来：遇到"偶发失败"要先分层定位，
别急着改协议代码**。
