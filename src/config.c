#include "config.h"
#include "logger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* 全局唯一的一份配置，进程起来以后只读 */
struct app_config g_cfg;

/****************************************************************
 * 去掉首尾空白，直接在原缓冲区上截断
 ****************************************************************/
static char *cfg_trim(char *s)
{
    char *end = NULL;

    if (NULL == s)
    {
        return NULL;
    }

    while ('\0' != *s && 0 != isspace((unsigned char)*s))
    {
        s++;
    }

    end = s + strlen(s);
    while (end > s && 0 != isspace((unsigned char)end[-1]))
    {
        end--;
    }
    *end = '\0';

    return s;
}

/****************************************************************
 * 字符串型配置项的拷贝，超长的话截断并报一声，
 * 免得串口设备名被悄悄截掉一半还照样去 open
 ****************************************************************/
static void cfg_copy(char *dst, size_t cap, const char *val)
{
    size_t n = strlen(val);

    if (n >= cap)
    {
        fprintf(stderr, "[config] 警告: 值 \"%s\" 超过 %u 字节，已截断\n",
                val, (unsigned int)cap - 1);
        n = cap - 1;
    }

    memcpy(dst, val, n);
    dst[n] = '\0';
}

void config_set_default(void)
{
    memset(&g_cfg, 0, sizeof(g_cfg));

    /* 串口：默认走模拟源，板子上改配置文件即可切真机，代码不用动 */
    cfg_copy(g_cfg.serial_dev, sizeof(g_cfg.serial_dev), CONFIG_SIM_TAG);
    g_cfg.serial_baud      = 115200;

    g_cfg.sample_period_ms = 1000;
    g_cfg.dev_id           = 1;

    /* 阈值取的是基站后备电源（48V 锂电组）的常见工作区间 */
    g_cfg.temp_max         = 60.0;
    g_cfg.hum_max          = 95.0;
    g_cfg.volt_min         = 46.0;
    g_cfg.volt_max         = 50.5;
    g_cfg.curr_max         = 5.0;
    g_cfg.smoke_max        = 300.0;

    cfg_copy(g_cfg.db_path, sizeof(g_cfg.db_path), "sensor.db");
    g_cfg.retain_days      = 7;

    g_cfg.upload_enable    = 1;
    cfg_copy(g_cfg.upload_host, sizeof(g_cfg.upload_host), "127.0.0.1");
    g_cfg.upload_port      = 9527;

    /* MQTT 默认连本机 broker，方便先在开发机上跑通再换云 */
    g_cfg.mqtt_enable      = 1;
    cfg_copy(g_cfg.mqtt_host, sizeof(g_cfg.mqtt_host), "127.0.0.1");
    g_cfg.mqtt_port        = 1883;

    /* 摄像头：OV5647/OV5640 在 i.MX6ULL 上一般注册成 /dev/video1 */
    g_cfg.camera_enable    = 1;
    cfg_copy(g_cfg.camera_dev, sizeof(g_cfg.camera_dev), "/dev/video1");
    g_cfg.camera_width     = 640;
    g_cfg.camera_height    = 480;
    cfg_copy(g_cfg.snapshot_dir, sizeof(g_cfg.snapshot_dir), "snapshots");

    g_cfg.ui_enable        = 1;
    g_cfg.slave_count      = 1;

    cfg_copy(g_cfg.log_path, sizeof(g_cfg.log_path), "monitor.log");
    g_cfg.log_level        = LOG_LVL_INFO;
}

/****************************************************************
 * 真正的赋值逻辑，配置文件和 MQTT 下发共用
 * 返回 0 成功、-1 未知键
 ****************************************************************/
