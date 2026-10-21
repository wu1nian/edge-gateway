# Modbus PC 仿真主站（COM4 ↔ COM3）

用 **你工程里的同一份 `modbus_host.c`** 在 PC 上跑主站：
- 主站：`pc_sim.exe`，打开 **COM4**
- 从站：**Modbus Slave**，打开 **COM3**
- COM3 ↔ COM4 是 com0com 建好的一对虚拟串口

这条链路验证的是「真串口版 `mb_port_t`」：协议核心不变，只把假串口换成了 Win32 串口 API。
它和 STM32 上跑的是**同一份 CRC / 组帧 / 解析 / 超时代码**。

> ⚠️ 本目录只用于 PC 仿真，不参与 STM32 固件编译，也不要加进 `CMakeLists.txt`。

---

## 一、准备

已确认本机具备：

- com0com 虚拟串口：`COM3` / `COM4` 已存在（设备管理器 → 端口）
- Modbus Slave：`C:\Program Files\Modbus Tools\Modbus Slave\mbslave.exe`
- MinGW gcc：`D:\MinGW\bin\gcc.exe`（在 PATH 中）

如果没有 `pc_sim.exe`，先编译：

```bat
cd /d D:\Embedded\edge-gateway\tools\pc_sim
build.bat
```

---

## 二、配置 Modbus Slave（从站）

1. 打开 **Modbus Slave**。
2. 菜单 **Connection → Connect**：
   - Port：`COM3`
   - Mode：`RTU`
   - Baud：`9600`
   - Data：`8`
   - Parity：`None`
   - Stop：`1`
   - 点 **OK**。状态栏应显示已连接。
3. 菜单 **Setup → Slave Definition**：
   - **Slave ID = 1**
   - **Function = 04**（Read Input Registers）
   - **Address = 1**
   - **Quantity = 2**
   - 点 **OK**。
4. 主界面会出现两个格子（地址 1、2）。双击填入：
   - 第 1 格（温度原始值）：`253` → 25.3 ℃
   - 第 2 格（湿度原始值）：`300` → 30.0 %RH

> 说明：仿真 SHT20 时用 **Function 04 / 输入寄存器 / 地址 1**。
> 如果照笔记 14 的从站定义做基线练习，则用 **Function 03 / Address 0**。

### 两种从站配置，别混用

| 用途 | Modbus Slave 里选 | 主站命令 |
|:---|:---|:---|
| 笔记 14 基线 | Function **03**，Address **0**，Quantity 2 | `run_note14.bat` |
| 真实 SHT20 口径 | Function **04**，Address **1**，Quantity 2 | `run.bat` |

主站的 `--func` / `--reg` 必须和 Modbus Slave 的 **Function / Address** 一一对应。
两边不一致时，从站要么回异常帧，要么干脆不应答（表现为超时）。

---

## 三、运行主站

在 `tools\pc_sim` 下，按从站配置二选一：

```bat
:: 笔记 14 基线（Function 03 / Address 0）
run_note14.bat

:: 真实 SHT20 口径（Function 04 / Address 1）
run.bat
```

`run_note14.bat` 等价于：

```bat
pc_sim.exe --com COM4 --func 3 --addr 1 --reg 0 --count 2
```

`run.bat` 等价于：

```bat
pc_sim.exe --com COM4 --func 4 --addr 1 --reg 1 --count 2
```

每秒循环读取（以 SHT20 为例）：

```bat
pc_sim.exe --com COM4 --func 4 --addr 1 --reg 1 --count 2 --loop
```

### 期望输出（SHT20：03 的报文同理，只是功能码和 CRC 不同）

```text
已打开 COM4：9600 8N1，从机地址 1，功能码 04，起始寄存器 0x0001，数量 2
TX (8): 01 04 00 01 00 02 20 0B
RX (9): 01 04 04 00 FD 01 2C 6A 39
  reg[0x0001] = 253 (0x00FD)
  reg[0x0002] = 300 (0x012C)
SHT20: T=25.3 C  H=30.0 %RH
```

