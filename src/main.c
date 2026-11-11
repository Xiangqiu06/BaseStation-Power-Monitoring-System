#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>

#include "linklist.h"
#include "mailbox.h"
#include "data_source.h"
#include "storage.h"
#include "alarm.h"
#include "uploader.h"
#include "mqtt.h"
#include "camera.h"
#include "ui_app.h"
#include "config.h"
#include "logger.h"

#define APP_VERSION     "1.2"
#define STATUS_EVERY    60          /* 每采集多少条打一次状态 */
#define CLEANUP_PERIOD  3600        /* 清理历史数据的间隔（秒） */
#define CAM_COOLDOWN_S  30          /* 两次抓拍的最小间隔，防止告警抖动写爆 SD 卡 */

static const char *g_cfg_path = CONFIG_PATH_DEFAULT;
static int         g_run_sec  = 0;  /* >0 表示跑够这么多秒自动退出，方便测试 */

/* 收到 SIGNAL 就会进这里 */
void sig_handler(int sig)
{
    (void)sig;
    g_running = 0;        /* 只做一件事：告诉所有线程该收工了 */
}

/****************************************************************
 * 采集线程
 *
 * 数据源初始化放在线程里，是因为串口打不开属于"运行期故障"，
 * 让线程自己退出并把 g_running 清掉，比在 main 里失败更好收尾
 ****************************************************************/
void *get_data_th(void *arg)
{
    struct msgdata mydata;
    unsigned long  cnt = 0;

    (void)arg;

    if (0 != data_source_init(g_cfg.serial_dev, g_cfg.serial_baud))
    {
        LOG_ERROR("数据源初始化失败，采集线程退出");
        g_running = 0;
        return NULL;
    }

    while (g_running)
    {
        memset(&mydata, 0, sizeof(mydata));

        if (0 != data_source_read(&mydata))
        {
            if (g_running)
            {
                /* 单次读失败（比如 Modbus 某个节点没应答）不退出，
                 * 只计数，下一个节点继续问 */
                continue;
            }
            break;
        }

        /* 采到的原始数据先交给告警线程，
         * 由它打上告警标志后再分发，避免几个消费者各判一次 */
        if (0 != send_mail_msg("get_data", mydata, "alarm_data"))
        {
            LOG_WARN("分发失败，alarm_data 不在邮箱系统里");
        }

        cnt++;

        if (0 == (cnt % STATUS_EVERY))
        {
            LOG_INFO("[status] 已采集 %lu 条，累计丢弃 %d 条，累计收帧错误 %lu 次",
                     cnt, GetDropCount(), data_source_err_count());
        }
    }

    data_source_close();
    LOG_INFO("[get_data] 采集线程退出，共采集 %lu 条", cnt);

    return NULL;
}

/****************************************************************
 * 告警判定线程
 *
 * 位置在采集和消费者之间：进来的是原始数据，出去的是打过告警标志的数据。
 * 这一步很轻（6 次比较），但把"判定逻辑"从采集里拆出去了，
 * 以后要加迟滞、加多级阈值，只动这个文件
 ****************************************************************/
void *alarm_data_th(void *arg)
{
    struct msgdata mydata;
    char           sendname[32] = {0};

    (void)arg;

    while (g_running)
    {
        memset(&mydata, 0, sizeof(mydata));

        if (0 != recv_mail_msg(&mydata, sendname))
        {
            continue;
        }

        (void)alarm_check(&mydata);

        send_mail_msg("alarm_data", mydata, "show_data");
        send_mail_msg("alarm_data", mydata, "save_data");
        send_mail_msg("alarm_data", mydata, "send_data");
        send_mail_msg("alarm_data", mydata, "ui_data");

        /* 只有"严重"级才叫摄像头，警告级抓图没意义还费 SD 卡 */
        if (ALARM_LEVEL_CRIT == mydata.level)
        {
            send_mail_msg("alarm_data", mydata, "camera_data");
        }
    }

    LOG_INFO("[alarm_data] 告警线程退出");

    return NULL;
}

/****************************************************************
 * 打印线程：调试用，正式部署可以把 log_level 调成 WARN 让它安静点
 ****************************************************************/
void *show_data_th(void *arg)
{
    struct msgdata mydata;
    char           sendname[32] = {0};
    char           text[128]    = {0};

    (void)arg;

    while (g_running)
    {
        memset(&mydata, 0, sizeof(mydata));

        if (0 != recv_mail_msg(&mydata, sendname))
        {
            continue;
        }

        (void)alarm_text(mydata.alarm, text, (int)sizeof(text));

        LOG_INFO("%s->show : 组%d T=%.2f H=%.2f V=%.2f A=%.2f S=%.0f SOC=%.1f [%s]",
                 sendname, mydata.bat_id, mydata.temp, mydata.hum,
                 mydata.v, mydata.a, mydata.smoke, mydata.soc, text);
    }

    LOG_INFO("[show_data] 打印线程退出");

    return NULL;
}

