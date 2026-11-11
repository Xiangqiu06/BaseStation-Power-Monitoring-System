#ifndef __CONFIG_H__
#define __CONFIG_H__

/*------------------------------------------------------------------
 * 运行配置
 *
 * 配置文件是一行一个 "键 = 值" 的纯文本，'#' 开头是注释。
 * 文件可以不存在，不存在就整套用默认值，方便在板子上先跑起来。
 *----------------------------------------------------------------*/

#define CONFIG_PATH_DEFAULT  "./config/sensor.conf"

/* 串口名留空 或者 填 "sim" 都表示使用内置模拟数据源 */
#define CONFIG_SIM_TAG       "sim"

struct app_config
{
    /* ---------- 串口 ---------- */
    char   serial_dev[64];      /* 例: /dev/ttymxc2，空串走模拟源 */
    int    serial_baud;         /* 115200 */

    /* ---------- 采集 ---------- */
    int    sample_period_ms;    /* 采样周期，没接真机时用它控制节奏 */
    int    dev_id;              /* 本机设备号，一起写进库里 */

    /* ---------- 告警阈值 ---------- */
    double temp_max;            /* 温度上限 ℃ */
    double hum_max;             /* 湿度上限 % */
    double volt_min;            /* 电压下限 V  —— 后备电源最关心这个 */
    double volt_max;            /* 电压上限 V */
    double curr_max;            /* 电流上限 A */
    double smoke_max;           /* 烟雾上限 ppm */

    /* ---------- 存储 ---------- */
    char   db_path[128];
    int    retain_days;         /* 数据保留天数，0 表示不清理 */

    /* ---------- 上报 ---------- */
    int    upload_enable;       /* 0 关 / 1 开（裸 TCP 行协议） */
    char   upload_host[64];
    int    upload_port;

    /* ---------- MQTT 云平台 ---------- */
    int    mqtt_enable;
    char   mqtt_host[64];
    int    mqtt_port;

    /* ---------- 摄像头（OV5647/OV5640） ---------- */
    int    camera_enable;
    char   camera_dev[64];      /* /dev/video1 之类 */
    int    camera_width;
    int    camera_height;
    char   snapshot_dir[128];   /* 抓拍文件存哪儿 */

    /* ---------- 显示（LVGL） ---------- */
    int    ui_enable;

    /* ---------- Modbus 从机组网 ---------- */
    int    slave_count;         /* 1~247，iMX6ULL 轮询几个电池组节点 */

    /* ---------- 日志 ---------- */
    char   log_path[128];       /* 空串表示只打屏幕 */
    int    log_level;           /* 0=ERROR 1=WARN 2=INFO 3=DEBUG */

    /* ---------- 内部使用，不从文件读 ---------- */
    char   cfg_path[128];       /* 上次 config_load 的路径，UI 上"保存"写回这里 */
};

extern struct app_config g_cfg;

/* 全部填默认值 */
void config_set_default(void);

/* 读配置文件。返回 0 表示读成功，-1 表示文件打不开（此时已经是默认值） */
int  config_load(const char *path);

/* 单独改一项（MQTT 云端下发命令走这里）。
 * 返回 0 成功、-1 未知配置项、-2 值不合法 */
int  config_set(const char *key, const char *val);

/* 把当前生效的配置整体写回配置文件（UI 上"保存"按钮用）。
 * 返回 0 成功 */
int  config_save(void);

/* 把最终生效的配置打到日志里，出问题的时候方便对数 */
void config_dump(void);

#endif
