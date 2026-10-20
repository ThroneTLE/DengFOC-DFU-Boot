#include "drv8303.h"
#include "main.h"

/* SPI3: PC10 SCK, PC11 MISO, PC12 MOSI; nSCS = PC13. Mode 1, 16-bit. */
static SPI_HandleTypeDef hspi3;

#define CTRL1_GATE_CURRENT_1A7  (0U << 0)
#define CTRL1_GATE_RESET        (1U << 2)
#define CTRL1_PWM_6INPUT        (0U << 3)
#define CTRL1_OCP_LATCH         (1U << 4)
#define CTRL1_OC_ADJ(code)      ((uint16_t)(code) << 6)
#define CTRL2_GAIN_80           (3U << 2)

void drv_spi_init(void)
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
  hspi3.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_32;
  hspi3.Init.FirstBit = SPI_FIRSTBIT_MSB;
  hspi3.Init.TIMode = SPI_TIMODE_DISABLE;
  hspi3.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
  hspi3.Init.CRCPolynomial = 10;
  if (HAL_SPI_Init(&hspi3) != HAL_OK)
  {
    Error_Handler();
  }
}

static uint16_t xfer(uint16_t w)
{
  uint16_t r = 0;
  HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, GPIO_PIN_RESET);
  (void)HAL_SPI_TransmitReceive(&hspi3, (uint8_t *)&w, (uint8_t *)&r, 1, 10);
  HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, GPIO_PIN_SET);
  for (volatile int i = 0; i < 100; i++)
  {
  }
  return r;
}

/* The answer to a frame arrives in the following frame. */
int drv_read(uint8_t reg, uint16_t *data)
{
  uint16_t cmd = (uint16_t)(0x8000U | ((uint16_t)(reg & 0x0FU) << 11));
  (void)xfer(cmd);
  uint16_t r = xfer(cmd);
  *data = r & 0x07FFU;
  /* bit 15 = frame fault, bits 14:11 echo the address */
  return ((r & 0x8000U) == 0U) && (((r >> 11) & 0x0FU) == (reg & 0x0FU));
}

int drv_write(uint8_t reg, uint16_t data)
{
  (void)xfer((uint16_t)(((uint16_t)(reg & 0x0FU) << 11) | (data & 0x07FFU)));
  return 1;
}

int drv_configure(void)
{
  const uint16_t ctrl1 = CTRL1_GATE_CURRENT_1A7 | CTRL1_PWM_6INPUT | CTRL1_OCP_LATCH | CTRL1_OC_ADJ(0);
  const uint16_t ctrl2 = CTRL2_GAIN_80;
  uint16_t v1 = 0;
  uint16_t v2 = 0;

  (void)drv_write(0x02, ctrl1 | CTRL1_GATE_RESET); /* clear latched faults */
  (void)drv_write(0x02, ctrl1);
  (void)drv_write(0x03, ctrl2);
  if (!drv_read(0x02, &v1) || !drv_read(0x03, &v2))
  {
    return 0;
  }
  return (v1 == ctrl1) && (v2 == ctrl2);
}
