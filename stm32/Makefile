#-------------------------------------------------------------------
# 后备电源检测系统 —— STM32F103RCT6 下位机固件
#
#   make                 编节点固件  -> build/node.elf / .hex / .bin
#   make ROLE=gateway    编网关固件  -> build/gateway.elf / .hex / .bin
#   make NODE=3          指定节点号（默认 1）
#   make flash           用 st-flash 烧录（需要装 stlink 工具）
#   make clean
#
# 工具链：arm-none-eabi-gcc（CubeIDE 自带或者 apt install gcc-arm-none-eabi）
#-------------------------------------------------------------------

# CubeIDE 自带的工具链路径（Windows）。Linux 下用系统自带的就行
TOOLCHAIN ?= /c/App/STM32CubeIDE/STM32CubeIDE_2.2.0/STM32CubeIDE/plugins/com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.14.3.rel1.win32_1.0.100.202602081740/tools/bin

ifeq ($(OS),Windows_NT)
  # Windows 下让 make 用 Unix 风格的 shell（Git Bash / MSYS2）
  SHELL := cmd.exe
  CC    := $(TOOLCHAIN)/arm-none-eabi-gcc.exe
  AS    := $(TOOLCHAIN)/arm-none-eabi-gcc.exe
  OBJCOPY := $(TOOLCHAIN)/arm-none-eabi-objcopy.exe
  SIZE  := $(TOOLCHAIN)/arm-none-eabi-size.exe
else
  CC    ?= arm-none-eabi-gcc
  AS    ?= arm-none-eabi-gcc
  OBJCOPY ?= arm-none-eabi-objcopy
  SIZE  ?= arm-none-eabi-size
endif

ROLE ?= node
NODE ?= 1

ifeq ($(ROLE),gateway)
  ROLE_DEF := -DAPP_ROLE=ROLE_GATEWAY
  TARGET   := gateway
else
  ROLE_DEF := -DAPP_ROLE=ROLE_NODE
  TARGET   := node
endif

BUILD := build

#------------------------- 源文件 -------------------------
C_SRCS := \
  Core/main.c \
  Core/system_clock.c \
  BSP/bsp_uart.c \
  BSP/bsp_adc.c \
  BSP/bsp_ds18b20.c \
  BSP/bsp_sx1278.c \
  BSP/bsp_modbus_slave.c \
  App/app_state.c \
  App/soc.c \
  App/bat_task.c \
  App/lora_task.c

# 网关角色用不到 ADC/采样任务，但一起编也没关系（体积由 --gc-sections 回收）
ASM_SRCS := Core/startup_stm32f103xe_gcc.s

RTOS_SRCS := \
  Middlewares/FreeRTOS/tasks.c \
  Middlewares/FreeRTOS/list.c \
  Middlewares/FreeRTOS/queue.c \
  Middlewares/FreeRTOS/timers.c \
  Middlewares/FreeRTOS/portable/GCC/ARM_CM3/port.c \
  Middlewares/FreeRTOS/portable/MemMang/heap_4.c

#------------------------- 编译选项 -------------------------
MCU     := -mcpu=cortex-m3 -mthumb
DEFS    := -DSTM32F103xE -DUSE_FULL_LL_DRIVER $(ROLE_DEF) -DNODE_ID=$(NODE)
INCS    := -ICore -IBSP -IApp \
           -IDrivers/CMSIS/Include \
           -IDrivers/CMSIS/Device/ST/STM32F1xx/Include \
           -IMiddlewares/FreeRTOS/include \
           -IMiddlewares/FreeRTOS/portable/GCC/ARM_CM3

# -O2 + 函数/数据分段，链接时用 --gc-sections 把没用到的代码剔掉
CFLAGS  := $(MCU) $(DEFS) $(INCS) -O2 -g3 -Wall -Wextra \
           -ffunction-sections -fdata-sections -fno-common \
           -std=gnu11

# 不用 nano.specs：界面/日志里有 %f，nano 版 printf 不支持浮点
LDFLAGS := $(MCU) -TSTM32F103RCTx_FLASH.ld \
           -specs=nosys.specs -Wl,--gc-sections \
           -Wl,-Map=$(BUILD)/$(TARGET).map -lm

OBJS := $(addprefix $(BUILD)/,$(notdir $(C_SRCS:.c=.o)))
OBJS += $(addprefix $(BUILD)/,$(notdir $(ASM_SRCS:.s=.o)))
OBJS += $(addprefix $(BUILD)/,$(notdir $(RTOS_SRCS:.c=.o)))

vpath %.c Core BSP App Middlewares/FreeRTOS Middlewares/FreeRTOS/portable/GCC/ARM_CM3 Middlewares/FreeRTOS/portable/MemMang
vpath %.s Core

.PHONY: all clean flash size

all: $(BUILD)/$(TARGET).elf $(BUILD)/$(TARGET).hex $(BUILD)/$(TARGET).bin size

$(BUILD):
	@mkdir -p $(BUILD)

$(BUILD)/%.o: %.c | $(BUILD)
	@echo "CC  $<"
	@$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: %.s | $(BUILD)
	@echo "AS  $<"
	@$(AS) $(MCU) -c $< -o $@

$(BUILD)/$(TARGET).elf: $(OBJS)
	@echo "LD  $@"
	@$(CC) $(OBJS) $(LDFLAGS) -o $@

$(BUILD)/$(TARGET).hex: $(BUILD)/$(TARGET).elf
	@$(OBJCOPY) -O ihex $< $@

$(BUILD)/$(TARGET).bin: $(BUILD)/$(TARGET).elf
	@$(OBJCOPY) -O binary $< $@

size: $(BUILD)/$(TARGET).elf
	@$(SIZE) $(BUILD)/$(TARGET).elf

flash: $(BUILD)/$(TARGET).bin
	st-flash write $(BUILD)/$(TARGET).bin 0x8000000

clean:
	@rm -rf $(BUILD)
	@echo "clean done"
