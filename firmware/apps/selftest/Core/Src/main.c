/*
 * DengFOC-ODrive single board self-test (application at 0x08008000).
 *
 * USB CDC console, one command per line:
 *   info   chip ID, UID, VDDA, reset flags
 *   adc    voltages on every analog-capable pin
 *   pull   pull-up / pull-down probe of every non-power pin
 *   watch  report level changes on the GPIO header for 30 s (any key stops)
 *   enc    read the on-board AS5047P over SPI3 (CS = PB3)
 *   drv    read DRV8303 registers (only with main power > 9 V)
 *   dfu    reboot into the USB DFU bootloader
 *
 * The power stage is held off for the whole run: EN_GATE, AUX_H/AUX_L and all
 * six gate inputs are driven low at boot. Only `drv` raises EN_GATE, briefly,
 * with every gate input still low.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "main.h"
#include "usbd_core.h"
#include "usbd_desc.h"
#include "usbd_cdc.h"
#include "usbd_cdc_if.h"

USBD_HandleTypeDef hUsbDeviceFS;
static ADC_HandleTypeDef hadc1;
static SPI_HandleTypeDef hspi3;
static uint32_t boot_reset_flags;

typedef struct
{
  const char *name;
  GPIO_TypeDef *port;
  uint16_t pin;
} Pin;

typedef struct
{
  const char *name;
  GPIO_TypeDef *port;
  uint16_t pin;
  uint32_t channel;
  uint32_t mult;
} AdcPin;

static const AdcPin adc_pins[] =
{
  {"VBUS     PA6 (x11)", GPIOA, GPIO_PIN_6, ADC_CHANNEL_6, 11},
  {"M0_TEMP  PC5", GPIOC, GPIO_PIN_5, ADC_CHANNEL_15, 1},
  {"AUX_TEMP PA5", GPIOA, GPIO_PIN_5, ADC_CHANNEL_5, 1},
  {"M0_IB    PC0", GPIOC, GPIO_PIN_0, ADC_CHANNEL_10, 1},
  {"M0_IC    PC1", GPIOC, GPIO_PIN_1, ADC_CHANNEL_11, 1},
  {"M1_TEMP  PA4", GPIOA, GPIO_PIN_4, ADC_CHANNEL_4, 1},
  {"M1_IC    PC2", GPIOC, GPIO_PIN_2, ADC_CHANNEL_12, 1},
  {"M1_IB    PC3", GPIOC, GPIO_PIN_3, ADC_CHANNEL_13, 1},
  {"M1_AL    PA7", GPIOA, GPIO_PIN_7, ADC_CHANNEL_7, 1},
  {"M1_BL    PB0", GPIOB, GPIO_PIN_0, ADC_CHANNEL_8, 1},
  {"M1_CL    PB1", GPIOB, GPIO_PIN_1, ADC_CHANNEL_9, 1},
  {"Pin1     PA0", GPIOA, GPIO_PIN_0, ADC_CHANNEL_0, 1},
  {"Pin2     PA1", GPIOA, GPIO_PIN_1, ADC_CHANNEL_1, 1},
  {"Pin3     PA2", GPIOA, GPIO_PIN_2, ADC_CHANNEL_2, 1},
  {"Pin4     PA3", GPIOA, GPIO_PIN_3, ADC_CHANNEL_3, 1},
  {"Pin5     PC4", GPIOC, GPIO_PIN_4, ADC_CHANNEL_14, 1},
};

/* Every pin that is safe to float or pull. Power stage, USB, SWD, SPI3 and
 * CAN_TX are excluded. */
