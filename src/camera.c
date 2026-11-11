#include "camera.h"
#include "config.h"
#include "logger.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/time.h>
#include <linux/videodev2.h>

/*------------------------------------------------------------------
 * V4L2 抓图
 *
 * 流程是 V4L2 的标准套路，一步一步来：
 *   open -> VIDIOC_QUERYCAP      问驱动支不支持 capture
 *        -> VIDIOC_S_FMT         定分辨率/像素格式
 *        -> VIDIOC_REQBUFS       申请 N 个缓冲
 *        -> mmap + VIDIOC_QBUF   把缓冲交给驱动
 *        -> VIDIOC_STREAMON      开流
 *        -> select + VIDIOC_DQBUF 取一帧
 *        -> 写文件 + VIDIOC_QBUF  还回去
 *
 * 为什么一次抓图要 open/close：告警是低频事件（几分钟一次），
 * 常开摄像头会一直占着 CSI 带宽、多耗 100mA 左右的电，
 * 对靠电池撑着的基站不值得。要用的时候开、用完就关。
 *----------------------------------------------------------------*/

#define CAM_BUF_CNT     4
#define CAM_TIMEOUT_MS  3000

struct cam_buf
{
    void *start;
    size_t length;
};

static int           g_ready   = 0;
static unsigned long g_count   = 0;
static char          g_last_path[160];

int camera_is_ready(void)
{
    return g_ready;
}

unsigned long camera_count(void)
{
    return g_count;
}

const char *camera_last_path(void)
{
    return g_last_path;
}

void camera_close(void)
{
    g_ready = 0;
    LOG_INFO("[camera] 已关闭，累计抓拍 %lu 张", g_count);
}

int camera_init(void)
{
    int fd;
    struct v4l2_capability cap;

    if (0 == g_cfg.camera_enable)
    {
        LOG_INFO("[camera] 未开启（camera_enable = 0）");
        return -1;
    }

    if ('\0' == g_cfg.camera_dev[0])
    {
        LOG_WARN("[camera] 没配设备节点");
        return -1;
    }

    fd = open(g_cfg.camera_dev, O_RDWR | O_NONBLOCK);
    if (fd < 0)
    {
        LOG_WARN("[camera] 打不开 %s: %s（没有摄像头也能跑，只是不抓图）",
                 g_cfg.camera_dev, strerror(errno));
        return -1;
    }

    if (0 != ioctl(fd, VIDIOC_QUERYCAP, &cap))
    {
        LOG_WARN("[camera] %s 不是 V4L2 设备", g_cfg.camera_dev);
        close(fd);
        return -1;
    }

    if (0 == (cap.capabilities & V4L2_CAP_VIDEO_CAPTURE))
    {
        LOG_WARN("[camera] %s 不支持视频采集（driver=%s card=%s）",
                 g_cfg.camera_dev, cap.driver, cap.card);
        close(fd);
        return -1;
    }

    close(fd);

    g_ready = 1;
    LOG_INFO("[camera] %s 就绪：driver=%s card=%s，%dx%d 抓拍",
             g_cfg.camera_dev, cap.driver, cap.card,
             g_cfg.camera_width, g_cfg.camera_height);

    return 0;
}

/****************************************************************
 * 按目录路径逐级建目录
 ****************************************************************/
static void cam_mkdir_for(const char *file_path)
{
    char  dir[192];
    char *p = NULL;

    snprintf(dir, sizeof(dir), "%s", file_path);

    p = strrchr(dir, '/');
    if (NULL == p)
    {
        return;
    }
    *p = '\0';

    for (p = dir + 1; '\0' != *p; p++)
    {
        if ('/' == *p)
        {
            *p = '\0';
            (void)mkdir(dir, 0755);
            *p = '/';
        }
    }
    (void)mkdir(dir, 0755);
}

/****************************************************************
 * 定格式：优先 MJPEG（直接就是 JPEG，省得做编码），
 * 不支持就退 YUYV，存成 .yuv 裸数据，PC 上也能看
 ****************************************************************/
static int cam_set_fmt(int fd, unsigned int *out_fmt, int *out_size)
{
    struct v4l2_format fmt;
    unsigned int      prefer[2];
    int               i;

    prefer[0] = V4L2_PIX_FMT_MJPEG;
    prefer[1] = V4L2_PIX_FMT_YUYV;

    for (i = 0; i < 2; i++)
    {
        memset(&fmt, 0, sizeof(fmt));
        fmt.type                = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        fmt.fmt.pix.width       = (unsigned int)g_cfg.camera_width;
        fmt.fmt.pix.height      = (unsigned int)g_cfg.camera_height;
        fmt.fmt.pix.pixelformat = prefer[i];
        fmt.fmt.pix.field       = V4L2_FIELD_ANY;

        if (0 != ioctl(fd, VIDIOC_S_FMT, &fmt))
        {
            continue;
        }

        /* 驱动可能把格式/尺寸改成最接近的，以返回值为准 */
        if (fmt.fmt.pix.pixelformat != prefer[i])
        {
            continue;
        }

        *out_fmt  = fmt.fmt.pix.pixelformat;
        *out_size = (int)fmt.fmt.pix.sizeimage;

        LOG_DEBUG("[camera] 采用格式 %c%c%c%c %ux%u sizeimage=%u",
                  (prefer[i] >> 0) & 0xFF, (prefer[i] >> 8) & 0xFF,
                  (prefer[i] >> 16) & 0xFF, (prefer[i] >> 24) & 0xFF,
                  fmt.fmt.pix.width, fmt.fmt.pix.height, fmt.fmt.pix.sizeimage);

        return 0;
    }

    return -1;
}

