#ifndef __LOGGER_H__
#define __LOGGER_H__

/*------------------------------------------------------------------
 * 分级日志
 *
 * 原来各线程直接 printf，跑久了屏幕一屏一屏滚，
 * 板子上又没法翻，出问题只能靠猜。这里统一收口：
 *   - 按级别过滤，默认 INFO
 *   - 同时写文件（带时间戳、级别、文件名行号），文件带滚动
 *   - 内部加锁，多线程调用不会串行写花
 *----------------------------------------------------------------*/

/* 日志级别，数字越小越严重 */
enum
{
    LOG_LVL_ERROR = 0,
    LOG_LVL_WARN  = 1,
    LOG_LVL_INFO  = 2,
    LOG_LVL_DEBUG = 3
};

/* path 传 NULL 或空串表示只输出到标准输出 */
int  logger_init(const char *path, int level);
void logger_close(void);
void logger_set_level(int level);

/* 单文件上限（字节），超过就挪成 .1 再重开；0 表示不限 */
void logger_set_max_size(long bytes);

void logger_write(int level, const char *file, int line, const char *fmt, ...);

#define LOG_ERROR(...)  logger_write(LOG_LVL_ERROR, __FILE__, __LINE__, __VA_ARGS__)
#define LOG_WARN(...)   logger_write(LOG_LVL_WARN,  __FILE__, __LINE__, __VA_ARGS__)
#define LOG_INFO(...)   logger_write(LOG_LVL_INFO,  __FILE__, __LINE__, __VA_ARGS__)
#define LOG_DEBUG(...)  logger_write(LOG_LVL_DEBUG, __FILE__, __LINE__, __VA_ARGS__)

#endif
