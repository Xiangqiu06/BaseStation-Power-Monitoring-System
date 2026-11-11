/**
 * @file  ui_stub.c
 * @brief 无界面版本（-DUI_DISABLED）用的空实现
 *
 * 主程序统一在 main.c 里注册 7 个线程，其中一个是 ui_data。
 * 编无界面版时把 ui_app.c 换成本文件：
 * 它不初始化 LVGL、不碰 framebuffer，只负责把队列排空，
 * 这样采集→告警→存储→上报的整条链路照常工作，
 * 而二进制体积能小掉 200KB 左右（LVGL 一整个库）。
 *
 * 正常编译时这个文件是空的，不会产生任何符号。
 */
#ifdef UI_DISABLED

#include "mailbox.h"

#include <stddef.h>

void *ui_app_thread(void *arg)
{
    struct msgdata m;
    char           sendname[32] = {0};

    (void)arg;

    while (g_running)
    {
        (void)recv_mail_msg_timeout(&m, sendname, 200);
    }

    return NULL;
}

#endif /* UI_DISABLED */
