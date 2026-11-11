#include "storage.h"
#include "logger.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "sqlite3.h"

/*------------------------------------------------------------------
 * SQLite 落库
 *
 * 设计上的几个取舍，写在这里免得以后忘：
 *
 * 1) 只由 save_data 一个线程调用，所以全局没有额外加锁；
 *    一个连接多线程抢反而更慢。
 * 2) WAL + synchronous=NORMAL：写入不阻塞读，掉电最多丢最近几秒，
 *    对 1Hz 的监测数据完全够用，还能显著减少 SD 卡写放大。
 * 3) INSERT 只预编译一次，之后每条 bind -> step -> reset，
 *    比每次拼 SQL 快一个数量级。
 * 4) sensor_data 保持原样不动，告警单独一张 alarm_log，
 *    老库直接能用，不需要迁移。
 *----------------------------------------------------------------*/

static sqlite3      *g_db    = NULL;
static sqlite3_stmt *g_ins   = NULL;
static sqlite3_stmt *g_alarm = NULL;
static char          g_path[128];

int storage_init(const char *path)
{
    const char *db_path = (NULL == path) ? DB_PATH_DEFAULT : path;
    char *err = NULL;
    int rc;

    if (NULL != g_db)
    {
        return 0;
    }

    snprintf(g_path, sizeof(g_path), "%s", db_path);

    rc = sqlite3_open(db_path, &g_db);
    if (SQLITE_OK != rc)
    {
        LOG_ERROR("[storage] 打开 %s 失败: %s", db_path, sqlite3_errmsg(g_db));
        sqlite3_close(g_db);
        g_db = NULL;
        return -1;
    }

    /* 5 秒锁等待：查库的工具和程序同时开着也不会直接报 SQLITE_BUSY */
    sqlite3_busy_timeout(g_db, 5000);

    sqlite3_exec(g_db, "PRAGMA journal_mode=WAL;",   NULL, NULL, NULL);
    sqlite3_exec(g_db, "PRAGMA synchronous=NORMAL;", NULL, NULL, NULL);

    /* ---------------- 传感器数据表（沿用原表结构） ---------------- */
    rc = sqlite3_exec(g_db,
            "CREATE TABLE IF NOT EXISTS sensor_data ("
            "  rec_id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "  dev_id INTEGER,"
            "  temp   REAL,"
            "  hum    REAL,"
            "  volt   REAL,"
            "  curr   REAL,"
            "  smoke  REAL,"
            "  ts     TEXT);",
            NULL, NULL, &err);
    if (SQLITE_OK != rc)
    {
        LOG_ERROR("[storage] 建 sensor_data 失败: %s", err);
        sqlite3_free(err);
        sqlite3_close(g_db);
        g_db = NULL;
        return -1;
    }

    /* ---------------- 告警表（新增） ---------------- */
    rc = sqlite3_exec(g_db,
            "CREATE TABLE IF NOT EXISTS alarm_log ("
            "  id      INTEGER PRIMARY KEY AUTOINCREMENT,"
            "  dev_id  INTEGER,"
            "  flags   INTEGER,"
            "  level   INTEGER,"
            "  detail  TEXT,"
            "  temp    REAL,"
            "  hum     REAL,"
            "  volt    REAL,"
            "  curr    REAL,"
            "  smoke   REAL,"
            "  ts      TEXT);",
            NULL, NULL, &err);
    if (SQLITE_OK != rc)
    {
        LOG_ERROR("[storage] 建 alarm_log 失败: %s", err);
        sqlite3_free(err);
        sqlite3_close(g_db);
        g_db = NULL;
        return -1;
    }

    sqlite3_exec(g_db,
            "CREATE INDEX IF NOT EXISTS idx_sensor_ts ON sensor_data(ts);",
            NULL, NULL, NULL);
    sqlite3_exec(g_db,
            "CREATE INDEX IF NOT EXISTS idx_alarm_ts ON alarm_log(ts);",
            NULL, NULL, NULL);

    /* 预编译一次，之后每条只做 bind + step + reset */
    rc = sqlite3_prepare_v2(g_db,
            "INSERT INTO sensor_data(dev_id,temp,hum,volt,curr,smoke,ts) "
            "VALUES(?,?,?,?,?,?,?);",
            -1, &g_ins, NULL);
    if (SQLITE_OK != rc)
    {
        LOG_ERROR("[storage] 预编译 INSERT 失败: %s", sqlite3_errmsg(g_db));
        sqlite3_close(g_db);
        g_db = NULL;
        return -1;
    }

    rc = sqlite3_prepare_v2(g_db,
            "INSERT INTO alarm_log(dev_id,flags,level,detail,temp,hum,volt,curr,smoke,ts) "
            "VALUES(?,?,?,?,?,?,?,?,?,?);",
            -1, &g_alarm, NULL);
    if (SQLITE_OK != rc)
    {
        LOG_ERROR("[storage] 预编译告警 INSERT 失败: %s", sqlite3_errmsg(g_db));
        sqlite3_close(g_db);
        g_db = NULL;
        return -1;
    }

    LOG_INFO("[storage] 数据库就绪: %s（现有 %d 条历史数据）",
             db_path, storage_count());
    return 0;
}

