# edge-gateway

一个跑在 STM32F411 上的工业物联网边缘网关。做的事情不复杂：从 RS485 总线上
的传感器读温湿度，存到本地 Flash，同时通过网络发到云端，中间有个触摸屏能看。

协议部分是我自己写的——Modbus-RTU 和 MQTT 3.1.1 都是从零手写，没有引入任何
现成的协议栈或客户端库。这是这个项目的主要目的：把「能调库跑通」变成「能自己
把字节拼对，并且说得清每个字节为什么是这个值」。

参考项目是 [`zgd-dev/-V2.0`](https://github.com/zgd-dev/-V2.0)（commit `40b4460`）。
那个仓库里有一个能跑的完整版本，我把它克隆下来当对照，但**不直接改它**——
从 CubeMX 新建工程重写，写完一个模块再回去比差距，看自己漏了什么。

## 硬件

STM32F411CEU6（WeAct 黑药丸）· ESP8266（AT 固件）· W25Q64 Flash ·
2.8" SPI 触摸屏（ILI9341 + XPT2046）· SP3485 RS485 收发器 · ST-Link V2

整机约 ¥130。选型理由和下单清单在 [`hardware/BOM.md`](hardware/BOM.md)。

## 进度

目前做完前四周，第五周还没开始。

| 周 | 内容 | 状态 |
|:--|:--|:--|
| W1 | CubeMX 骨架、96 MHz 时钟、FreeRTOS、printf 重定向 | 完成 |
| W2 | 手写 Modbus-RTU 主站，PC 仿真先行 | 完成 |
| W3 | 手写 MQTT 3.1.1 报文层，真 broker 验证 | 完成 |
| W4 | RTOS 任务架构（任务划分、队列、锁、数据流） | 骨架完成 |
| W5 | Flash 分区与掉电保护 | 未开始 |
| W6 | 传感器采集、LVGL 界面 | 未开始 |
| W7 | ESP8266 联网、接真云端 | 未开始 |
| W8 | 优化与压测 | 未开始 |
| W9 | 收尾、文档、复盘 | 未开始 |

**W4 只做到骨架，功能还没有。** 六个任务的空壳、四个队列、一把互斥锁都跑起来了，
每个任务的业务逻辑还是 `TODO` 占位，要等 W5-W7 分别填进去。架构文档在
[`docs/task-architecture.md`](docs/task-architecture.md)。

- `app_tasks.c` 里 6 个任务全部创建成功，编译 0 error 0 warning。
- 编译期断言卡住了 `sensor_rec_t` 是 16 字节、`seq` 在偏移 12，W5 的落盘格式依赖这个。
- 优先级按截止期排：Modbus 4（200 ms 硬超时）、网络 3、传感器和存储 2、UI 1、监控 0。
- 删除 CubeMX 默认任务时，把它原来的喂狗调用接到了 `monitor_task` 里——IWDG 在调度器
  启动前就已使能，超时 20 s，不接手的话板子会每 20 s 复位一次。
- **跑起来还没有硬件验证**，调度计数和栈水位都是空的，等有板再补。

S24 又把数据流的两块核心逻辑抽成了独立模块放进固件：

- `ring_buffer.c` 管 Flash 环形缓冲的游标（`wp`/`rp`、满空判据、写满覆盖最旧）。
- `net_fsm.c` 管网络三态机（`OFFLINE`/`ONLINE`/`BACKFILL`）和指数退避重连。
- 这两个模块不碰 HAL 也不碰 FreeRTOS，所以能直接用 gcc 在 PC 上测。
  `tools/test_ring_fsm` 里 72 项断言全过，其中包含「断网 30 分钟攒 1800 条后补传」和
  「补传中途再断网」两个场景。
- 写测试的时候抓到一个真 bug：`rb_pending()` 直接算 `wp - rp`，满载覆盖的瞬间 `wp`
  绕回 0 而 `rp` 是 1，`0 - 1` 在 `uint32_t` 下变成 4294967295。改成先各自对容量取模再减。
- 环形缓冲的游标逻辑是真的，但**数据本体还没写进 W25Q64**，落盘要等 W5。
- `net_task` 里有一处占位：现在取到记录就直接 `rb_commit()`，而正确语义是收到 PUBACK
  才提交。留着是为了让状态机能跑到 `ONLINE`，**W7 接上 ESP8266 后必须删掉**。
  这件事在架构文档 §7 里单独写了，因为它是目前唯一与「不丢数据」承诺相冲突的地方。

**W3 是证据最实的一周**：

- `mqtt_codec.c` 692 行，纯 C，只 include 了 `<stddef.h>` 和 `<string.h>`，
  不碰 HAL 也不碰 FreeRTOS。所以在 PC 上用 gcc 就能直接编译测试。
- 30 条单元测试向量全部通过，`-Wall -Wextra -Werror` 零警告。
- 拿真 broker 测了两轮：本机 Mosquitto、公网 HiveMQ、公网 EMQX，三家都是
  正向 7 项通过、4 条故意改坏的反例被正确拒绝。
- 固件编译 0 error 0 warning，RAM 16.47%、FLASH 7.90%。

细节在 [`docs/w3-summary.md`](docs/w3-summary.md)。

## 文件说明

下面按目录讲每个文件是干什么的。CubeMX 自动生成的代码和手写代码会分开标。

### 根目录

| 文件 | 说明 |
|:--|:--|
| `README.md` | 就是这份文件 |
| `.gitignore` | 排除构建产物（`build/`、`*.elf`、`*.exe`）、编辑器配置、CubeMX 备份，以及整个 `reference/`（自带 `.git`，不排除会被当成子模块） |
| `.clang-format` | 统一代码风格，clangd 和 clang-format 共用。基于 LLVM 改了缩进宽度、行宽、大括号位置 |

### `firmware/EdgeGateway/` —— 固件工程

工程根目录的文件：

| 文件 | 说明 |
|:--|:--|
| `EdgeGateway.ioc` | CubeMX 工程文件。引脚分配、时钟树、外设开关都以它为准，改了要重新生成 |
| `.mxproject` | CubeMX 的生成记录，用来判断哪些文件是它生成的 |
| `CMakeLists.txt` | 构建主文件。CubeMX 生成的部分之外，我在这里手工登记了自己写的六个源文件（`modbus_host.c`、`modbus_port_stm32.c`、`mqtt_codec.c`、`app_tasks.c`、`ring_buffer.c`、`net_fsm.c`） |
| `CMakePresets.json` | 定义 `Debug` / `Release` 两个配置，都继承一个隐藏的 `default`（Ninja 生成器 + ARM 工具链）。产物分别落在 `build/Debug` 和 `build/Release` |
| `STM32F411xx_FLASH.ld` | 链接脚本，规定代码放 FLASH（512 KB）、变量放 RAM（128 KB）的地址布局 |
| `startup_stm32f411xe.s` | 启动汇编。设置初始栈指针、定义中断向量表、跳到 `main` |
| `cmake/gcc-arm-none-eabi.cmake` | 交叉编译工具链描述，告诉 CMake 用 `arm-none-eabi-gcc` |
| `cmake/stm32cubemx/CMakeLists.txt` | CubeMX 生成的构建片段，把 HAL 库、FreeRTOS、CMSIS 编译成静态库 |

`Core/Inc/` —— 头文件：

| 文件 | 说明 |
|:--|:--|
| `main.h` | 主头文件，主要是引脚宏定义。CubeMX 生成 |
| `modbus_host.h` | **手写**。Modbus 协议核心的对外接口：功能码、参数范围、SHT20 寄存器地址、帧结构体、CRC 函数、收发函数 |
| `modbus_port_stm32.h` | **手写**。移植层接口，只有一个 `mb_port_stm32()`，返回一个绑定到 USART2 的串口操作表 |
| `mqtt_codec.h` | **手写**。MQTT 报文层接口：RL 编解码、各种组帧函数、收包分类、心跳判断，以及 `MQTT_RL_MAX_BYTES` 等常量 |
| `app_config.h` | **手写**。任务架构里的所有数字：6 个优先级、6 个栈深度、4 个队列深度、3 个周期，每个都写了依据 |
| `app_tasks.h` | **手写**。任务骨架对外接口：`app_tasks_init()`、`app_tasks_init_result()`、`app_net_state()`、`app_fmt_record()`，以及 `sensor_rec_t` 结构体。网络三态枚举从 `net_fsm.h` 引入，不重复定义 |
| `ring_buffer.h` | **手写**。Flash 环形缓冲的游标接口：`rb_init`/`rb_put`/`rb_peek`/`rb_commit`/`rb_stat`，以及 `RB_CAPACITY`（4096 条）和 `RB_RECORD_BYTES`（32 字节）。注释里写清了「先写数据再推进 `wp`」「收到 PUBACK 才推进 `rp`」这两条铁律 |
| `net_fsm.h` | **手写**。网络三态机接口：三个状态枚举、`net_fsm_on_connected`/`on_disconnected`/`after_ack`、退避接口。纯逻辑，不依赖 HAL |
| `FreeRTOSConfig.h` | FreeRTOS 配置。tick 定 1000 Hz，抢占式调度，堆 15360 字节。CubeMX 生成，我改过堆大小 |
| `stm32f4xx_hal_conf.h` | HAL 模块开关，决定哪些外设驱动被编进去 |
| `stm32f4xx_it.h` | 中断服务函数声明 |
| `gpio.h` / `usart.h` / `spi.h` / `rtc.h` / `iwdg.h` | 各外设的初始化函数声明，CubeMX 生成 |

`Core/Src/` —— 源文件：

| 文件 | 说明 |
|:--|:--|
| `main.c` | 程序入口。外设初始化、启动 FreeRTOS 调度器。初始化代码是 CubeMX 生成的，`USER CODE` 区里是我写的，其中最重要的一段是 `_write` 重定向到 USART1，`printf` 才能打到串口 |
| `freertos.c` | FreeRTOS 初始化。CubeMX 原本在这里建了一个默认任务，W4 删掉了它，改成调用 `app_tasks_init()` 并把返回值存下来给 `main.c` 检查。文件末尾的栈溢出钩子 `vApplicationStackOverflowHook` 保留：任务爆栈时停在那里，方便挂调试器看是谁 |
| `modbus_host.c` | **手写，231 行**。Modbus-RTU 全部协议逻辑：CRC-16 查表、0x03/0x04/0x06 组帧、正常与异常响应解析、字节间超时和帧间超时两层超时。完全不含 HAL，靠调用方传入的读写函数指针操作串口 |
| `modbus_port_stm32.c` | **手写，77 行**。把 `modbus_host` 和 STM32 连起来：用 `HAL_UART_Transmit` / `HAL_UART_Receive` 收发，加上 RS485 方向脚（DE）的翻转时序 |
| `mqtt_codec.c` | **手写，692 行**。MQTT 3.1.1 报文层：Remaining Length 变长整数编解码、CONNECT/SUBSCRIBE/PUBLISH/PUBACK/PINGREQ/DISCONNECT 组帧、收包边界分类、心跳到期判断 |
| `app_tasks.c` | **手写**。任务骨架：创建 6 个任务、4 个队列和 1 把互斥锁，先建同步对象再建任务。日志统一走 `log_line()` 并受 `uart1_mutex` 保护，`monitor_task` 每 10 s 打一次计数、栈水位、丢帧数、环形缓冲游标和网络状态 |
| `ring_buffer.c` | **手写**。环形缓冲游标实现：满判据 `(wp+1)%N==rp` 时覆盖最旧并计溢出，`rb_pending()` 先对容量取模再相减。不管数据怎么读怎么写，那是 W5 的事 |
| `net_fsm.c` | **手写**。三态机迁移 + 指数退避（1→2→4→8→16→30 s 封顶）。断开时不动 `rp`，补完才回 `ONLINE` |
| `gpio.c` / `usart.c` / `spi.c` / `rtc.c` / `iwdg.c` | 各外设初始化，CubeMX 生成，目前没有手工改动。`usart.c` 里开了两路：USART1 给调试打印，USART2 给 RS485。ESP8266 用的串口要等 W7 再加 |
| `stm32f4xx_hal_msp.c` | HAL 的底层初始化回调（时钟使能、引脚复用、NVIC 配置），CubeMX 生成 |
| `stm32f4xx_hal_timebase_tim.c` | 用 TIM 做 HAL 的时基，这样 HAL 的 `HAL_Delay` 不占 SysTick，SysTick 留给 FreeRTOS |
| `stm32f4xx_it.c` | 中断服务函数实现，CubeMX 生成 |
| `system_stm32f4xx.c` | 系统时钟初始化，CubeMX 生成 |
| `syscalls.c` / `sysmem.c` | newlib 的系统调用桩，CubeMX 生成。里面的 `_write` 是弱符号，`main.c` 里那个强符号会覆盖它 |

`Drivers/` 和 `Middlewares/` 是 ST 官方代码和 FreeRTOS 源码，原样引入，没有改动：

- `Drivers/CMSIS/` —— ARM 内核头文件和启动支持
- `Drivers/STM32F4xx_HAL_Driver/` —— ST 的 HAL 库
- `Middlewares/Third_Party/FreeRTOS/` —— FreeRTOS 内核 + CMSIS-RTOS v2 封装层

### `tools/` —— PC 侧验证工具

这里的东西都不参与固件编译，纯粹为了在 PC 上先把协议逻辑验掉。三个工具都
复用 `firmware/` 里同一份协议源码——不存在「测试用一份、固件用另一份」。

| 文件 | 说明 |
|:--|:--|
| `pc_sim/pc_sim_main.c` | Modbus 主站 PC 仿真。用 com0com 建一对虚拟串口，程序开一头当主站，Modbus Slave 开另一头当从站，验证真串口链路 |
| `pc_sim/win_serial_port.c` / `.h` | Win32 串口 API 的封装，实现和 STM32 版一样的读写函数指针，这样 `modbus_host.c` 两边能通用 |
| `pc_sim/build.bat` / `run.bat` / `run_note14.bat` | 编译和运行脚本 |
| `pc_sim/README.md` | 怎么配 com0com、怎么配 Modbus Slave、怎么注入错误 |
| `mqtt_test/test_mqtt_codec.c` | MQTT 报文层单元测试，30 条向量。覆盖 RL 编解码边界值、各种 CONNECT 组合、SUBSCRIBE/PUBLISH 组帧、收包分类、返回码映射、心跳判断 |
| `mqtt_test/build.bat` / `run.bat` | 编译（`-std=c11 -Wall -Wextra -Werror -O2`）和运行 |
| `mqtt_broker_check/broker_check.c` | 真 broker 验证。用 Winsock 直连 broker，手拼 CONNECT/SUBSCRIBE/PUBLISH/PINGREQ/DISCONNECT 发过去，检查回包；另外开四条连接故意发坏报文，确认 broker 真的会拒绝 |
| `mqtt_broker_check/mosquitto.conf` | 本地 Mosquitto 配置，只监听 `127.0.0.1:1883`、允许匿名，避免把匿名 broker 暴露到局域网 |
| `mqtt_broker_check/build.bat` / `run.bat` | 编译（链 `ws2_32`）和运行 |
| `mqtt_broker_check/README.md` | 正向用例和反例分别怎么判定、怎么换公网 broker |
| `test_ring_fsm/test_ring_fsm.c` | 环形缓冲 + 网络三态机的单元测试，72 项断言。除了逐条验游标和退避，还有两个端到端场景：断网 30 分钟攒 1800 条后重连补传、补传中途再断网时 `rp` 停在原处 |
| `test_ring_fsm/build.bat` | 编译（`-std=c99 -Wall -Wextra -Werror -O2`），直接引用固件目录里的两个 `.c`，不复制代码 |
| `test_modbus.c` | Modbus 早期的手工测试：先验 CRC（标准向量 `123456789` 应得 `4B37`），再把三种请求帧打出来跟笔记里的期望字节逐个对。后来被 `pc_sim` 的串口链路测试取代了，留着当快速自检 |

### `docs/` —— 文档

| 文件 | 说明 |
|:--|:--|
| `modbus.md` | Modbus 协议合同。SHT20 的寄存器表、请求响应帧格式、哨兵帧、手册里 CRC 的勘误。代码以这份为准 |
| `mqtt.md` | MQTT 验证记录。每个单元跑了什么、实际输出是什么、公网 broker 偶发失败怎么定位的，还有一节专门写「哪些是自己推的、哪些只是参考了命名风格」 |
| `w3-summary.md` | W3 阶段总结。交付物、验证证据、设计决策、踩坑、验收清单、验证边界 |
| `task-architecture.md` | W4 任务架构。6 个任务的优先级与依据、共享资源用锁还是队列、数据流与网络三态机、故障隔离、内存占用、待补清单 |

### `hardware/`

| 文件 | 说明 |
|:--|:--|
| `BOM.md` | 物料清单。每个器件的型号、价格、下单时要认准什么参数 |

### `reference/`

`zgd-V2.0/` 是参考项目原样克隆，自带 `.git`，被 `.gitignore` 排除在外，
不会进版本库，也不会被当成子模块。只读，不修改。

## 关于验证

这个项目里我会尽量把「证明了什么」和「没证明什么」分开写。

比如 W3 的 broker 验证全部在 PC 上完成：Windows gcc 编译 `mqtt_codec.c`，
Winsock 直连 broker。这证明了**协议实现的字节是对的**，但**没有证明** STM32
侧真的能收发、ESP8266 真的能透传、云端真的能收到数据。

后者要等硬件到货之后才能测，在 W7。在那之前的文档里，相关内容都标记为待验证，
不会写成已完成。编译通过不等于跑通，这两个是不同的事。

## 怎么跑

固件：

```bat
cd firmware\EdgeGateway
cmake --preset Debug
cmake --build build\Debug
```

需要把 `arm-none-eabi-gcc` 放在 PATH 里。产物是 `build/Debug/EdgeGateway.elf`。

MQTT 报文层单元测试（不需要硬件）：

```bat
cd tools\mqtt_test
build.bat
run.bat
```

真 broker 验证（需要本机有 Mosquitto，或者能连公网）：

```bat
cd tools\mqtt_broker_check
build.bat
broker_check.exe 127.0.0.1 1883
```

也可以直接打公网测试 broker：`broker_check.exe broker.hivemq.com 1883`。

环形缓冲和三态机单元测试（同样不需要硬件）：

```bat
cd tools\test_ring_fsm
build.bat
test_ring_fsm.exe
```

需要 gcc 在 PATH 里，MinGW 或 WinLibs 都行。它会直接编译 `firmware/EdgeGateway/Core/Src/` 下的
`ring_buffer.c` 和 `net_fsm.c`，不复制一份代码，所以测的就是固件里真正跑的那份。