笔记 14 基线（Function 03 / Address 0）的期望报文是：

```text
TX (8): 01 03 00 00 00 02 C4 0B
RX (9): 01 03 04 00 FD 01 2C 6B 8E
```

- `TX` 是主站发出的 8 字节请求；最后两字节是 CRC（低字节在前）。
- `RX` 是从站回的 9 字节响应；最后两字节是 CRC。
- 温度 253 → 25.3 ℃；湿度 300 → 30.0 %RH。

---

## 四、错误注入（验收用）

### 1. 从站离线

把 Modbus Slave 的 Connection 断开，或在它没连上 COM3 时运行：

```bat
pc_sim.exe --com COM4 --func 4 --addr 1 --reg 1 --count 2
```

期望：

```text
TX (8): 01 04 00 01 00 02 20 0B
ERR: MB_ERR_TIMEOUT (7)
```

说明整帧 200 ms 超时生效，没有死等。

### 2. 功能码不符

Modbus Slave 仍配置为 **Function 04**，但主站发 0x03：

```bat
pc_sim.exe --com COM4 --func 3 --addr 1 --reg 1 --count 2
```

期望收到异常帧（功能码 0x83、异常码 0x01）：

```text
RX (5): 01 83 01 80 F0
EXCEPTION: 0x01（非法功能（从站不支持该功能码））
```

> 若当前用的是笔记 14 的 **03** 配置，就反过来：从站配 03，主站发 `--func 4`，
> 异常帧的功能码会变成 `0x84`。具体返回取决于 Modbus Slave 版本/配置；
> 也可能是 `MB_ERR_TIMEOUT`（从站直接忽略不支持的功能码）。两种都说明「没有把错误当正常数据」。

### 3. 地址不符 / 寄存器不存在

把 Slave Definition 的 Address 改成 10，仍请求起始寄存器 1：

期望异常码 `0x02`（非法数据地址），或从站不应答导致超时。

### 4. 从站地址不符

```bat
pc_sim.exe --com COM4 --func 4 --addr 2 --reg 1 --count 2
```

从站 ID=1，不会应答地址 2 → `MB_ERR_TIMEOUT`。

> CRC 错、半帧、地址/功能码不符这些更细的用例，已经在白盒自测（假串口）里覆盖；虚拟串口主要验证「真链路 + 真串口 API」。

---

## 五、常见问题

| 现象 | 原因 | 处理 |
|:---|:---|:---|
| `打开 COM4 失败` | COM4 被串口助手/其他程序占用 | 关掉占用程序；一个口只能一个程序用 |
| 一直 `MB_ERR_TIMEOUT` | Modbus Slave 没 Connect、连错口、ID/功能码/波特率不一致 | 按第二节逐项核对；确认 Slave 状态栏已连接 |
| `EXCEPTION 0x01` | 从站不支持该功能码（如配置 04 却发 03） | 统一 `--func` 与 Modbus Slave 的 Function |
| `EXCEPTION 0x02` | 请求的寄存器地址不存在 | 对齐 `--reg` 与 Slave Definition 的 Address |
| `MB_ERR_CRC` | 线路干扰 / 参数不一致 / 收到坏帧 | 虚拟串口一般不会；检查两端 9600 8N1 |
| 读到的值一直是 0 | 从站格子没填值 | 双击格子填 253、300 |

---

## 六、它和 STM32 的关系

```text
PC:      pc_sim_main.c  →  modbus_host.c  →  win_serial_port.c  →  COM4
STM32:   freertos.c     →  modbus_host.c  →  modbus_port_stm32.c → USART2
```

- `modbus_host.c`：**同一份文件**，一行不改；
- 只替换最下面的移植层：Win32 串口 ↔ HAL 串口；
- 这就是 `mb_port_t` 依赖注入的价值，也是 W2 面试可以讲的重点。