static const Pin probe_pins[] =
{
  {"Pin1 PA0", GPIOA, GPIO_PIN_0},
  {"Pin2 PA1", GPIOA, GPIO_PIN_1},
  {"Pin3 PA2", GPIOA, GPIO_PIN_2},
  {"Pin4 PA3", GPIOA, GPIO_PIN_3},
  {"Pin5 PC4", GPIOC, GPIO_PIN_4},
  {"Pin6 PB2", GPIOB, GPIO_PIN_2},
  {"Pin7 PA15", GPIOA, GPIO_PIN_15},
  {"Pin8/AS5047_CS PB3", GPIOB, GPIO_PIN_3},
  {"ENC_A PB4", GPIOB, GPIO_PIN_4},
  {"ENC_B PB5", GPIOB, GPIO_PIN_5},
  {"ENC_Z PC9", GPIOC, GPIO_PIN_9},
  {"VBUS PA6", GPIOA, GPIO_PIN_6},
  {"M0_TEMP PC5", GPIOC, GPIO_PIN_5},
  {"AUX_TEMP PA5", GPIOA, GPIO_PIN_5},
  {"M0_IB PC0", GPIOC, GPIO_PIN_0},
  {"M0_IC PC1", GPIOC, GPIO_PIN_1},
  {"M1_TEMP PA4", GPIOA, GPIO_PIN_4},
  {"M1_IC PC2", GPIOC, GPIO_PIN_2},
  {"M1_IB PC3", GPIOC, GPIO_PIN_3},
  {"M1_AL PA7", GPIOA, GPIO_PIN_7},
  {"M1_BL PB0", GPIOB, GPIO_PIN_0},
  {"M1_CL PB1", GPIOB, GPIO_PIN_1},
  {"M1_AH PC6", GPIOC, GPIO_PIN_6},
  {"M1_BH PC7", GPIOC, GPIO_PIN_7},
  {"M1_CH PC8", GPIOC, GPIO_PIN_8},
  {"M1_nCS PC14", GPIOC, GPIO_PIN_14},
  {"M1_ENC_Z PC15", GPIOC, GPIO_PIN_15},
  {"M1_ENC_A PB6", GPIOB, GPIO_PIN_6},
  {"M1_ENC_B PB7", GPIOB, GPIO_PIN_7},
  {"CAN_RX PB8", GPIOB, GPIO_PIN_8},
  {"nFAULT PD2", GPIOD, GPIO_PIN_2},
};

static const Pin header_pins[] =
{
  {"Pin1 PA0", GPIOA, GPIO_PIN_0},
  {"Pin2 PA1", GPIOA, GPIO_PIN_1},
  {"Pin3 PA2", GPIOA, GPIO_PIN_2},
  {"Pin4 PA3", GPIOA, GPIO_PIN_3},
  {"Pin5 PC4", GPIOC, GPIO_PIN_4},
  {"Pin6 PB2", GPIOB, GPIO_PIN_2},
  {"Pin7 PA15", GPIOA, GPIO_PIN_15},
};

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

static void out(const char *fmt, ...)
{
  char buf[200];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (n > 0)
  {
    cdc_write(buf, (uint32_t)((n < (int)sizeof(buf)) ? n : (int)sizeof(buf) - 1));
  }
}

static void SystemClock_Config(void)
{
  RCC_OscInitTypeDef osc = {0};
  RCC_ClkInitTypeDef clk = {0};

  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  osc.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  osc.HSEState = RCC_HSE_ON;
  osc.PLL.PLLState = RCC_PLL_ON;
  osc.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  osc.PLL.PLLM = 8;
  osc.PLL.PLLN = 336;
  osc.PLL.PLLP = RCC_PLLP_DIV2;
  osc.PLL.PLLQ = 7;
  if (HAL_RCC_OscConfig(&osc) != HAL_OK)
  {
    Error_Handler();
  }

  clk.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
  clk.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  clk.AHBCLKDivider = RCC_SYSCLK_DIV1;
  clk.APB1CLKDivider = RCC_HCLK_DIV4;
  clk.APB2CLKDivider = RCC_HCLK_DIV2;
  if (HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_5) != HAL_OK)
  {
    Error_Handler();
  }
}

