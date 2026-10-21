#include "win_serial_port.h"

#include <stdio.h>
#include <string.h>

/* ---------------- mb_port_t 回调 ---------------- */

static int win_write(const uint8_t *buf, uint16_t len, void *ctx)
{
    win_serial_t *s = (win_serial_t *)ctx;
    DWORD written = 0;

    if (s == NULL || s->h == INVALID_HANDLE_VALUE)
        return -1;

    if (!WriteFile(s->h, buf, (DWORD)len, &written, NULL))
        return -1;

    return (written == (DWORD)len) ? 0 : -1;
}

static int win_read_byte(uint8_t *b, uint32_t timeout_ms, void *ctx)
{
    win_serial_t *s = (win_serial_t *)ctx;
    COMMTIMEOUTS to;
    DWORD got = 0;

    if (s == NULL || s->h == INVALID_HANDLE_VALUE || b == NULL)
        return -1;

    /* 同步读 1 字节：
     * ReadIntervalTimeout = MAXWORD/MAXDWORD 让 ReadFile 在有字节时立即返回；
     * ReadTotalTimeoutConstant 给出本次调用的总等待上限。
     * 超时时 ReadFile 返回 TRUE 且 got == 0，所以用 got == 1 判断成功。 */
    memset(&to, 0, sizeof(to));
    to.ReadIntervalTimeout         = MAXDWORD;
    to.ReadTotalTimeoutMultiplier  = 0;
    to.ReadTotalTimeoutConstant    = timeout_ms;
    to.WriteTotalTimeoutMultiplier = 0;
    to.WriteTotalTimeoutConstant   = 1000;

    if (!SetCommTimeouts(s->h, &to))
        return -1;

    if (!ReadFile(s->h, b, 1, &got, NULL))
        return -1;

    return (got == 1) ? 0 : -1;
}

static uint32_t win_tick_ms(void)
{
    return (uint32_t)GetTickCount();
}

/* ---------------- 打开 / 关闭 ---------------- */

int win_serial_open(win_serial_t *s, const char *port_name)
{
    char path[64];
    DCB dcb;

    if (s == NULL || port_name == NULL)
        return -1;

    memset(s, 0, sizeof(*s));
    s->h = INVALID_HANDLE_VALUE;

    /* COM10 及以上必须用 \\.\COMx 形式，COM3/COM4 也兼容 */
    _snprintf(path, sizeof(path), "\\\\.\\%s", port_name);
    path[sizeof(path) - 1] = '\0';

    s->h = CreateFileA(path,
                       GENERIC_READ | GENERIC_WRITE,
                       0,                    /* 独占打开：一个口只能一个程序用 */
                       NULL,
                       OPEN_EXISTING,
                       0,                    /* 同步模式 */
                       NULL);
    if (s->h == INVALID_HANDLE_VALUE)
    {
        fprintf(stderr, "打开 %s 失败，GetLastError=%lu\n", port_name, GetLastError());
        fprintf(stderr, "  检查：端口是否存在、是否被其他程序占用（Modbus Slave 应占 COM3）\n");
        return -1;
    }

    memset(&dcb, 0, sizeof(dcb));
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(s->h, &dcb))
    {
        fprintf(stderr, "GetCommState 失败，GetLastError=%lu\n", GetLastError());
        CloseHandle(s->h);
        s->h = INVALID_HANDLE_VALUE;
        return -1;
    }

    dcb.BaudRate          = CBR_9600;
    dcb.ByteSize          = 8;
    dcb.Parity            = NOPARITY;
    dcb.StopBits          = ONESTOPBIT;
    dcb.fBinary           = TRUE;
    dcb.fParity           = FALSE;
    dcb.fOutxCtsFlow      = FALSE;
    dcb.fOutxDsrFlow      = FALSE;
    dcb.fDtrControl       = DTR_CONTROL_DISABLE;
    dcb.fDsrSensitivity   = FALSE;
    dcb.fTXContinueOnXoff = TRUE;
    dcb.fOutX             = FALSE;
    dcb.fInX              = FALSE;
    dcb.fErrorChar        = FALSE;
    dcb.fNull             = FALSE;
    dcb.fRtsControl       = RTS_CONTROL_DISABLE;
    dcb.fAbortOnError     = FALSE;

    if (!SetCommState(s->h, &dcb))
    {
        fprintf(stderr, "SetCommState 失败，GetLastError=%lu\n", GetLastError());
        CloseHandle(s->h);
        s->h = INVALID_HANDLE_VALUE;
        return -1;
    }

    SetupComm(s->h, 1024, 1024);
    PurgeComm(s->h, PURGE_RXCLEAR | PURGE_TXCLEAR);

    s->port.write     = win_write;
    s->port.read_byte = win_read_byte;
    s->port.tick_ms   = win_tick_ms;
    s->port.ctx       = s;

    return 0;
}

void win_serial_close(win_serial_t *s)
{
    if (s != NULL && s->h != INVALID_HANDLE_VALUE)
    {
        CloseHandle(s->h);
        s->h = INVALID_HANDLE_VALUE;
    }
}

const mb_port_t *win_serial_port(win_serial_t *s)
{
    return (s != NULL) ? &s->port : NULL;
}
