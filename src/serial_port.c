#include "serial_port.h"
#include "logger.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <sys/select.h>

/****************************************************************
 * 波特率 -> termios 常量
 ****************************************************************/
static speed_t baud_to_speed(int baud)
{
    switch (baud)
    {
        case 9600:   return B9600;
        case 19200:  return B19200;
        case 38400:  return B38400;
        case 57600:  return B57600;
        case 115200: return B115200;
        case 230400: return B230400;
        default:     return B115200;
    }
}

/****************************************************************
 * 打开并配置串口：8 位数据、无校验、1 位停止、无流控、原始模式
 *
 * 原始模式很关键，不然换行符会被 tty 层吃掉/改写，
 * 二进制的 Modbus 帧和 AA55 帧都会被改坏
 ****************************************************************/
int serial_port_open(const char *dev, int baud)
{
    struct termios tio;
    speed_t        spd;
    int            fd;

    if (NULL == dev || '\0' == dev[0])
    {
        return -1;
    }

    fd = open(dev, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0)
    {
        LOG_ERROR("打开串口 %s 失败: %s", dev, strerror(errno));
        return -1;
    }

    if (0 != tcgetattr(fd, &tio))
    {
        LOG_ERROR("tcgetattr(%s) 失败: %s", dev, strerror(errno));
        close(fd);
        return -1;
    }

    cfmakeraw(&tio);

    tio.c_cflag |= (CLOCAL | CREAD);    /* 忽略调制解调器线，允许接收 */
    tio.c_cflag &= ~CRTSCTS;            /* 不要硬件流控 */
    tio.c_cflag &= ~CSTOPB;             /* 1 位停止位 */
    tio.c_cflag &= ~PARENB;             /* 无校验 */
    tio.c_cflag &= ~CSIZE;
    tio.c_cflag |= CS8;                 /* 8 位数据 */

    tio.c_cflag &= ~(tcflag_t)CRTSCTS;  /* 再清一次，兼容老内核 */
    tio.c_iflag &= ~(tcflag_t)(IXON | IXOFF | IXANY);

    tio.c_cc[VMIN]  = 0;                /* 由 select 控制超时，不让 read 死等 */
    tio.c_cc[VTIME] = 0;

    spd = baud_to_speed(baud);
    if (0 != cfsetispeed(&tio, spd) || 0 != cfsetospeed(&tio, spd))
    {
        LOG_ERROR("设置波特率 %d 失败", baud);
        close(fd);
        return -1;
    }

    if (0 != tcsetattr(fd, TCSANOW, &tio))
    {
        LOG_ERROR("tcsetattr(%s) 失败: %s", dev, strerror(errno));
        close(fd);
        return -1;
    }

    (void)tcflush(fd, TCIFLUSH);        /* 丢掉上电前积压的脏数据 */

    return fd;
}

void serial_port_close(int fd)
{
    if (fd >= 0)
    {
        close(fd);
    }
}

void serial_port_flush(int fd)
{
    if (fd >= 0)
    {
        (void)tcflush(fd, TCIOFLUSH);
    }
}

int serial_port_read(int fd, unsigned char *buf, int cap, int timeout_ms)
{
    fd_set         rfds;
    struct timeval tv;
    int            r;
    ssize_t        n;

    if (fd < 0 || NULL == buf || cap <= 0)
    {
        return -1;
    }

    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);

    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    r = select(fd + 1, &rfds, NULL, NULL, &tv);
    if (r < 0)
    {
        if (EINTR == errno)
        {
            return 0;
        }
        return -1;
    }
    if (0 == r)
    {
        return 0;                       /* 超时 */
    }

    n = read(fd, buf, (size_t)cap);
    if (n < 0)
    {
        if (EINTR == errno || EAGAIN == errno)
        {
            return 0;
        }
        return -1;
    }

    return (int)n;
}

int serial_port_write(int fd, const unsigned char *buf, int len)
{
    int     sent = 0;
    ssize_t n;

    if (fd < 0 || NULL == buf || len <= 0)
    {
        return -1;
    }

    while (sent < len)
    {
        n = write(fd, buf + sent, (size_t)(len - sent));

        if (n < 0)
        {
            if (EINTR == errno)
            {
                continue;
            }
            if (EAGAIN == errno)
            {
                /* 发送缓冲满了，稍等再写，Modbus 帧都很短，一般不会走到这 */
                usleep(1000);
                continue;
            }
            return -1;
        }

        sent += (int)n;
    }

    (void)tcdrain(fd);                  /* 等真正发完，Modbus 有 3.5 字符间隔要求 */

    return 0;
}
