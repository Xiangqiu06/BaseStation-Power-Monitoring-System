#===================================================================
# 后备电源检测系统 —— 顶层构建入口
#
#   make            编译
#   make run        用默认配置前台跑（Ctrl+C 退出）
#   make test       队列单元测试
#   make clean      清理
#   make install    装到 /opt/basestation 并注册 systemd 服务
#
# 板子上交叉编译：
#   make CC=arm-linux-gnueabihf-gcc
#===================================================================

CC      ?= gcc
PREFIX  ?= /opt/basestation
SRCDIR   = src

.PHONY: all run test clean install uninstall help

all:
	$(MAKE) -C $(SRCDIR) CC=$(CC)

run: all
	cd $(SRCDIR) && ./app -c ../config/sensor.conf

test:
	$(MAKE) -C $(SRCDIR) test CC=$(CC)

clean:
	$(MAKE) -C $(SRCDIR) clean

install: all
	install -d $(PREFIX)/bin $(PREFIX)/config /var/lib/basestation /var/log/basestation
	install -m 755 $(SRCDIR)/app $(PREFIX)/bin/basestation
	install -m 644 config/sensor.conf $(PREFIX)/config/sensor.conf
	@echo "已安装到 $(PREFIX)，服务文件见 scripts/sensor-monitor.service"

uninstall:
	rm -f $(PREFIX)/bin/basestation
	rm -rf $(PREFIX)

help:
	@echo "make          编译"
	@echo "make run      用默认配置运行"
	@echo "make test     队列单元测试"
	@echo "make clean    清理"
	@echo "make install  安装到 $(PREFIX)"
