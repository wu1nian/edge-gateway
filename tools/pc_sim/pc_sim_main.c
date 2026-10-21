#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "modbus_host.h"
#include "win_serial_port.h"

static void usage(const char *exe)
{
    printf("用法: %s [选项]\n", exe);
    printf("  --com COM4        主站串口（默认 COM4）\n");
    printf("  --addr 1          从机地址 1~247（默认 1）\n");
    printf("  --func 4          功能码 3 或 4（默认 4，SHT20 输入寄存器）\n");
    printf("  --reg 1           起始寄存器地址（默认 1）\n");
    printf("  --count 2         读取寄存器数量 1~125（默认 2）\n");
    printf("  --loop            每秒读一次，Ctrl+C 退出\n");
    printf("  --help            显示本帮助\n");
    printf("\n示例:\n");
    printf("  %s --com COM4 --func 4 --addr 1 --reg 1 --count 2\n", exe);
    printf("  %s --com COM4 --func 3 --addr 1 --reg 1 --count 2 --loop\n", exe);
}

static void print_hex(const char *tag, const uint8_t *buf, uint16_t len)
{
    printf("%s (%u):", tag, (unsigned)len);
    for (uint16_t i = 0; i < len; i++)
        printf(" %02X", buf[i]);
    printf("\n");
}

static const char *status_name(mb_status_t st)
{
    switch (st)
    {
        case MB_OK:            return "MB_OK";
        case MB_ERR_PARAM:     return "MB_ERR_PARAM";
        case MB_ERR_LEN:       return "MB_ERR_LEN";
        case MB_ERR_ADDR:      return "MB_ERR_ADDR";
        case MB_ERR_FUNC:      return "MB_ERR_FUNC";
        case MB_ERR_CRC:       return "MB_ERR_CRC";
        case MB_ERR_EXCEPTION: return "MB_ERR_EXCEPTION";
        case MB_ERR_TIMEOUT:   return "MB_ERR_TIMEOUT";
        case MB_ERR_IO:        return "MB_ERR_IO";
        default:               return "UNKNOWN";
    }
}

static const char *exception_name(uint8_t exc)
{
    switch (exc)
    {
        case 0x01: return "非法功能（从站不支持该功能码）";
        case 0x02: return "非法数据地址（寄存器不存在）";
        case 0x03: return "非法数据值（写入值越界）";
        case 0x04: return "从站设备故障";
        default:   return "未知异常码";
    }
}

static void print_hint(mb_status_t st, uint8_t addr, uint8_t func)
{
    switch (st)
    {
        case MB_ERR_TIMEOUT:
            printf("  提示：Modbus Slave 是否已 Connect 到 COM3？Slave ID=%u、Function=0%u 是否一致？\n",
                   (unsigned)addr, (unsigned)func);
            printf("        两端必须是 9600 8N1，且 COM3 <-> COM4 是 com0com 的同一对端口。\n");
            break;
        case MB_ERR_ADDR:
            printf("  提示：收到的是其他从站的回应；检查 Slave ID 是否与 --addr 一致。\n");
            break;
        case MB_ERR_FUNC:
            printf("  提示：从站功能码与 --func 不一致。仿真 SHT20 时 Modbus Slave 要选 04。\n");
            break;
        case MB_ERR_CRC:
            printf("  提示：帧被干扰或从站返回了坏帧；检查线路/虚拟串口参数。\n");
            break;
        default:
            break;
    }
}

