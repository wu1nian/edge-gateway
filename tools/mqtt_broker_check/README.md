# S20 · 真 broker 验证

把 `mqtt_codec` 手拼出来的报文直接喂给真实 broker。它不经过 HAL、不经过
FreeRTOS，也不使用任何 MQTT 客户端库——如果 broker 认这些字节，就说明
S15–S19 写出来的帧是对的。

## 验证内容

正向（一条 TCP 连接走完）：

| 步骤 | 发出 | 期望 |
|---|---|---|
| CONNECT | `10 .. 00 04 MQTT 04 02 00 3C ...` | `20 02 00 00` |
| SUBSCRIBE | `82 .. <id> gw01/data 00` | `90 03 <id> 00` |
| PUBLISH QoS1 | `32 .. gw01/data <id> {"t":25.3}` | `40 02 <id>` + broker 回显 PUBLISH |
| PINGREQ | `C0 00` | `D0 00` |
| DISCONNECT | `E0 00` | 无回包，正常关闭 |

反例（每条都开新连接）：

1. 协议级别 `0x04 → 0x03`：应收到非 0 CONNACK 返回码或断开；
2. CONNECT 的 RL 改小 1（`0x10 → 0x0F`）：broker 应等待剩余字节，不能当成功；
3. CONNECT 标志 reserved 位置 1（`0x02 → 0x03`）：应拒绝；
4. SUBSCRIBE 首字节 `0x82 → 0x80`：应断开或拒绝，不能回 SUBACK。

## 本地 Mosquitto

```bat
mosquitto.exe -c mosquitto.conf -v
run.bat
```

`mosquitto.conf` 只做两件事：监听 1883、允许匿名。监听地址收紧为
`127.0.0.1`（笔记写的是 `0.0.0.0`，本地验证用回环即可，避免匿名 broker
暴露到局域网）。`-v` 会把每条报文/协议错误打到控制台，反例时很有用。

### 本机环境（2026-10-31 实测）

| 组件 | 版本 / 路径 | 说明 |
|---|---|---|
| Mosquitto | 2.1.2 · `C:\Program Files\mosquitto\` | 已装成 Windows 服务，默认配置只听 `127.0.0.1` / `::1`，且允许匿名 |
| MQTTX | 1.13.2 便携版 · `%LOCALAPPDATA%\Programs\MQTTX\MQTTX.exe` | 桌面有快捷方式，用来当旁观者 |
| 网络调试助手 | NetAssistant 1.4.1 · `%LOCALAPPDATA%\Programs\NetAssistant\netassistant.exe` | TCP Client + HEX 模式，替代 SSCOM / NetAssist |

Mosquitto 服务在跑时 1883 已被占用，**不要再手动起第二个**。想看 broker
日志时，用管理员 cmd 先 `net stop mosquitto`，再执行上面的手动启动命令，
验证完 `net start mosquitto` 恢复服务。

本机全量运行结果（正向 7 项 + 4 条反例，`RESULT: ALL PASS`）已记入
`docs/mqtt.md`。

## 没有本地 broker 时

工具接受 host / port，可直接连公网测试 broker（只发无害的 `gw01/data`）：

```bat
run.bat broker.emqx.io 1883
run.bat broker.hivemq.com 1883
```

公网 broker 的拒绝方式可能和 Mosquitto 不同：有的回非 0 CONNACK，
有的直接关 TCP。反例判定只看「有没有成功证据」，两种情况都算通过。

## 独立旁观者（MQTTX）

正向用例运行期间，用 MQTTX 连同一个 broker，订阅 `gw01/data`，
应能看到载荷 `{"t":25.3}`。这一步是为了证明「不只是自己的客户端自说自话」。

## 退出码

`0` = 全部通过；`1` = 有失败；`2` = Winsock 初始化失败。