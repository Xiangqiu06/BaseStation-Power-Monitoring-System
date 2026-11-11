#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
模拟上位机接收端，用来联调 uploader.c

用法:
    python3 tools/fake_server.py            # 监听 0.0.0.0:9527
    python3 tools/fake_server.py 9527       # 指定端口

收到的每一行都会带时间戳打印出来；收到 ALARM 行会用 [!!] 标出来。
按 Ctrl+C 退出。

如果想让程序感受一下断网，直接把这个脚本关掉再开起来，
程序会按 1s/2s/4s... 退避重连，并把断网期间的数据补发上来。
"""

import socket
import sys
import threading
import time

HOST = "0.0.0.0"
PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 9527

lock = threading.Lock()
total = {"data": 0, "alarm": 0}


def handle(conn, addr):
    peer = "%s:%d" % addr
    print("[+] 客户端接入 %s" % peer)
    buf = b""
    try:
        while True:
            chunk = conn.recv(4096)
            if not chunk:
                break
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                text = line.decode("utf-8", "replace").strip()
                if not text:
                    continue
                stamp = time.strftime("%H:%M:%S")
                if text.startswith("ALARM"):
                    with lock:
                        total["alarm"] += 1
                    print("[%s] [!!] %s" % (stamp, text))
                elif text.startswith("DATA"):
                    with lock:
                        total["data"] += 1
                    print("[%s]      %s" % (stamp, text))
                else:
                    print("[%s] [sys] %s" % (stamp, text))
    except ConnectionResetError:
        pass
    finally:
        conn.close()
        print("[-] 客户端断开 %s" % peer)


def main():
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((HOST, PORT))
    srv.listen(8)
    print("模拟上位机已启动，监听 %s:%d，等板子上报..." % (HOST, PORT))
    try:
        while True:
            conn, addr = srv.accept()
            threading.Thread(target=handle, args=(conn, addr), daemon=True).start()
    except KeyboardInterrupt:
        print("\n收到 DATA %d 条，ALARM %d 条，退出" % (total["data"], total["alarm"]))
    finally:
        srv.close()


if __name__ == "__main__":
    main()
