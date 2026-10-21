#ifndef WIN_SERIAL_PORT_H
#define WIN_SERIAL_PORT_H

#include <windows.h>

/* windows.h 里也定义了 MB_OK（MessageBox 返回值），会与 modbus_host.h
 * 的枚举 MB_OK 冲突。PC 仿真工具不用 MessageBox，这里先取消该宏。 */
#ifdef MB_OK
#undef MB_OK
#endif

#include "modbus_host.h"

/* PC 侧的 mb_port_t 实现：用 Win32 串口 API 打开 COMx。
 * 只用于 PC 仿真，不参与 STM32 固件编译。 */
typedef struct {
    HANDLE    h;      /* CreateFile 返回的串口句柄 */
    mb_port_t port;   /* 供 mb_transact 使用的函数表 */
} win_serial_t;

/* 打开并配置串口（9600 8N1）。成功返回 0，失败返回 -1。 */
int win_serial_open(win_serial_t *s, const char *port_name);

/* 关闭串口。 */
void win_serial_close(win_serial_t *s);

/* 取得 mb_port_t 指针，可直接传给 mb_transact。 */
const mb_port_t *win_serial_port(win_serial_t *s);

#endif /* WIN_SERIAL_PORT_H */