static int cfg_apply(const char *key, const char *val)
{
    if (0 == strcmp(key, "serial_dev"))
    {
        cfg_copy(g_cfg.serial_dev, sizeof(g_cfg.serial_dev), val);
    }
    else if (0 == strcmp(key, "serial_baud"))
    {
        g_cfg.serial_baud = atoi(val);
    }
    else if (0 == strcmp(key, "sample_period_ms"))
    {
        g_cfg.sample_period_ms = atoi(val);
    }
    else if (0 == strcmp(key, "dev_id"))
    {
        g_cfg.dev_id = atoi(val);
    }
    else if (0 == strcmp(key, "temp_max"))
    {
        g_cfg.temp_max = atof(val);
    }
    else if (0 == strcmp(key, "hum_max"))
    {
        g_cfg.hum_max = atof(val);
    }
    else if (0 == strcmp(key, "volt_min"))
    {
        g_cfg.volt_min = atof(val);
    }
    else if (0 == strcmp(key, "volt_max"))
    {
        g_cfg.volt_max = atof(val);
    }
    else if (0 == strcmp(key, "curr_max"))
    {
        g_cfg.curr_max = atof(val);
    }
    else if (0 == strcmp(key, "smoke_max"))
    {
        g_cfg.smoke_max = atof(val);
    }
    else if (0 == strcmp(key, "db_path"))
    {
        cfg_copy(g_cfg.db_path, sizeof(g_cfg.db_path), val);
    }
    else if (0 == strcmp(key, "retain_days"))
    {
        g_cfg.retain_days = atoi(val);
    }
    else if (0 == strcmp(key, "upload_enable"))
    {
        g_cfg.upload_enable = atoi(val);
    }
    else if (0 == strcmp(key, "upload_host"))
    {
        cfg_copy(g_cfg.upload_host, sizeof(g_cfg.upload_host), val);
    }
    else if (0 == strcmp(key, "upload_port"))
    {
        g_cfg.upload_port = atoi(val);
    }
    else if (0 == strcmp(key, "mqtt_enable"))
    {
        g_cfg.mqtt_enable = atoi(val);
    }
    else if (0 == strcmp(key, "mqtt_host"))
    {
        cfg_copy(g_cfg.mqtt_host, sizeof(g_cfg.mqtt_host), val);
    }
    else if (0 == strcmp(key, "mqtt_port"))
    {
        g_cfg.mqtt_port = atoi(val);
    }
    else if (0 == strcmp(key, "camera_enable"))
    {
        g_cfg.camera_enable = atoi(val);
    }
    else if (0 == strcmp(key, "camera_dev"))
    {
        cfg_copy(g_cfg.camera_dev, sizeof(g_cfg.camera_dev), val);
    }
    else if (0 == strcmp(key, "camera_width"))
    {
        g_cfg.camera_width = atoi(val);
    }
    else if (0 == strcmp(key, "camera_height"))
    {
        g_cfg.camera_height = atoi(val);
    }
    else if (0 == strcmp(key, "snapshot_dir"))
    {
        cfg_copy(g_cfg.snapshot_dir, sizeof(g_cfg.snapshot_dir), val);
    }
    else if (0 == strcmp(key, "ui_enable"))
    {
        g_cfg.ui_enable = atoi(val);
    }
    else if (0 == strcmp(key, "slave_count"))
    {
        g_cfg.slave_count = atoi(val);
    }
    else if (0 == strcmp(key, "log_path"))
    {
        cfg_copy(g_cfg.log_path, sizeof(g_cfg.log_path), val);
    }
    else if (0 == strcmp(key, "log_level"))
    {
        g_cfg.log_level = atoi(val);
    }
    else
    {
        return -1;
    }

    return 0;
}

/****************************************************************
 * 逐行解析 "key = value"
 * 认不出来的键只警告不报错，这样以后加新键老版本也能跑
 ****************************************************************/
