/*
 * DengFOC-ODrive single (STM32F405RG, 8 MHz HSE) USB DFU bootloader.
 *
 * Enters USB DFU (DfuSe, 0483:DF11) when:
 *   - the application at BOOT_APP_ADDR is missing/invalid, or
 *   - RESET is pressed twice within BOOT_DBLTAP_WINDOW_MS, or
 *   - the application wrote BOOT_MAGIC_DFU_REQ to BOOT_MAGIC_ADDR and reset.
 * Otherwise jumps to the application without touching clocks or GPIO,
 * so the power stage pins stay in their reset (input) state.
 */
#include "main.h"
#include "usbd_core.h"
#include "usbd_desc.h"
#include "usbd_dfu.h"
#include "usbd_dfu_flash.h"

#define BOOT_MAGIC (*(volatile uint32_t *)BOOT_MAGIC_ADDR)

USBD_HandleTypeDef hUsbDeviceFS;

static int app_is_valid(void)
{
  uint32_t sp = *(volatile uint32_t *)BOOT_APP_ADDR;
  uint32_t rv = *(volatile uint32_t *)(BOOT_APP_ADDR + 4U);
  int sp_ok = (sp > 0x20000000U && sp <= 0x20020000U) ||
              (sp > 0x10000000U && sp <= 0x10010000U);
  int rv_ok = ((rv & 1U) != 0U) && (rv > BOOT_APP_ADDR) && (rv < BOOT_FLASH_END);
  return sp_ok && rv_ok;
}

/* Busy-wait on SysTick at the reset clock (HSI 16 MHz), no interrupts used. */
static void delay_ms_hsi(uint32_t ms)
{
  SysTick->LOAD = (HSI_VALUE / 1000U) - 1U;
  SysTick->VAL = 0U;
  SysTick->CTRL = SysTick_CTRL_CLKSOURCE_Msk | SysTick_CTRL_ENABLE_Msk;
  while (ms-- > 0U)
  {
    while ((SysTick->CTRL & SysTick_CTRL_COUNTFLAG_Msk) == 0U)
    {
    }
  }
  SysTick->CTRL = 0U;
  SysTick->LOAD = 0U;
  SysTick->VAL = 0U;
}

static void jump_to_app(void)
{
  uint32_t sp = *(volatile uint32_t *)BOOT_APP_ADDR;
  uint32_t rv = *(volatile uint32_t *)(BOOT_APP_ADDR + 4U);

  SCB->VTOR = BOOT_APP_ADDR;
  __DSB();
  __ISB();
  __set_MSP(sp);
  ((void (*)(void))rv)();
  while (1)
  {
  }
}

static void run_dfu(void)
{
  HAL_Init();
  SystemClock_Config();

  if (USBD_Init(&hUsbDeviceFS, &FS_Desc, DEVICE_FS) != USBD_OK)
  {
    Error_Handler();
  }
  if (USBD_RegisterClass(&hUsbDeviceFS, &USBD_DFU) != USBD_OK)
  {
    Error_Handler();
  }
  if (USBD_DFU_RegisterMedia(&hUsbDeviceFS, &USBD_DFU_Flash_fops) != USBD_OK)
  {
    Error_Handler();
  }
  if (USBD_Start(&hUsbDeviceFS) != USBD_OK)
  {
    Error_Handler();
  }

  while (1)
  {
    __WFI();
  }
}

int main(void)
{
  uint32_t magic = BOOT_MAGIC;

  if (magic == BOOT_MAGIC_DFU_REQ)
  {
    BOOT_MAGIC = 0U;
    run_dfu();
  }
  if (!app_is_valid())
  {
    BOOT_MAGIC = 0U;
    run_dfu();
  }
  if (magic == BOOT_MAGIC_DBLTAP)
  {
    BOOT_MAGIC = 0U;
    run_dfu();
  }

  /* Arm the double-tap window: a second RESET inside it lands in DFU. */
  BOOT_MAGIC = BOOT_MAGIC_DBLTAP;
  delay_ms_hsi(BOOT_DBLTAP_WINDOW_MS);
  BOOT_MAGIC = 0U;

  jump_to_app();
  return 0;
}

/* SYSCLK 168 MHz from 8 MHz HSE, USB 48 MHz (M=8, N=336, P=2, Q=7). */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 8;
  RCC_OscInitStruct.PLL.PLLN = 336;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = 7;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK |
                                RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;
  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_5) != HAL_OK)
  {
    Error_Handler();
  }
}

void Error_Handler(void)
{
  __disable_irq();
  while (1)
  {
  }
}