/****************************************************************
 * 存储线程
 *
 * 单独一个线程管数据库，主要是把 SD 卡的慢 IO 从采集路径上摘出去：
 * 卡写一次要几十毫秒，放在采集线程里会直接拖垮 1Hz 的采集节奏
 ****************************************************************/
void *save_data_th(void *arg)
{
    struct msgdata mydata;
    char           sendname[32] = {0};
    char           text[128]    = {0};
    unsigned long  saved        = 0;
    unsigned long  alarms       = 0;
    time_t         last_cleanup = 0;

    (void)arg;

    if (0 != storage_init(g_cfg.db_path))
    {
        LOG_ERROR("[save_data] 数据库初始化失败，存储线程退出");
        g_running = 0;
        return NULL;
    }

    last_cleanup = time(NULL);

    while (g_running)
    {
        memset(&mydata, 0, sizeof(mydata));

        if (0 != recv_mail_msg(&mydata, sendname))
        {
            continue;
        }

        if (0 != storage_insert(&mydata))
        {
            continue;      /* 单条写失败不终止线程，继续存后面的 */
        }
        saved++;

        /* 有告警才写告警表，正常数据不占这份空间 */
        if (0 != mydata.alarm)
        {
            (void)alarm_text(mydata.alarm, text, (int)sizeof(text));

            if (0 == storage_insert_alarm(&mydata, text))
            {
                alarms++;
            }
        }

        /* 顺手做数据保留：一小时看一次时间，比每条都判断时间省 CPU */
        if (time(NULL) - last_cleanup >= CLEANUP_PERIOD)
        {
            last_cleanup = time(NULL);

            if (g_cfg.retain_days > 0)
            {
                (void)storage_cleanup(g_cfg.retain_days);
            }

            LOG_INFO("[save_data] 已入库 %lu 条，其中告警 %lu 条，库内共 %d 条",
                     saved, alarms, storage_count());
        }
    }

    storage_close();
    LOG_INFO("[save_data] 存储线程退出，共入库 %lu 条，告警 %lu 条", saved, alarms);

    return NULL;
}

/****************************************************************
 * 上报线程：裸 TCP 行协议 + MQTT 云平台，两条通道可以同时开
 ****************************************************************/
void *send_data_th(void *arg)
{
    struct msgdata mydata;
    char           sendname[32] = {0};
    unsigned long  dropped = 0;

    (void)arg;

    /* 返回 -1 只是"配置有问题、别发"，不影响线程继续把队列排空，
     * 否则生产者发一条就阻塞在队列满上，整个流水线会堵死 */
    (void)uploader_init();
    (void)mqtt_init();

    if (0 == g_cfg.upload_enable && 0 == g_cfg.mqtt_enable)
    {
        while (g_running)
        {
            memset(&mydata, 0, sizeof(mydata));

            if (0 == recv_mail_msg(&mydata, sendname))
            {
                dropped++;
            }
        }

        LOG_INFO("[send_data] 上报功能全部关闭，线程退出（未上报 %lu 条）", dropped);
        return NULL;
    }

    while (g_running)
    {
        memset(&mydata, 0, sizeof(mydata));

        if (0 != recv_mail_msg(&mydata, sendname))
        {
            continue;
        }

        /* MQTT：标准物联网通道，接云端平台用这个 */
        if (0 != g_cfg.mqtt_enable)
        {
            (void)mqtt_publish(&mydata);
            mqtt_poll();                /* 收下行配置 + 发心跳 */
        }

        /* 裸 TCP：现场自建上位机用这个，报文是人能读的行文本 */
        if (0 != g_cfg.upload_enable)
        {
            (void)uploader_send(&mydata);
        }
    }

    /* 退出前最后再试一次补发，尽量少丢点 */
    (void)uploader_flush_offline();
    uploader_close();
    mqtt_close();

    return NULL;
}

/****************************************************************
 * 摄像头线程：只有严重告警才被投递，收到就抓一张
 *
 * 单独开线程是因为抓一帧要 100~500ms，放在告警线程里会把
 * 整条流水线卡住一次；而抓图本身失败了也不该影响主流程
 ****************************************************************/