/* Drive every power-stage input to its off state before anything else runs. */
static void power_stage_safe(void)
{
  GPIO_InitTypeDef g = {0};

  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOD_CLK_ENABLE();

  /* EN_GATE PB12, AUX_L PB10, AUX_H PB11, low-side gates PB13..15 */
  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_10 | GPIO_PIN_11 | GPIO_PIN_12 |
                    GPIO_PIN_13 | GPIO_PIN_14 | GPIO_PIN_15, GPIO_PIN_RESET);
  /* High-side gates PA8..10 */
  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_8 | GPIO_PIN_9 | GPIO_PIN_10, GPIO_PIN_RESET);
  /* SPI chip selects idle high: DRV8303 nSCS PC13, AS5047P CSn PB3 */
  HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, GPIO_PIN_SET);
  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_3, GPIO_PIN_SET);

  g.Mode = GPIO_MODE_OUTPUT_PP;
  g.Pull = GPIO_NOPULL;
  g.Speed = GPIO_SPEED_FREQ_LOW;
  g.Pin = GPIO_PIN_3 | GPIO_PIN_10 | GPIO_PIN_11 | GPIO_PIN_12 |
          GPIO_PIN_13 | GPIO_PIN_14 | GPIO_PIN_15;
  HAL_GPIO_Init(GPIOB, &g);
  g.Pin = GPIO_PIN_8 | GPIO_PIN_9 | GPIO_PIN_10;
  HAL_GPIO_Init(GPIOA, &g);
  g.Pin = GPIO_PIN_13;
  HAL_GPIO_Init(GPIOC, &g);
}

static void as5047_cs_restore(void)
{
  GPIO_InitTypeDef g = {0};
  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_3, GPIO_PIN_SET);
  g.Pin = GPIO_PIN_3;
  g.Mode = GPIO_MODE_OUTPUT_PP;
  g.Pull = GPIO_NOPULL;
  g.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOB, &g);
}

static void spi3_init(void)
{
  GPIO_InitTypeDef g = {0};

  __HAL_RCC_SPI3_CLK_ENABLE();
  g.Pin = GPIO_PIN_10 | GPIO_PIN_11 | GPIO_PIN_12;
  g.Mode = GPIO_MODE_AF_PP;
  g.Pull = GPIO_NOPULL;
  g.Speed = GPIO_SPEED_FREQ_MEDIUM;
  g.Alternate = GPIO_AF6_SPI3;
  HAL_GPIO_Init(GPIOC, &g);

  hspi3.Instance = SPI3;
  hspi3.Init.Mode = SPI_MODE_MASTER;
  hspi3.Init.Direction = SPI_DIRECTION_2LINES;
  hspi3.Init.DataSize = SPI_DATASIZE_16BIT;
  hspi3.Init.CLKPolarity = SPI_POLARITY_LOW;
  hspi3.Init.CLKPhase = SPI_PHASE_2EDGE;
  hspi3.Init.NSS = SPI_NSS_SOFT;
  hspi3.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_32; /* 42 MHz / 32 = 1.3 MHz */
  hspi3.Init.FirstBit = SPI_FIRSTBIT_MSB;
  hspi3.Init.TIMode = SPI_TIMODE_DISABLE;
  hspi3.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
  hspi3.Init.CRCPolynomial = 10;
  if (HAL_SPI_Init(&hspi3) != HAL_OK)
  {
    Error_Handler();
  }
}

static uint16_t spi_xfer(GPIO_TypeDef *cs_port, uint16_t cs_pin, uint16_t word)
{
  uint16_t rx = 0;
  HAL_GPIO_WritePin(cs_port, cs_pin, GPIO_PIN_RESET);
  (void)HAL_SPI_TransmitReceive(&hspi3, (uint8_t *)&word, (uint8_t *)&rx, 1, 10);
  HAL_GPIO_WritePin(cs_port, cs_pin, GPIO_PIN_SET);
  for (volatile int i = 0; i < 100; i++)
  {
  }
  return rx;
}

static void adc_init(void)
{
  __HAL_RCC_ADC1_CLK_ENABLE();
  hadc1.Instance = ADC1;
  hadc1.Init.ClockPrescaler = ADC_CLOCK_SYNC_PCLK_DIV4;
  hadc1.Init.Resolution = ADC_RESOLUTION_12B;
  hadc1.Init.ScanConvMode = DISABLE;
  hadc1.Init.ContinuousConvMode = DISABLE;
  hadc1.Init.DiscontinuousConvMode = DISABLE;
  hadc1.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_NONE;
  hadc1.Init.ExternalTrigConv = ADC_SOFTWARE_START;
  hadc1.Init.DataAlign = ADC_DATAALIGN_RIGHT;
  hadc1.Init.NbrOfConversion = 1;
  hadc1.Init.DMAContinuousRequests = DISABLE;
  hadc1.Init.EOCSelection = ADC_EOC_SINGLE_CONV;
  if (HAL_ADC_Init(&hadc1) != HAL_OK)
  {
    Error_Handler();
  }
}