/****************************************************************
 * 时间结构 -> "YYYY-MM-DD HH:MM:SS"
 ****************************************************************/
static int fmt_ts(const struct tm *tim, char *buf, size_t cap)
{
    if (NULL == tim || NULL == buf)
    {
        return -1;
    }

    if (0 == strftime(buf, cap, "%Y-%m-%d %H:%M:%S", tim))
    {
        return -1;
    }

    return 0;
}

int storage_insert(const struct msgdata *m)
{
    char ts[32] = {0};
    int rc;

    if (NULL == g_db || NULL == g_ins || NULL == m)
    {
        return -1;
    }

    if (0 != fmt_ts(&m->tim, ts, sizeof(ts)))
    {
        LOG_WARN("[storage] 时间格式化失败，本条丢弃");
        return -1;
    }

    sqlite3_bind_int   (g_ins, 1, m->id);
    sqlite3_bind_double(g_ins, 2, m->temp);
    sqlite3_bind_double(g_ins, 3, m->hum);
    sqlite3_bind_double(g_ins, 4, m->v);
    sqlite3_bind_double(g_ins, 5, m->a);
    sqlite3_bind_double(g_ins, 6, m->smoke);
    sqlite3_bind_text  (g_ins, 7, ts, -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(g_ins);
    if (SQLITE_DONE != rc)
    {
        LOG_ERROR("[storage] 写库失败: %s", sqlite3_errmsg(g_db));
        sqlite3_reset(g_ins);
        sqlite3_clear_bindings(g_ins);
        return -1;
    }

    /* 复位：不复位的话下一次 step 直接返回 SQLITE_MISUSE */
    sqlite3_reset(g_ins);
    sqlite3_clear_bindings(g_ins);

    return 0;
}

int storage_insert_alarm(const struct msgdata *m, const char *text)
{
    char ts[32] = {0};
    int rc;

    if (NULL == g_db || NULL == g_alarm || NULL == m)
    {
        return -1;
    }

    if (0 != fmt_ts(&m->tim, ts, sizeof(ts)))
    {
        return -1;
    }

    sqlite3_bind_int   (g_alarm, 1, m->id);
    sqlite3_bind_int   (g_alarm, 2, (int)m->alarm);
    sqlite3_bind_int   (g_alarm, 3, m->level);
    sqlite3_bind_text  (g_alarm, 4, (NULL == text) ? "" : text, -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(g_alarm, 5, m->temp);
    sqlite3_bind_double(g_alarm, 6, m->hum);
    sqlite3_bind_double(g_alarm, 7, m->v);
    sqlite3_bind_double(g_alarm, 8, m->a);
    sqlite3_bind_double(g_alarm, 9, m->smoke);
    sqlite3_bind_text  (g_alarm, 10, ts, -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(g_alarm);
    if (SQLITE_DONE != rc)
    {
        LOG_ERROR("[storage] 写告警失败: %s", sqlite3_errmsg(g_db));
        sqlite3_reset(g_alarm);
        sqlite3_clear_bindings(g_alarm);
        return -1;
    }

    sqlite3_reset(g_alarm);
    sqlite3_clear_bindings(g_alarm);

    return 0;
}

int storage_count(void)
{
    sqlite3_stmt *stmt = NULL;
    int n = -1;

    if (NULL == g_db)
    {
        return -1;
    }

    if (SQLITE_OK == sqlite3_prepare_v2(g_db,
            "SELECT COUNT(*) FROM sensor_data;", -1, &stmt, NULL))
    {
        if (SQLITE_ROW == sqlite3_step(stmt))
        {
            n = sqlite3_column_int(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }

    return n;
}

#if !defined(_WIN32)
/****************************************************************
 * "YYYY-MM-DD HH:MM:SS" -> struct tm
 * 库里存的是文本，读回来要转成结构体才能填 msgdata
 ****************************************************************/
static void parse_ts(const char *s, struct tm *out)
{
    struct tm tmv;

    if (NULL == s || NULL == out)
    {
        return;
    }

    memset(&tmv, 0, sizeof(tmv));
    if (NULL != strptime(s, "%Y-%m-%d %H:%M:%S", &tmv))
    {
        *out = tmv;
    }
}
#else
static void parse_ts(const char *s, struct tm *out)
{
    (void)s;
    if (NULL != out)
    {
        memset(out, 0, sizeof(*out));
    }
}
#endif

int storage_query_recent(struct msgdata *out, int max)
{
    sqlite3_stmt *stmt = NULL;
    int n = 0;

    if (NULL == g_db || NULL == out || max <= 0)
    {
        return -1;
    }

    if (SQLITE_OK != sqlite3_prepare_v2(g_db,
            "SELECT dev_id,temp,hum,volt,curr,smoke,ts FROM sensor_data "
            "ORDER BY rec_id DESC LIMIT ?;", -1, &stmt, NULL))
    {
        return -1;
    }

    sqlite3_bind_int(stmt, 1, max);

    while (SQLITE_ROW == sqlite3_step(stmt) && n < max)
    {
        memset(&out[n], 0, sizeof(out[n]));
        out[n].id    = sqlite3_column_int(stmt, 0);
        out[n].temp  = (float)sqlite3_column_double(stmt, 1);
        out[n].hum   = (float)sqlite3_column_double(stmt, 2);
        out[n].v     = (float)sqlite3_column_double(stmt, 3);
        out[n].a     = (float)sqlite3_column_double(stmt, 4);
        out[n].smoke = (float)sqlite3_column_double(stmt, 5);
        parse_ts((const char *)sqlite3_column_text(stmt, 6), &out[n].tim);
        n++;
    }

    sqlite3_finalize(stmt);
    return n;
}

int storage_export_csv(const char *csv_path, const char *from_ts, const char *to_ts)
{
    sqlite3_stmt *stmt = NULL;
    FILE         *fp   = NULL;
    const char   *sql  = NULL;
    int           n    = 0;
    int           idx  = 0;

    if (NULL == g_db || NULL == csv_path)
    {
        return -1;
    }

    if (NULL != from_ts && NULL != to_ts)
    {
        sql = "SELECT dev_id,temp,hum,volt,curr,smoke,ts FROM sensor_data "
              "WHERE ts>=? AND ts<=? ORDER BY rec_id;";
    }
    else if (NULL != from_ts)
    {
        sql = "SELECT dev_id,temp,hum,volt,curr,smoke,ts FROM sensor_data "
              "WHERE ts>=? ORDER BY rec_id;";
    }
    else
    {
        sql = "SELECT dev_id,temp,hum,volt,curr,smoke,ts FROM sensor_data "
              "ORDER BY rec_id;";
    }

    if (SQLITE_OK != sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL))
    {
        LOG_ERROR("[storage] 导出预编译失败: %s", sqlite3_errmsg(g_db));
        return -1;
    }

    if (NULL != from_ts)
    {
        sqlite3_bind_text(stmt, ++idx, from_ts, -1, SQLITE_TRANSIENT);

        if (NULL != to_ts)
        {
            sqlite3_bind_text(stmt, ++idx, to_ts, -1, SQLITE_TRANSIENT);
        }
    }

    fp = fopen(csv_path, "w");
    if (NULL == fp)
    {
        LOG_ERROR("[storage] 打不开导出文件 %s", csv_path);
        sqlite3_finalize(stmt);
        return -1;
    }

    /* 带 BOM，Windows 下用 Excel 直接双击打开不乱码 */
    fputs("\xEF\xBB\xBF", fp);
    fputs("dev_id,temp,hum,volt,curr,smoke,ts\n", fp);

    while (SQLITE_ROW == sqlite3_step(stmt))
    {
        const unsigned char *ts = sqlite3_column_text(stmt, 6);

        fprintf(fp, "%d,%.2f,%.2f,%.2f,%.2f,%.0f,%s\n",
                sqlite3_column_int(stmt, 0),
                sqlite3_column_double(stmt, 1),
                sqlite3_column_double(stmt, 2),
                sqlite3_column_double(stmt, 3),
                sqlite3_column_double(stmt, 4),
                sqlite3_column_double(stmt, 5),
                (NULL == ts) ? "" : (const char *)ts);
        n++;
    }

    sqlite3_finalize(stmt);
    fclose(fp);

    LOG_INFO("[storage] 已导出 %d 条到 %s", n, csv_path);
    return n;
}

int storage_cleanup(int retain_days)
{
    char sql[256];
    int  total = 0;

    if (NULL == g_db || retain_days <= 0)
    {
        return -1;
    }

    /* 用 SQLite 自己的 datetime 算，不依赖本地时区换算 */
    snprintf(sql, sizeof(sql),
             "DELETE FROM sensor_data WHERE ts < datetime('now','localtime','-%d day');",
             retain_days);
    if (SQLITE_OK == sqlite3_exec(g_db, sql, NULL, NULL, NULL))
    {
        total += sqlite3_changes(g_db);
    }

    snprintf(sql, sizeof(sql),
             "DELETE FROM alarm_log WHERE ts < datetime('now','localtime','-%d day');",
             retain_days);
    if (SQLITE_OK == sqlite3_exec(g_db, sql, NULL, NULL, NULL))
    {
        total += sqlite3_changes(g_db);
    }

    if (total > 0)
    {
        /* 删完把空间还回去，不然 db 文件只会一直变大 */
        sqlite3_exec(g_db, "PRAGMA incremental_vacuum;", NULL, NULL, NULL);
        sqlite3_exec(g_db, "PRAGMA wal_checkpoint(TRUNCATE);", NULL, NULL, NULL);
        LOG_INFO("[storage] 清理 %d 天前数据，删除 %d 条", retain_days, total);
    }
    else
    {
        LOG_DEBUG("[storage] 没有需要清理的历史数据（保留 %d 天）", retain_days);
    }

    return total;
}

void storage_close(void)
{
    if (NULL != g_ins)
    {
        sqlite3_finalize(g_ins);
        g_ins = NULL;
    }
    if (NULL != g_alarm)
    {
        sqlite3_finalize(g_alarm);
        g_alarm = NULL;
    }
    if (NULL != g_db)
    {
        /* 关库前做一次 checkpoint，把 WAL 合并回主库，
         * 断电重启后不用回放 WAL，打开更快 */
        sqlite3_exec(g_db, "PRAGMA wal_checkpoint(TRUNCATE);", NULL, NULL, NULL);
        sqlite3_close(g_db);
        g_db = NULL;
        LOG_INFO("[storage] 数据库已关闭: %s", g_path);
    }
}