int config_load(const char *path)
{
    FILE *fp = NULL;
    char  line[512];
    int   lineno = 0;
    int   hit    = 0;

    config_set_default();

    if (NULL == path || '\0' == path[0])
    {
        return -1;
    }

    fp = fopen(path, "r");
    if (NULL == fp)
    {
        fprintf(stderr, "[config] 打不开 %s，全部使用默认配置\n", path);
        cfg_copy(g_cfg.cfg_path, sizeof(g_cfg.cfg_path), path);
        return -1;
    }

    cfg_copy(g_cfg.cfg_path, sizeof(g_cfg.cfg_path), path);

    while (NULL != fgets(line, (int)sizeof(line), fp))
    {
        char *p   = NULL;
        char *key = NULL;
        char *val = NULL;
        char *eq  = NULL;

        lineno++;

        p = cfg_trim(line);
        if ('\0' == p[0] || '#' == p[0] || ';' == p[0])
        {
            continue;               /* 空行和注释 */
        }

        eq = strchr(p, '=');
        if (NULL == eq)
        {
            fprintf(stderr, "[config] 第 %d 行没有 '='，跳过: %s\n", lineno, p);
            continue;
        }

        *eq = '\0';
        key = cfg_trim(p);
        val = cfg_trim(eq + 1);

        /* 值里允许写行尾注释: 115200   # 波特率 */
        p = strchr(val, '#');
        if (NULL != p)
        {
            *p  = '\0';
            val = cfg_trim(val);
        }

        if ('\0' == key[0])
        {
            continue;
        }

        if (0 != cfg_apply(key, val))
        {
            fprintf(stderr, "[config] 第 %d 行未知配置项: %s（忽略）\n", lineno, key);
            continue;
        }

        hit++;
    }

    fclose(fp);

    /* 兜底校正：写错了也不至于让程序行为变得莫名其妙 */
    if (g_cfg.sample_period_ms < 100)
    {
        fprintf(stderr, "[config] sample_period_ms 太小，强制为 100\n");
        g_cfg.sample_period_ms = 100;
    }
    if (g_cfg.log_level < LOG_LVL_ERROR || g_cfg.log_level > LOG_LVL_DEBUG)
    {
        g_cfg.log_level = LOG_LVL_INFO;
    }

    fprintf(stderr, "[config] 已读取 %s，生效 %d 项\n", path, hit);
    return 0;
}

int config_set(const char *key, const char *val)
{
    if (NULL == key || NULL == val)
    {
        return -2;
    }

    return cfg_apply(key, val);
}

/****************************************************************
 * 把当前配置整体写回文件
 *
 * 界面上改了阈值点"保存"走这里；MQTT 下发的阈值只改内存，
 * 掉电重启会回到文件里的值 —— 这是故意的，防止云端误操作把
 * 现场配置改乱还留不下来。
 ****************************************************************/