static uint32_t adc_read_raw(uint32_t channel)
{
  ADC_ChannelConfTypeDef c = {0};
  uint32_t sum = 0;

  c.Channel = channel;
  c.Rank = 1;
  c.SamplingTime = ADC_SAMPLETIME_480CYCLES;
  (void)HAL_ADC_ConfigChannel(&hadc1, &c);
  for (int i = 0; i < 16; i++)
  {
    HAL_ADC_Start(&hadc1);
    (void)HAL_ADC_PollForConversion(&hadc1, 10);
    sum += HAL_ADC_GetValue(&hadc1);
  }
  HAL_ADC_Stop(&hadc1);
  return sum / 16U;
}

static uint32_t vdda_mv(void)
{
  uint32_t cal = *(const volatile uint16_t *)0x1FFF7A2AU; /* VREFINT at 3.3 V */
  uint32_t raw = adc_read_raw(ADC_CHANNEL_VREFINT);
  return (raw == 0U) ? 3300U : (3300U * cal) / raw;
}

static void set_mode(GPIO_TypeDef *port, uint16_t pin, uint32_t mode, uint32_t pull)
{
  GPIO_InitTypeDef g = {0};
  g.Pin = pin;
  g.Mode = mode;
  g.Pull = pull;
  g.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(port, &g);
}

static int read_with_pull(const Pin *p, uint32_t pull)
{
  set_mode(p->port, p->pin, GPIO_MODE_INPUT, pull);
  HAL_Delay(3);
  return HAL_GPIO_ReadPin(p->port, p->pin) == GPIO_PIN_SET;
}

/* ---------------------------------------------------------------- commands */

static void cmd_info(void)
{
  const volatile uint32_t *uid = (const volatile uint32_t *)0x1FFF7A10U;
  out("IDCODE=0x%08lX  FLASH=%u KB  UID=%08lX-%08lX-%08lX\r\n",
      DBGMCU->IDCODE, *(const volatile uint16_t *)0x1FFF7A22U, uid[0], uid[1], uid[2]);
  out("SYSCLK=%lu Hz  VTOR=0x%08lX  boot RCC_CSR=0x%08lX\r\n",
      HAL_RCC_GetSysClockFreq(), SCB->VTOR, boot_reset_flags);
  uint32_t vdda = vdda_mv();
  uint32_t traw = adc_read_raw(ADC_CHANNEL_TEMPSENSOR);
  int32_t tmv = (int32_t)((traw * vdda) / 4095U);
  int32_t t10 = ((tmv - 760) * 10 * 10) / 25 + 250; /* 0.76 V @25 C, 2.5 mV/C */
  out("VDDA=%lu mV  MCU temp=%ld.%ld C\r\n", vdda, t10 / 10, (t10 < 0 ? -t10 : t10) % 10);
}

static void cmd_adc(void)
{
  uint32_t vdda = vdda_mv();
  out("VDDA=%lu mV (all values averaged over 16 samples)\r\n", vdda);
  for (size_t i = 0; i < ARRAY_LEN(adc_pins); i++)
  {
    const AdcPin *a = &adc_pins[i];
    set_mode(a->port, a->pin, GPIO_MODE_ANALOG, GPIO_NOPULL);
    uint32_t raw = adc_read_raw(a->channel);
    uint32_t mv = (raw * vdda) / 4095U;
    if (a->mult != 1U)
    {
      out("%-20s raw=%4lu  pin=%4lu mV  -> %5lu mV\r\n", a->name, raw, mv, mv * a->mult);
    }
    else if (a->channel == ADC_CHANNEL_15)
    {
      /* ODrive v3.5 FET thermistor fit, x = V/3.3 */
      float x = (float)mv / 3300.0f;
      float t = ((363.93910201f * x - 462.15369634f) * x + 307.55129571f) * x - 27.72569531f;
      int32_t t10 = (int32_t)(t * 10.0f);
      out("%-20s raw=%4lu  pin=%4lu mV  (ODrive fit %ld.%ld C)\r\n", a->name, raw, mv,
          t10 / 10, (t10 < 0 ? -t10 : t10) % 10);
    }
    else
    {
      out("%-20s raw=%4lu  pin=%4lu mV\r\n", a->name, raw, mv);
    }
  }
}

