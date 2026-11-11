#include "logger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>

#define LOG_LINE_MAX     1024
#define LOG_PATH_MAX     256

static FILE          *g_fp       = NULL;
static int            g_level    = LOG_LVL_INFO;
static long           g_max_size = 4L * 1024 * 1024;   /* 默认单文件 4MB */
static char           g_path[LOG_PATH_MAX];

/* 多线程往里写，必须串起来 */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static const char *log_level_name(int level)
{
    switch (level)
    {
        case LOG_LVL_ERROR: return "ERROR";
        case LOG_LVL_WARN:  return "WARN ";
        case LOG_LVL_INFO:  return "INFO ";
        case LOG_LVL_DEBUG: return "DEBUG";
        default:            return "?????";
    }
}

/* 只取文件名，日志里不想要一长串路径 */
static const char *log_basename(const char *path)
{
    const char *p = NULL;

    if (NULL == path)
    {
        return "?";
    }

    p = strrchr(path, '/');
    if (NULL != p)
    {
        return p + 1;
    }

    p = strrchr(path, '\\');
    if (NULL != p)
    {
        return p + 1;
    }

    return path;
}

/****************************************************************
 * 目录不存在就一层层建出来，/var/log/basestation 这种不会预先存在
 ****************************************************************/
static void log_mkdir_for(const char *file_path)
{
    char  dir[LOG_PATH_MAX];
    char *p = NULL;

    snprintf(dir, sizeof(dir), "%s", file_path);

    p = strrchr(dir, '/');
    if (NULL == p)
    {
        return;                        /* 就当前目录，不用建 */
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
 * 文件太大就滚动一次：monitor.log -> monitor.log.1
 * 只留一份历史，板子上的 SD 卡经不起无限写
 ****************************************************************/
static void log_rotate_locked(void)
{
    char old_name[LOG_PATH_MAX + 8];
    long size = 0;

    if (NULL == g_fp || g_max_size <= 0)
    {
        return;
    }

    if (0 != fflush(g_fp))
    {
        return;
    }

    size = ftell(g_fp);
    if (size < g_max_size)
    {
        return;
    }

    fclose(g_fp);
    g_fp = NULL;

    snprintf(old_name, sizeof(old_name), "%s.1", g_path);
    (void)remove(old_name);
    (void)rename(g_path, old_name);

    g_fp = fopen(g_path, "a");
    if (NULL == g_fp)
    {
        fprintf(stderr, "[logger] 滚动后重开 %s 失败，日志只打屏幕\n", g_path);
    }
}

int logger_init(const char *path, int level)
{
    pthread_mutex_lock(&g_lock);

    g_level = level;
    if (g_level < LOG_LVL_ERROR)
    {
        g_level = LOG_LVL_ERROR;
    }
    if (g_level > LOG_LVL_DEBUG)
    {
        g_level = LOG_LVL_DEBUG;
    }

    if (NULL != g_fp)
    {
        fclose(g_fp);
        g_fp = NULL;
    }
    g_path[0] = '\0';

    if (NULL != path && '\0' != path[0])
    {
        log_mkdir_for(path);

        g_fp = fopen(path, "a");
        if (NULL == g_fp)
        {
            pthread_mutex_unlock(&g_lock);
            fprintf(stderr, "[logger] 打不开 %s，日志只打屏幕\n", path);
            return -1;
        }

        snprintf(g_path, sizeof(g_path), "%s", path);
        setvbuf(g_fp, NULL, _IOLBF, 0);
    }

    pthread_mutex_unlock(&g_lock);
    return 0;
}

void logger_set_level(int level)
{
    pthread_mutex_lock(&g_lock);
    g_level = level;
    pthread_mutex_unlock(&g_lock);
}

void logger_set_max_size(long bytes)
{
    pthread_mutex_lock(&g_lock);
    g_max_size = bytes;
    pthread_mutex_unlock(&g_lock);
}

void logger_write(int level, const char *file, int line, const char *fmt, ...)
{
    char      msg[LOG_LINE_MAX];
    char      ts[32];
    char      out[LOG_LINE_MAX + 96];
    time_t    now;
    struct tm tmv;
    va_list   ap;
    int       n;

    if (level > g_level)
    {
        return;
    }

    va_start(ap, fmt);
    n = vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    if (n < 0)
    {
        return;
    }

    now = time(NULL);
    memset(&tmv, 0, sizeof(tmv));
#if defined(_WIN32)
    {
        struct tm *ptm = localtime(&now);
        if (NULL != ptm)
        {
            tmv = *ptm;
        }
    }
#else
    (void)localtime_r(&now, &tmv);
#endif

    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tmv);

    snprintf(out, sizeof(out), "%s [%s] %s:%d  %s\n",
             ts, log_level_name(level), log_basename(file), line, msg);

    pthread_mutex_lock(&g_lock);

    /* 屏幕和控制台都给一份：板子上跑 systemd 时看 journal，
     * 手工前台跑的时候看屏幕，两边都不耽误 */
    fputs(out, stdout);

    if (NULL != g_fp)
    {
        fputs(out, g_fp);
        log_rotate_locked();
    }

    pthread_mutex_unlock(&g_lock);
}

void logger_close(void)
{
    pthread_mutex_lock(&g_lock);

    if (NULL != g_fp)
    {
        fflush(g_fp);
        fclose(g_fp);
        g_fp = NULL;
    }
    pthread_mutex_unlock(&g_lock);
}
