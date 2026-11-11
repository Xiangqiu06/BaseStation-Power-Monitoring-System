#-------------------------------------------------------------------
# 后备电源检测系统
#
#   make          编出 app（主程序，含 LVGL 界面）
#   make noui     编一个不带界面的版本（app_noui），没有屏幕时用
#   make test     编并跑队列单元测试
#   make clean    清理中间产物
#   make memcheck valgrind 查内存泄漏
#
# 交叉编译（i.MX6ULL 板子）：
#   make CC=arm-linux-gnueabihf-gcc
#
# 目录约定：
#   ../include               本项目头文件
#   ../third_party/lvgl      LVGL 8.4 核心（只保留 src，去掉 demos/examples）
#   ../third_party/lv_conf.h LVGL 配置（放在 lvgl 同级，LVGL 默认往这儿找）
#   ../ui/drivers            fbdev/evdev 驱动层 + lv_drv_conf.h
#   ../ui/fonts              界面中文字体（tools/gen_font.sh 生成）
#   ../ui/squareline         SquareLine Studio 界面源文件（参考用，不参与编译）
#-------------------------------------------------------------------

TARGET   := app
TARGET_N := app_noui

CC       ?= gcc
AR       ?= ar

INC      := -I../include -I../third_party -I../third_party/sqlite -I../ui/drivers

CFLAGS   := -g -O2 -Wall -Wextra -D_GNU_SOURCE $(INC)
LDFLAGS  := -lpthread -lm

# 第三方代码用这套：不套 -Wall，免得一堆无关警告把重点刷没
CFLAGS_3RD := -g -O2 -D_GNU_SOURCE $(INC)

BUILD    := build

#------------------------- 应用源码 -------------------------
APP_SRCS := main.c \
            linklist.c \
            linkqueue.c \
            mailbox.c \
            data_source.c \
            serial_port.c \
            protocol.c \
            modbus.c \
            alarm.c \
            storage.c \
            uploader.c \
            mqtt.c \
            camera.c \
            config.c \
            logger.c

UI_SRCS  := ui_app.c \
            ../ui/fonts/ui_font_cn_16.c \
            ../ui/fonts/ui_font_cn_28.c

# 不带界面的版本：把手写界面和字体剔掉，换成空实现，不链 LVGL
NOUI_SRCS := $(filter-out ui_app.c,$(APP_SRCS))

#------------------------- LVGL -------------------------
LVGL_SRCS  := $(shell find ../third_party/lvgl/src -name '*.c')
LVGL_LIB   := $(BUILD)/liblvgl.a

LVDRV_SRCS := ../ui/drivers/lv_drivers/display/fbdev.c \
              ../ui/drivers/lv_drivers/indev/evdev.c

#------------------------- SQLite -------------------------
SQLITE_DIR := ../third_party/sqlite
SQLITE_OBJ := $(BUILD)/sqlite3.o

#------------------------- 目标 -------------------------
.PHONY: all noui test clean memcheck

all: $(TARGET)

$(TARGET): $(APP_SRCS) $(UI_SRCS) $(LVDRV_SRCS) $(LVGL_LIB) $(SQLITE_OBJ) $(wildcard ../include/*.h)
	@mkdir -p $(BUILD)
	$(CC) $(APP_SRCS) $(UI_SRCS) $(LVDRV_SRCS) $(LVGL_LIB) $(SQLITE_OBJ) \
	    -o $@ $(CFLAGS) $(LDFLAGS)
	@echo "编译完成: src/$(TARGET)"

# 无界面版：不链 LVGL，体积小、启动快，适合只做数据采集的部署
noui: $(NOUI_SRCS) ui_stub.c $(SQLITE_OBJ) $(wildcard ../include/*.h)
	@mkdir -p $(BUILD)
	$(CC) $(NOUI_SRCS) ui_stub.c $(SQLITE_OBJ) -o $(TARGET_N) \
	    $(CFLAGS) -DUI_DISABLED $(LDFLAGS)
	@echo "编译完成: src/$(TARGET_N)（无界面）"

# LVGL 单独编成静态库：两百多个 .c，每次全重编太慢，
# 只有 LVGL 源码变化时才重编
$(LVGL_LIB): $(LVGL_SRCS) ../third_party/lv_conf.h
	@mkdir -p $(BUILD)/lvgl_obj
	@rm -f $(BUILD)/lvgl_obj/*.o
	@set -e; for f in $(LVGL_SRCS); do \
	    o=$(BUILD)/lvgl_obj/$$(echo $$f | sed 's|\.\./||; s|/|_|g; s|\.c$$|.o|'); \
	    $(CC) $(CFLAGS_3RD) -c $$f -o $$o; \
	done
	$(AR) rcs $@ $(BUILD)/lvgl_obj/*.o
	@echo "LVGL 静态库完成: $@"

# 第三方代码：单独编译，不套 -Wall -Wextra
$(SQLITE_OBJ): $(SQLITE_DIR)/sqlite3.c
	@mkdir -p $(BUILD)
	$(CC) -c $(SQLITE_DIR)/sqlite3.c -o $@ -O2 $(INC) -DSQLITE_THREADSAFE=1

# 队列单元测试：单独编，不参与主程序
test_queue: test_queue.c linkqueue.c $(wildcard ../include/*.h)
	$(CC) test_queue.c linkqueue.c -o $@ $(CFLAGS)

test: test_queue
	./test_queue

clean:
	rm -f $(TARGET) $(TARGET_N) test_queue
	rm -rf $(BUILD)

memcheck:
	valgrind --tool=memcheck --leak-check=full --show-leak-kinds=all ./$(TARGET)