static void cmd_pull(void)
{
  out("%-22s up dn  result\r\n", "pin");
  for (size_t i = 0; i < ARRAY_LEN(probe_pins); i++)
  {
    const Pin *p = &probe_pins[i];
    int up = read_with_pull(p, GPIO_PULLUP);
    int dn = read_with_pull(p, GPIO_PULLDOWN);
    const char *r = (up && !dn) ? "follows pull (floating / weak load)"
                  : (up && dn)  ? "HIGH (driven or strong pull-up)"
                  : (!up && !dn) ? "LOW (driven or strong pull-down)"
                  : "inverted?";
    out("%-22s  %d  %d  %s\r\n", p->name, up, dn, r);
    set_mode(p->port, p->pin, GPIO_MODE_INPUT, GPIO_NOPULL);
  }
  as5047_cs_restore();
}

static void cmd_watch(uint32_t seconds)
{
  uint8_t last[ARRAY_LEN(header_pins)];
  for (size_t i = 0; i < ARRAY_LEN(header_pins); i++)
  {
    set_mode(header_pins[i].port, header_pins[i].pin, GPIO_MODE_INPUT, GPIO_PULLDOWN);
  }
  HAL_Delay(5);
  for (size_t i = 0; i < ARRAY_LEN(header_pins); i++)
  {
    last[i] = (uint8_t)(HAL_GPIO_ReadPin(header_pins[i].port, header_pins[i].pin) == GPIO_PIN_SET);
  }
  out("watch %lu s: header pins pulled down, touch one with 3.3 V through 1 k. Any key stops.\r\n", seconds);
  uint32_t t0 = HAL_GetTick();
  while ((HAL_GetTick() - t0) < seconds * 1000U)
  {
    if (cdc_getc() >= 0)
    {
      break;
    }
    for (size_t i = 0; i < ARRAY_LEN(header_pins); i++)
    {
      uint8_t v = (uint8_t)(HAL_GPIO_ReadPin(header_pins[i].port, header_pins[i].pin) == GPIO_PIN_SET);
      if (v != last[i])
      {
        out("[%6lu ms] %-10s %d -> %d\r\n", HAL_GetTick() - t0, header_pins[i].name, last[i], v);
        last[i] = v;
      }
    }
    HAL_Delay(2);
  }
  for (size_t i = 0; i < ARRAY_LEN(header_pins); i++)
  {
    set_mode(header_pins[i].port, header_pins[i].pin, GPIO_MODE_INPUT, GPIO_NOPULL);
  }
  out("watch done\r\n");
}

static uint16_t as5047_cmd(uint16_t addr, int read)
{
  uint16_t w = (uint16_t)((addr & 0x3FFFU) | (read ? 0x4000U : 0U));
  uint16_t ones = 0;
  for (int b = 0; b < 15; b++)
  {
    ones += (w >> b) & 1U;
  }
  if (ones & 1U)
  {
    w |= 0x8000U;
  }
  return w;
}

static void cmd_enc(void)
{
  as5047_cs_restore();
  (void)spi_xfer(GPIOB, GPIO_PIN_3, as5047_cmd(0x0001, 1));                 /* ERRFL */
  uint16_t errfl = spi_xfer(GPIOB, GPIO_PIN_3, as5047_cmd(0x3FFC, 1));      /* -> ERRFL */
  uint16_t diag = spi_xfer(GPIOB, GPIO_PIN_3, as5047_cmd(0x3FFF, 1));       /* -> DIAAGC */
  out("AS5047P ERRFL=0x%04X  DIAAGC=0x%04X (MAGH=%d MAGL=%d COF=%d LF=%d AGC=%d)\r\n",
      errfl, diag, (diag >> 10) & 1, (diag >> 11) & 1, (diag >> 9) & 1, (diag >> 8) & 1, diag & 0xFF);
  for (int i = 0; i < 5; i++)
  {
    uint16_t a = spi_xfer(GPIOB, GPIO_PIN_3, as5047_cmd(0x3FFF, 1));         /* ANGLECOM */
    uint32_t deg10 = ((uint32_t)(a & 0x3FFFU) * 3600U) / 16384U;
    out("  angle raw=%5u  %lu.%lu deg  EF=%d\r\n", a & 0x3FFFU, deg10 / 10U, deg10 % 10U, (a >> 14) & 1);
    HAL_Delay(50);
  }
  if (diag == 0x0000U || diag == 0xFFFFU)
  {
    out("  no valid answer: encoder absent or SPI/CS wrong\r\n");
  }
}