void *camera_th(void *arg)
{
    struct msgdata mydata;
    char           sendname[32] = {0};
    time_t         last_snap    = 0;
    unsigned long  shots        = 0;

    (void)arg;

    if (0 != camera_init())
    {
        /* 没摄像头也一样能跑，只是不抓图 */
        while (g_running)
        {
            (void)recv_mail_msg(&mydata, sendname);
        }
        return NULL;
    }

    while (g_running)
    {
        char reason[48];
        char path[192];

        memset(&mydata, 0, sizeof(mydata));

        if (0 != recv_mail_msg(&mydata, sendname))
        {
            continue;
        }

        if (time(NULL) - last_snap < CAM_COOLDOWN_S)
        {
            LOG_DEBUG("[camera] 距上次抓拍不足 %d 秒，跳过", CAM_COOLDOWN_S);
            continue;
        }

        snprintf(reason, sizeof(reason), "L%d_%03X", mydata.level, mydata.alarm);

        if (0 == camera_snapshot(reason, path, (int)sizeof(path)))
        {
            last_snap = time(NULL);
            shots++;
        }
    }

    camera_close();
    LOG_INFO("[camera] 摄像头线程退出，共抓拍 %lu 张", shots);

    return NULL;
}

static void usage(const char *prog)
{
    printf("后备电源检测系统 %s\n", APP_VERSION);
    printf("用法: %s [-c 配置文件] [-t 运行秒数] [-h]\n", prog);
    printf("  -c  指定配置文件，默认 %s\n", CONFIG_PATH_DEFAULT);
    printf("  -t  跑够 N 秒自动退出（不用按 Ctrl+C 也能测），默认一直跑\n");
    printf("  -h  显示这段帮助\n");
}

/****************************************************************
 * 解析命令行。参数不多，手写比引 getopt 干净
 ****************************************************************/
static int parse_args(int argc, char **argv)
{
    int i;

    for (i = 1; i < argc; i++)
    {
        if (0 == strcmp(argv[i], "-h") || 0 == strcmp(argv[i], "--help"))
        {
            usage(argv[0]);
            return 1;
        }
        else if (0 == strcmp(argv[i], "-c") && i + 1 < argc)
        {
            g_cfg_path = argv[++i];
        }
        else if (0 == strcmp(argv[i], "-t") && i + 1 < argc)
        {
            g_run_sec = atoi(argv[++i]);
        }
        else
        {
            printf("未知参数: %s\n\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    return 0;
}

int main(int argc, char **argv)
{
    Node_t *pmail = NULL;
    time_t  t0    = 0;

    if (0 != parse_args(argc, argv))
    {
        return 0;
    }

    /* 配置要在 logger 之前读，因为日志路径本身也是配置项 */
    (void)config_load(g_cfg_path);

    /* 日志文件打不开不致命，logger 内部会退化成只打屏幕 */
    (void)logger_init(g_cfg.log_path, g_cfg.log_level);

    LOG_INFO("==================================================");
    LOG_INFO(" 后备电源检测系统 v%s  启动", APP_VERSION);
    LOG_INFO(" 构建于 %s %s", __DATE__, __TIME__);
    LOG_INFO("==================================================");

    config_dump();

    signal(SIGINT,  sig_handler);     /* Ctrl+C */
    signal(SIGTERM, sig_handler);     /* kill 默认信号 */
    signal(SIGPIPE, SIG_IGN);         /* 网络断了别把整个进程打死 */

    pmail = create_mailbox_system();
    if (NULL == pmail)
    {
        LOG_ERROR("邮箱系统创建失败");
        logger_close();
        return -1;
    }

    /* 先起消费者，最后起生产者：
     * 反过来的话第一条数据进来时消费者还没注册，会被丢掉 */
    if (0 != register_thread_mail_task(pmail, "show_data",   show_data_th)   ||
        0 != register_thread_mail_task(pmail, "save_data",   save_data_th)   ||
        0 != register_thread_mail_task(pmail, "send_data",   send_data_th)   ||
        0 != register_thread_mail_task(pmail, "ui_data",     ui_app_thread)  ||
        0 != register_thread_mail_task(pmail, "camera_data", camera_th)      ||
        0 != register_thread_mail_task(pmail, "alarm_data",  alarm_data_th)  ||
        0 != register_thread_mail_task(pmail, "get_data",    get_data_th))
    {
        LOG_ERROR("线程注册失败，进程退出");
        g_running = 0;
        destroy_mailbox(pmail);
        logger_close();
        return -1;
    }

    LOG_INFO("7 个线程已启动：get_data -> alarm_data -> show / save / send / ui / camera");

    t0 = time(NULL);

    /* 主线程在这里等着，直到收到退出信号或者跑够 -t 指定的秒数 */
    while (g_running)
    {
        sleep(1);

        if (g_run_sec > 0 && (time(NULL) - t0) >= g_run_sec)
        {
            LOG_INFO("已运行 %d 秒，按 -t 参数主动退出", g_run_sec);
            g_running = 0;
        }
    }

    LOG_INFO("收到退出标志，开始回收资源...");

    destroy_mailbox(pmail);

    LOG_INFO("运行时长 %ld 秒，全部线程已安全退出", (long)(time(NULL) - t0));
    LOG_INFO("==================================================");

    logger_close();

    return 0;
}