int config_save(void)
{
    FILE *fp = NULL;
    const char *path = g_cfg.cfg_path;

    if ('\0' == path[0])
    {
        LOG_WARN("[config] 不知道配置文件路径，保存失败（启动时没带 -c）");
        return -1;
    }

    fp = fopen(path, "w");
    if (NULL == fp)
    {
        LOG_ERROR("[config] 写 %s 失败", path);
        return -1;
    }

    fprintf(fp, "# 由后备电源检测系统自动生成 %s\n", "（界面保存）");
    fprintf(fp, "# 手工改也可以，重启后生效\n\n");

    fprintf(fp, "#---------------- 串口 ----------------\n");
    fprintf(fp, "serial_dev  = %s\n", g_cfg.serial_dev);
    fprintf(fp, "serial_baud = %d\n\n", g_cfg.serial_baud);

    fprintf(fp, "#---------------- 采集 ----------------\n");
    fprintf(fp, "sample_period_ms = %d\n", g_cfg.sample_period_ms);
    fprintf(fp, "dev_id           = %d\n", g_cfg.dev_id);
    fprintf(fp, "slave_count      = %d\n\n", g_cfg.slave_count);

    fprintf(fp, "#---------------- 告警阈值 ----------------\n");
    fprintf(fp, "temp_max   = %.1f\n", g_cfg.temp_max);
    fprintf(fp, "hum_max    = %.1f\n", g_cfg.hum_max);
    fprintf(fp, "volt_min   = %.2f\n", g_cfg.volt_min);
    fprintf(fp, "volt_max   = %.2f\n", g_cfg.volt_max);
    fprintf(fp, "curr_max   = %.2f\n", g_cfg.curr_max);
    fprintf(fp, "smoke_max  = %.1f\n\n", g_cfg.smoke_max);

    fprintf(fp, "#---------------- 存储 ----------------\n");
    fprintf(fp, "db_path     = %s\n", g_cfg.db_path);
    fprintf(fp, "retain_days = %d\n\n", g_cfg.retain_days);

    fprintf(fp, "#---------------- 上报(裸TCP) ----------------\n");
    fprintf(fp, "upload_enable = %d\n", g_cfg.upload_enable);
    fprintf(fp, "upload_host   = %s\n", g_cfg.upload_host);
    fprintf(fp, "upload_port   = %d\n\n", g_cfg.upload_port);

    fprintf(fp, "#---------------- MQTT ----------------\n");
    fprintf(fp, "mqtt_enable = %d\n", g_cfg.mqtt_enable);
    fprintf(fp, "mqtt_host   = %s\n", g_cfg.mqtt_host);
    fprintf(fp, "mqtt_port   = %d\n\n", g_cfg.mqtt_port);

    fprintf(fp, "#---------------- 摄像头 ----------------\n");
    fprintf(fp, "camera_enable = %d\n", g_cfg.camera_enable);
    fprintf(fp, "camera_dev    = %s\n", g_cfg.camera_dev);
    fprintf(fp, "camera_width  = %d\n", g_cfg.camera_width);
    fprintf(fp, "camera_height = %d\n", g_cfg.camera_height);
    fprintf(fp, "snapshot_dir  = %s\n\n", g_cfg.snapshot_dir);

    fprintf(fp, "#---------------- 显示 / 日志 ----------------\n");
    fprintf(fp, "ui_enable = %d\n", g_cfg.ui_enable);
    fprintf(fp, "log_path  = %s\n", g_cfg.log_path);
    fprintf(fp, "log_level = %d\n", g_cfg.log_level);

    fclose(fp);

    LOG_INFO("[config] 配置已写回 %s", path);
    return 0;
}

void config_dump(void)
{
    LOG_INFO("================ 运行配置 ================");
    LOG_INFO("采集: dev=%s baud=%d period=%dms dev_id=%d slaves=%d",
             g_cfg.serial_dev, g_cfg.serial_baud,
             g_cfg.sample_period_ms, g_cfg.dev_id, g_cfg.slave_count);
    LOG_INFO("阈值: T<=%.1f H<=%.1f V=[%.2f,%.2f] I<=%.2f S<=%.1f",
             g_cfg.temp_max, g_cfg.hum_max,
             g_cfg.volt_min, g_cfg.volt_max,
             g_cfg.curr_max, g_cfg.smoke_max);
    LOG_INFO("存储: %s  保留 %d 天", g_cfg.db_path, g_cfg.retain_days);
    LOG_INFO("上报: %s -> %s:%d",
             g_cfg.upload_enable ? "开" : "关",
             g_cfg.upload_host, g_cfg.upload_port);
    LOG_INFO("MQTT: %s -> %s:%d",
             g_cfg.mqtt_enable ? "开" : "关",
             g_cfg.mqtt_host, g_cfg.mqtt_port);
    LOG_INFO("摄像: %s %s %dx%d -> %s/",
             g_cfg.camera_enable ? "开" : "关", g_cfg.camera_dev,
             g_cfg.camera_width, g_cfg.camera_height, g_cfg.snapshot_dir);
    LOG_INFO("显示: %s   日志: %s level=%d",
             g_cfg.ui_enable ? "开" : "关", g_cfg.log_path, g_cfg.log_level);
    LOG_INFO("==========================================");
}