static uint16_t drv_read(uint8_t reg)
{
  uint16_t w = (uint16_t)(0x8000U | ((uint16_t)(reg & 0x0FU) << 11));
  (void)spi_xfer(GPIOC, GPIO_PIN_13, w);
  return spi_xfer(GPIOC, GPIO_PIN_13, w);
}

static void cmd_drv(void)
{
  set_mode(GPIOA, GPIO_PIN_6, GPIO_MODE_ANALOG, GPIO_NOPULL);
  uint32_t vbus = (adc_read_raw(ADC_CHANNEL_6) * vdda_mv() / 4095U) * 11U;
  out("VBUS=%lu mV  nFAULT(PD2)=%d before\r\n", vbus, HAL_GPIO_ReadPin(GPIOD, GPIO_PIN_2) == GPIO_PIN_SET);
  if (vbus < 9000U)
  {
    out("refused: connect main power (12-24 V, current-limited) first; DRV8303 is unpowered on USB only\r\n");
    return;
  }
  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_12, GPIO_PIN_SET);   /* EN_GATE; all gate inputs stay low */
  HAL_Delay(20);
  for (uint8_t r = 0; r < 4; r++)
  {
    uint16_t v = drv_read(r);
    out("DRV8303 reg%u = 0x%04X (addr field %u, data 0x%03X, frame fault %d)\r\n",
        r, v, (v >> 11) & 0xF, v & 0x7FF, (v >> 15) & 1);
  }
  out("nFAULT(PD2)=%d with EN_GATE high\r\n", HAL_GPIO_ReadPin(GPIOD, GPIO_PIN_2) == GPIO_PIN_SET);
  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_12, GPIO_PIN_RESET);
  out("EN_GATE back low\r\n");
}

static void cmd_dfu(void)
{
  out("rebooting into DFU bootloader\r\n");
  HAL_Delay(50);
  *(volatile uint32_t *)BOOT_MAGIC_ADDR = BOOT_MAGIC_DFU_REQ;
  NVIC_SystemReset();
}

static void handle_line(char *line)
{
  if (strcmp(line, "info") == 0)        cmd_info();
  else if (strcmp(line, "adc") == 0)    cmd_adc();
  else if (strcmp(line, "pull") == 0)   cmd_pull();
  else if (strcmp(line, "watch") == 0)  cmd_watch(30);
  else if (strcmp(line, "enc") == 0)    cmd_enc();
  else if (strcmp(line, "drv") == 0)    cmd_drv();
  else if (strcmp(line, "dfu") == 0)    cmd_dfu();
  else if (line[0] != '\0')             out("commands: info adc pull watch enc drv dfu\r\n");
  out("> ");
}

int main(void)
{
  power_stage_safe();
  boot_reset_flags = RCC->CSR;
  RCC->CSR |= RCC_CSR_RMVF;

  HAL_Init();
  SystemClock_Config();
  adc_init();
  spi3_init();

  if (USBD_Init(&hUsbDeviceFS, &FS_Desc, DEVICE_FS) != USBD_OK ||
      USBD_RegisterClass(&hUsbDeviceFS, &USBD_CDC) != USBD_OK ||
      USBD_CDC_RegisterInterface(&hUsbDeviceFS, &USBD_Interface_fops_FS) != USBD_OK ||
      USBD_Start(&hUsbDeviceFS) != USBD_OK)
  {
    Error_Handler();
  }

  char line[32];
  size_t len = 0;
  while (1)
  {
    int c = cdc_getc();
    if (c < 0)
    {
      continue;
    }
    if (c == '\r' || c == '\n')
    {
      line[len] = '\0';
      handle_line(line);
      len = 0;
    }
    else if (len < sizeof(line) - 1U)
    {
      line[len++] = (char)c;
    }
  }
}

void Error_Handler(void)
{
  /* Keep the power stage off, then hang. */
  GPIOB->BSRR = (uint32_t)(GPIO_PIN_10 | GPIO_PIN_11 | GPIO_PIN_12) << 16;
  __disable_irq();
  while (1)
  {
  }
}
