#ifndef __CAMERA_H__
#define __CAMERA_H__

/*------------------------------------------------------------------
 * OV5647 / OV5640 摄像头（V4L2）
 *
 * 用途：告警发生时抓一张现场照片存本地，方便事后确认是"真冒烟"
 * 还是"有人开柜门扬了灰"。
 *
 * 走标准 V4L2，所以不绑定具体 sensor：
 *   OV5647（树莓派那棵，i.MX6ULL 上接 CSI 也能用）
 *   OV5640（i.MX6ULL 官方适配得多，参数更全）
 * 只要驱动注册出 /dev/videoN 就能用，优先选 MJPEG，
 * 拿不到就退化成 YUYV。
 *----------------------------------------------------------------*/

int  camera_init(void);                     /* 0 成功；-1 打不开设备 */

/* 抓一帧存文件。
 *   reason   写进文件名，比如 "smoke" "undervolt"
 *   out_path 传出完整路径，可为 NULL
 * 返回 0 成功 */
int  camera_snapshot(const char *reason, char *out_path, int cap);

int  camera_is_ready(void);
unsigned long camera_count(void);
void camera_close(void);

/* 最近的抓拍路径，UI 上可以显示 */
const char *camera_last_path(void);

#endif
