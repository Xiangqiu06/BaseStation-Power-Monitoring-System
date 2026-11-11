#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
看一眼数据库里存了什么，不用装 sqlite3 命令行也能用

用法:
    python3 tools/db_view.py                     # 默认看 sensor.db
    python3 tools/db_view.py /var/lib/basestation/sensor.db
    python3 tools/db_view.py sensor.db 50        # 看最近 50 条
"""

import sqlite3
import sys

path = sys.argv[1] if len(sys.argv) > 1 else "sensor.db"
limit = int(sys.argv[2]) if len(sys.argv) > 2 else 20

con = sqlite3.connect(path)
cur = con.cursor()

cur.execute("SELECT name FROM sqlite_master WHERE type='table'")
tables = [r[0] for r in cur.fetchall()]
print("库文件: %s" % path)
print("表    : %s" % ", ".join(tables))

if "sensor_data" in tables:
    cur.execute("SELECT COUNT(*) FROM sensor_data")
    total = cur.fetchone()[0]
    cur.execute("SELECT MIN(ts), MAX(ts) FROM sensor_data")
    first, last = cur.fetchone()
    print("\n采集数据 %d 条，时间跨度 %s ~ %s" % (total, first, last))

    print("\n最近 %d 条:" % limit)
    print("%-6s %-8s %-8s %-8s %-8s %-8s %s" %
          ("dev", "temp", "hum", "volt", "curr", "smoke", "ts"))
    cur.execute(
        "SELECT dev_id,temp,hum,volt,curr,smoke,ts FROM sensor_data "
        "ORDER BY rec_id DESC LIMIT ?", (limit,))
    for row in cur.fetchall():
        print("%-6d %-8.2f %-8.2f %-8.2f %-8.2f %-8.0f %s" % row)

if "alarm_log" in tables:
    cur.execute("SELECT COUNT(*) FROM alarm_log")
    print("\n告警记录 %d 条:" % cur.fetchone()[0])
    cur.execute(
        "SELECT level,flags,detail,volt,curr,ts FROM alarm_log "
        "ORDER BY id DESC LIMIT 10")
    for level, flags, detail, volt, curr, ts in cur.fetchall():
        print("  [%d] 0x%02X %-20s V=%.2f I=%.2f  %s" %
              (level, flags, detail, volt, curr, ts))

con.close()