int main(int argc, char **argv)
{
    const char *com = "COM4";
    uint8_t  addr  = 0x01;
    uint8_t  func  = MB_FC_READ_INPUT;
    uint16_t reg   = SHT20_REG_TEMP;
    uint16_t count = 2;
    int      loop  = 0;

    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--help") == 0)
        {
            usage(argv[0]);
            return 0;
        }
        else if (strcmp(argv[i], "--loop") == 0)
        {
            loop = 1;
        }
        else if (strcmp(argv[i], "--com") == 0 && i + 1 < argc)
        {
            com = argv[++i];
        }
        else if (strcmp(argv[i], "--addr") == 0 && i + 1 < argc)
        {
            unsigned long v = strtoul(argv[++i], NULL, 0);
            if (v < 1 || v > 247) { fprintf(stderr, "addr 必须在 1~247\n"); return 2; }
            addr = (uint8_t)v;
        }
        else if (strcmp(argv[i], "--func") == 0 && i + 1 < argc)
        {
            unsigned long v = strtoul(argv[++i], NULL, 0);
            if (v == 3)      func = MB_FC_READ_HOLDING;
            else if (v == 4) func = MB_FC_READ_INPUT;
            else { fprintf(stderr, "func 只能是 3 或 4\n"); return 2; }
        }
        else if (strcmp(argv[i], "--reg") == 0 && i + 1 < argc)
        {
            unsigned long v = strtoul(argv[++i], NULL, 0);
            if (v > 0xFFFFul) { fprintf(stderr, "reg 必须在 0~65535\n"); return 2; }
            reg = (uint16_t)v;
        }
        else if (strcmp(argv[i], "--count") == 0 && i + 1 < argc)
        {
            unsigned long v = strtoul(argv[++i], NULL, 0);
            if (v < 1 || v > MB_READ_MAX) { fprintf(stderr, "count 必须在 1~125\n"); return 2; }
            count = (uint16_t)v;
        }
        else
        {
            fprintf(stderr, "未知参数: %s\n", argv[i]);
            usage(argv[0]);
            return 2;
        }
    }

    win_serial_t serial;
    if (win_serial_open(&serial, com) != 0)
        return 1;

    printf("已打开 %s：9600 8N1，从机地址 %u，功能码 0%u，起始寄存器 0x%04X，数量 %u\n",
           com, (unsigned)addr, (unsigned)func, (unsigned)reg, (unsigned)count);

    do
    {
        uint8_t  req[8];
        uint8_t  resp[300];       /* 125 个寄存器最长响应 255 字节 */
        uint16_t vals[MB_READ_MAX];
        uint16_t rlen = 0;
        uint8_t  exc  = 0;

        int n = mb_build_read_req(addr, func, reg, count, req);
        if (n != 8)
        {
            fprintf(stderr, "组帧失败：参数非法\n");
            break;
        }

        print_hex("TX", req, 8);

        mb_status_t st = mb_transact(win_serial_port(&serial), req, 8,
                                     resp, (uint16_t)sizeof(resp), &rlen);
        if (st != MB_OK)
        {
            printf("ERR: %s (%d)\n", status_name(st), (int)st);
            print_hint(st, addr, func);
            if (!loop)
                break;
            Sleep(1000);
            continue;
        }

        print_hex("RX", resp, rlen);

        st = mb_parse_read_resp(resp, rlen, addr, func, vals, count, &exc);
        if (st == MB_ERR_EXCEPTION)
        {
            printf("EXCEPTION: 0x%02X（%s）\n", (unsigned)exc, exception_name(exc));
        }
        else if (st != MB_OK)
        {
            printf("解析失败: %s (%d)\n", status_name(st), (int)st);
            print_hint(st, addr, func);
        }
        else
        {
            for (uint16_t i = 0; i < count; i++)
            {
                printf("  reg[0x%04X] = %u (0x%04X)\n",
                       (unsigned)(reg + i),
                       (unsigned)vals[i],
                       (unsigned)vals[i]);
            }

            /* SHT20 口径：前两个输入寄存器是温度、湿度，单位 0.1 */
            if (func == MB_FC_READ_INPUT && count >= 2)
            {
                int16_t t = (int16_t)vals[0];
                printf("SHT20: T=%.1f C  H=%.1f %%RH\n",
                       t / 10.0f, vals[1] / 10.0f);
            }
        }

        if (loop)
            Sleep(1000);

    } while (loop);

    win_serial_close(&serial);
    return 0;
}