int camera_snapshot(const char *reason, char *out_path, int cap)
{
    struct cam_buf    bufs[CAM_BUF_CNT];
    struct v4l2_requestbuffers req;
    struct v4l2_buffer b;
    unsigned int      pixfmt = 0;
    int               sizeimage = 0;
    int               fd;
    int               i;
    int               ret = -1;
    int               type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    FILE             *fp = NULL;
    time_t            now;
    struct tm         tmv;
    char              path[192];
    const char       *ext = "jpg";

    memset(bufs, 0, sizeof(bufs));
    memset(&req, 0, sizeof(req));
    memset(&b, 0, sizeof(b));

    if (0 == g_cfg.camera_enable)
    {
        return -1;
    }

    fd = open(g_cfg.camera_dev, O_RDWR | O_NONBLOCK);
    if (fd < 0)
    {
        LOG_WARN("[camera] 抓拍失败，打不开 %s: %s",
                 g_cfg.camera_dev, strerror(errno));
        return -1;
    }

    if (0 != cam_set_fmt(fd, &pixfmt, &sizeimage))
    {
        LOG_WARN("[camera] 没有可用的 MJPEG/YUYV 格式");
        close(fd);
        return -1;
    }

    /* 申请缓冲区 */
    req.count  = CAM_BUF_CNT;
    req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;

    if (0 != ioctl(fd, VIDIOC_REQBUFS, &req))
    {
        LOG_WARN("[camera] REQBUFS 失败: %s", strerror(errno));
        close(fd);
        return -1;
    }

    if (req.count < 1)
    {
        LOG_WARN("[camera] 驱动没给缓冲区");
        close(fd);
        return -1;
    }

    /* mmap 并全部入队 */
    for (i = 0; i < (int)req.count; i++)
    {
        memset(&b, 0, sizeof(b));
        b.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        b.index  = (unsigned int)i;

        if (0 != ioctl(fd, VIDIOC_QUERYBUF, &b))
        {
            LOG_WARN("[camera] QUERYBUF 失败: %s", strerror(errno));
            goto out;
        }

        bufs[i].length = b.length;
        bufs[i].start  = mmap(NULL, b.length, PROT_READ | PROT_WRITE,
                              MAP_SHARED, fd, (off_t)b.m.offset);
        if (MAP_FAILED == bufs[i].start)
        {
            bufs[i].start = NULL;
            LOG_WARN("[camera] mmap 失败: %s", strerror(errno));
            goto out;
        }

        if (0 != ioctl(fd, VIDIOC_QBUF, &b))
        {
            LOG_WARN("[camera] QBUF 失败: %s", strerror(errno));
            goto out;
        }
    }

    type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (0 != ioctl(fd, VIDIOC_STREAMON, &type))
    {
        LOG_WARN("[camera] STREAMON 失败: %s", strerror(errno));
        goto out;
    }

    /* 等一帧 */
    {
        fd_set         rfds;
        struct timeval tv;
        int            r;
        int            waited = 0;

        for (;;)
        {
            FD_ZERO(&rfds);
            FD_SET(fd, &rfds);
            tv.tv_sec  = 0;
            tv.tv_usec = 200000;

            r = select(fd + 1, &rfds, NULL, NULL, &tv);
            if (r < 0)
            {
                if (EINTR == errno)
                {
                    continue;
                }
                break;
            }

            waited += 200;
            if (r > 0)
            {
                memset(&b, 0, sizeof(b));
                b.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                b.memory = V4L2_MEMORY_MMAP;

                if (0 == ioctl(fd, VIDIOC_DQBUF, &b))
                {
                    break;              /* 拿到一帧 */
                }
            }

            if (waited > CAM_TIMEOUT_MS)
            {
                LOG_WARN("[camera] 等帧超时 %d ms", CAM_TIMEOUT_MS);
                break;
            }
        }

        if (0 == b.bytesused)
        {
            goto stream_off;
        }
    }

    /* 写文件 */
    now = time(NULL);
    {
        struct tm *ptm = localtime(&now);

        if (NULL != ptm)
        {
            tmv = *ptm;
        }
        else
        {
            memset(&tmv, 0, sizeof(tmv));
        }
    }

    if (V4L2_PIX_FMT_YUYV == pixfmt)
    {
        ext = "yuv";
    }

    snprintf(path, sizeof(path), "%s/snap_%04d%02d%02d_%02d%02d%02d_%s.%s",
             g_cfg.snapshot_dir,
             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec,
             (NULL == reason || '\0' == reason[0]) ? "event" : reason,
             ext);

    cam_mkdir_for(path);

    fp = fopen(path, "wb");
    if (NULL == fp)
    {
        LOG_WARN("[camera] 打不开 %s: %s", path, strerror(errno));
        goto stream_off;
    }

    if (b.bytesused != fwrite(bufs[b.index].start, 1, b.bytesused, fp))
    {
        LOG_WARN("[camera] 写文件失败");
        goto stream_off;
    }

    fclose(fp);
    fp = NULL;

    snprintf(g_last_path, sizeof(g_last_path), "%s", path);
    if (NULL != out_path && cap > 0)
    {
        snprintf(out_path, (size_t)cap, "%s", path);
    }

    g_count++;
    ret = 0;

    LOG_INFO("[camera] 已抓拍 %s（%u 字节，%s）", path, b.bytesused,
             (V4L2_PIX_FMT_MJPEG == pixfmt) ? "MJPEG" : "YUYV");

stream_off:
    (void)ioctl(fd, VIDIOC_STREAMOFF, &type);

out:
    if (NULL != fp)
    {
        fclose(fp);
    }

    for (i = 0; i < CAM_BUF_CNT; i++)
    {
        if (NULL != bufs[i].start)
        {
            (void)munmap(bufs[i].start, bufs[i].length);
            bufs[i].start = NULL;
        }
    }

    close(fd);

    return ret;
}
