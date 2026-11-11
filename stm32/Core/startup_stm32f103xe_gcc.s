/*********************************************************************
 * GNU as 语法的启动文件（STM32F103RCT6）
 *
 * CubeMX 生成的工程默认给 Keil 的 armasm 版启动文件，用
 * arm-none-eabi-gcc 编译不了，所以这里自己写一份：
 *   - 建中断向量表（顺序必须和 RM0008 的 NVIC 表一致，错一位就进错中断）
 *   - Reset_Handler：拷 .data、清 .bss、调 SystemInit、跳 main
 *   - 所有中断默认指向 Default_Handler，C 里定义了同名函数就自动覆盖
 *********************************************************************/

  .syntax unified
  .cpu cortex-m3
  .thumb

  .global g_pfnVectors
  .global Reset_Handler

  .section .isr_vector, "a", %progbits
  .type g_pfnVectors, %object
g_pfnVectors:
  .word _estack
  .word Reset_Handler
  .word NMI_Handler
  .word HardFault_Handler
  .word MemManage_Handler
  .word BusFault_Handler
  .word UsageFault_Handler
  .word 0
  .word 0
  .word 0
  .word 0
  .word SVC_Handler
  .word DebugMon_Handler
  .word 0
  .word PendSV_Handler
  .word SysTick_Handler

  /* 外部中断 0 ~ 59 */
  .word WWDG_IRQHandler
  .word PVD_IRQHandler
  .word TAMPER_IRQHandler
  .word RTC_IRQHandler
  .word FLASH_IRQHandler
  .word RCC_IRQHandler
  .word EXTI0_IRQHandler
  .word EXTI1_IRQHandler
  .word EXTI2_IRQHandler
  .word EXTI3_IRQHandler
  .word EXTI4_IRQHandler
  .word DMA1_Channel1_IRQHandler
  .word DMA1_Channel2_IRQHandler
  .word DMA1_Channel3_IRQHandler
  .word DMA1_Channel4_IRQHandler
  .word DMA1_Channel5_IRQHandler
  .word DMA1_Channel6_IRQHandler
  .word DMA1_Channel7_IRQHandler
  .word ADC1_2_IRQHandler
  .word USB_HP_CAN1_TX_IRQHandler
  .word USB_LP_CAN1_RX0_IRQHandler
  .word CAN1_RX1_IRQHandler
  .word CAN1_SCE_IRQHandler
  .word EXTI9_5_IRQHandler
  .word TIM1_BRK_IRQHandler
  .word TIM1_UP_IRQHandler
  .word TIM1_TRG_COM_IRQHandler
  .word TIM1_CC_IRQHandler
  .word TIM2_IRQHandler
  .word TIM3_IRQHandler
  .word TIM4_IRQHandler
  .word I2C1_EV_IRQHandler
  .word I2C1_ER_IRQHandler
  .word I2C2_EV_IRQHandler
  .word I2C2_ER_IRQHandler
  .word SPI1_IRQHandler
  .word SPI2_IRQHandler
  .word USART1_IRQHandler
  .word USART2_IRQHandler
  .word USART3_IRQHandler
  .word EXTI15_10_IRQHandler
  .word RTCAlarm_IRQHandler
  .word USBWakeUp_IRQHandler
  .word TIM8_BRK_IRQHandler
  .word TIM8_UP_IRQHandler
  .word TIM8_TRG_COM_IRQHandler
  .word TIM8_CC_IRQHandler
  .word ADC3_IRQHandler
  .word FSMC_IRQHandler
  .word SDIO_IRQHandler
  .word TIM5_IRQHandler
  .word SPI3_IRQHandler
  .word UART4_IRQHandler
  .word UART5_IRQHandler
  .word TIM6_IRQHandler
  .word TIM7_IRQHandler
  .word DMA2_Channel1_IRQHandler
  .word DMA2_Channel2_IRQHandler
  .word DMA2_Channel3_IRQHandler
  .word DMA2_Channel4_5_IRQHandler

  .size g_pfnVectors, . - g_pfnVectors

  .section .text.Reset_Handler, "ax", %progbits
  .weak Reset_Handler
  .type Reset_Handler, %function
Reset_Handler:
  /* 先把栈指针设为向量表第 0 项（LDR 方式，比直接 mov 安全） */
  ldr   r0, =_estack
  mov   sp, r0

  /* 把 .data 从 Flash 拷到 RAM */
  ldr   r0, =_sdata
  ldr   r1, =_sidata
  ldr   r2, =_edata
copy_data:
  cmp   r0, r2
  bcs   copy_data_done
  ldr   r3, [r1], #4
  str   r3, [r0], #4
  b     copy_data
copy_data_done:

  /* .bss 清零 */
  ldr   r0, =_sbss
  ldr   r1, =_ebss
  movs  r2, #0
clear_bss:
  cmp   r0, r1
  bcs   clear_bss_done
  str   r2, [r0]
  adds  r0, r0, #4
  b     clear_bss
clear_bss_done:

  /* 开 FPU 前的准备这里不需要（M3 没有 FPU），直接配时钟 */
  bl    SystemInit

  /* 如果用到 C++ 静态构造或者 libc 的 init 段，这里调一次 */
  bl    __libc_init_array

  bl    main

  /* main 正常不应该返回，返回了就死循环 */
loop_forever:
  b     loop_forever

  .size Reset_Handler, . - Reset_Handler

/*********************************************************************
 * 默认中断处理：所有中断先指向这里，C 里定义了同名函数就覆盖
 *********************************************************************/
  .section .text.Default_Handler, "ax", %progbits
  .type Default_Handler, %function
Default_Handler:
  b Default_Handler
  .size Default_Handler, . - Default_Handler

  .macro weak_handler name
  .weak \name
  .thumb_set \name, Default_Handler
  .endm

  weak_handler NMI_Handler
  weak_handler HardFault_Handler
  weak_handler MemManage_Handler
  weak_handler BusFault_Handler
  weak_handler UsageFault_Handler
  weak_handler SVC_Handler
  weak_handler DebugMon_Handler
  weak_handler PendSV_Handler
  weak_handler SysTick_Handler

  weak_handler WWDG_IRQHandler
  weak_handler PVD_IRQHandler
  weak_handler TAMPER_IRQHandler
  weak_handler RTC_IRQHandler
  weak_handler FLASH_IRQHandler
  weak_handler RCC_IRQHandler
  weak_handler EXTI0_IRQHandler
  weak_handler EXTI1_IRQHandler
  weak_handler EXTI2_IRQHandler
  weak_handler EXTI3_IRQHandler
  weak_handler EXTI4_IRQHandler
  weak_handler DMA1_Channel1_IRQHandler
  weak_handler DMA1_Channel2_IRQHandler
  weak_handler DMA1_Channel3_IRQHandler
  weak_handler DMA1_Channel4_IRQHandler
  weak_handler DMA1_Channel5_IRQHandler
  weak_handler DMA1_Channel6_IRQHandler
  weak_handler DMA1_Channel7_IRQHandler
  weak_handler ADC1_2_IRQHandler
  weak_handler USB_HP_CAN1_TX_IRQHandler
  weak_handler USB_LP_CAN1_RX0_IRQHandler
  weak_handler CAN1_RX1_IRQHandler
  weak_handler CAN1_SCE_IRQHandler
  weak_handler EXTI9_5_IRQHandler
  weak_handler TIM1_BRK_IRQHandler
  weak_handler TIM1_UP_IRQHandler
  weak_handler TIM1_TRG_COM_IRQHandler
  weak_handler TIM1_CC_IRQHandler
  weak_handler TIM2_IRQHandler
  weak_handler TIM3_IRQHandler
  weak_handler TIM4_IRQHandler
  weak_handler I2C1_EV_IRQHandler
  weak_handler I2C1_ER_IRQHandler
  weak_handler I2C2_EV_IRQHandler
  weak_handler I2C2_ER_IRQHandler
  weak_handler SPI1_IRQHandler
  weak_handler SPI2_IRQHandler
  weak_handler USART1_IRQHandler
  weak_handler USART2_IRQHandler
  weak_handler USART3_IRQHandler
  weak_handler EXTI15_10_IRQHandler
  weak_handler RTCAlarm_IRQHandler
  weak_handler USBWakeUp_IRQHandler
  weak_handler TIM8_BRK_IRQHandler
  weak_handler TIM8_UP_IRQHandler
  weak_handler TIM8_TRG_COM_IRQHandler
  weak_handler TIM8_CC_IRQHandler
  weak_handler ADC3_IRQHandler
  weak_handler FSMC_IRQHandler
  weak_handler SDIO_IRQHandler
  weak_handler TIM5_IRQHandler
  weak_handler SPI3_IRQHandler
  weak_handler UART4_IRQHandler
  weak_handler UART5_IRQHandler
  weak_handler TIM6_IRQHandler
  weak_handler TIM7_IRQHandler
  weak_handler DMA2_Channel1_IRQHandler
  weak_handler DMA2_Channel2_IRQHandler
  weak_handler DMA2_Channel3_IRQHandler
  weak_handler DMA2_Channel4_5_IRQHandler
