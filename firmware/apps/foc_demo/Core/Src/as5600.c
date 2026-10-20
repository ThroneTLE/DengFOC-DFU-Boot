#include "as5600.h"
#include "main.h"

#define AS5600_ADDR     0x36U
#define REG_CONF_H      0x07U
#define REG_STATUS      0x0BU
#define REG_RAW_ANGLE_H 0x0CU
#define REG_AGC         0x1AU
#define REG_MAGNITUDE_H 0x1BU

typedef struct
{
  const char *name;
  GPIO_TypeDef *port;
  uint16_t pin;
} HdrPin;

static const HdrPin hdr[] =
{
  {"Pin1(PA0)", GPIOA, GPIO_PIN_0},
  {"Pin2(PA1)", GPIOA, GPIO_PIN_1},
  {"Pin3(PA2)", GPIOA, GPIO_PIN_2},
  {"Pin4(PA3)", GPIOA, GPIO_PIN_3},
  {"Pin5(PC4)", GPIOC, GPIO_PIN_4},
  {"Pin6(PB2)", GPIOB, GPIO_PIN_2},
  {"Pin7(PA15)", GPIOA, GPIO_PIN_15},
};
#define HDR_COUNT (sizeof(hdr) / sizeof(hdr[0]))

static const HdrPin *scl;
static const HdrPin *sda;
static int found;
static int fast;              /* pointer-less RAW ANGLE reads verified */
static uint32_t half_cycles;  /* DWT cycles per half bit */
static uint32_t read_us;

static inline void lo(const HdrPin *p)  { p->port->BSRR = (uint32_t)p->pin << 16; }
static inline void rel(const HdrPin *p) { p->port->BSRR = p->pin; }
static inline int  rd(const HdrPin *p)  { return (p->port->IDR & p->pin) != 0U; }

static inline void dly(void)
{
  uint32_t t0 = DWT->CYCCNT;
  while ((DWT->CYCCNT - t0) < half_cycles)
  {
  }
}

/* Release a line and wait until it is really high (slow pull-ups, stretching). */
static int up(const HdrPin *p)
{
  rel(p);
  uint32_t t0 = DWT->CYCCNT;
  while (!rd(p))
  {
    if ((DWT->CYCCNT - t0) > (SystemCoreClock / 1000000U) * 200U)
    {
      return 0;
    }
  }
  return 1;
}

static void pin_od(const HdrPin *p)
{
  GPIO_InitTypeDef g = {0};
  rel(p);
  g.Pin = p->pin;
  g.Mode = GPIO_MODE_OUTPUT_OD;
  g.Pull = GPIO_PULLUP;
  g.Speed = GPIO_SPEED_FREQ_MEDIUM;
  HAL_GPIO_Init(p->port, &g);
}

static void pin_float(const HdrPin *p)
{
  GPIO_InitTypeDef g = {0};
  g.Pin = p->pin;
  g.Mode = GPIO_MODE_INPUT;
  g.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(p->port, &g);
}

static int start(void)
{
  if (!up(sda) || !up(scl))
  {
    return 0;
  }
  dly();
  lo(sda);
  dly();
  lo(scl);
  dly();
  return 1;
}

static int restart(void)
{
  rel(sda);
  dly();
  if (!up(scl))
  {
    return 0;
  }
  dly();
  lo(sda);
  dly();
  lo(scl);
  dly();
  return 1;
}

static void stop(void)
{
  lo(scl);
  dly();
  lo(sda);
  dly();
  (void)up(scl);
  dly();
  rel(sda);
  dly();
}

static int wbyte(uint8_t b)
{
  for (int i = 7; i >= 0; i--)
  {
    if (b & (1U << i))
    {
      rel(sda);
    }
    else
    {
      lo(sda);
    }
    dly();
    if (!up(scl))
    {
      return 0;
    }
    dly();
    lo(scl);
  }
  rel(sda);
  dly();
  if (!up(scl))
  {
    return 0;
  }
  dly();
  int ack = !rd(sda);
  lo(scl);
  dly();
  return ack;
}

static uint8_t rbyte(int ack)
{
  uint8_t b = 0;
  rel(sda);
  for (int i = 7; i >= 0; i--)
  {
    dly();
    (void)up(scl);
    dly();
    if (rd(sda))
    {
      b |= (uint8_t)(1U << i);
    }
    lo(scl);
  }
  if (ack)
  {
    lo(sda);
  }
  dly();
  (void)up(scl);
  dly();
  lo(scl);
  rel(sda);
  dly();
  return b;
}

/* Clock out a stuck slave, then issue STOP. */
static void recover(void)
{
  rel(sda);
  for (int i = 0; i < 9; i++)
  {
    lo(scl);
    dly();
    rel(scl);
    dly();
  }
  stop();
}

static int read_regs(uint8_t reg, uint8_t *buf, int n)
{
  int ok = start() && wbyte(AS5600_ADDR << 1) && wbyte(reg) && restart() &&
           wbyte((AS5600_ADDR << 1) | 1U);
  if (ok)
  {
    for (int i = 0; i < n; i++)
    {
      buf[i] = rbyte(i < n - 1);
    }
  }
  stop();
  return ok;
}

static int write_regs(uint8_t reg, const uint8_t *buf, int n)
{
  int ok = start() && wbyte(AS5600_ADDR << 1) && wbyte(reg);
  for (int i = 0; ok && i < n; i++)
  {
    ok = wbyte(buf[i]);
  }
  stop();
  return ok;
}

/* RAW ANGLE suppresses pointer auto-increment, so after the pointer is set
 * once a bare read returns the angle again (AS5600 datasheet). */
static int read_angle_fast(uint16_t *raw)
{
  uint8_t b[2];
  int ok = start() && wbyte((AS5600_ADDR << 1) | 1U);
  if (ok)
  {
    b[0] = rbyte(1);
    b[1] = rbyte(0);
  }
  stop();
  if (ok)
  {
    *raw = (uint16_t)(((b[0] & 0x0FU) << 8) | b[1]);
  }
  return ok;
}

static int read_angle_full(uint16_t *raw)
{
  uint8_t b[2];
  if (!read_regs(REG_RAW_ANGLE_H, b, 2))
  {
    return 0;
  }
  *raw = (uint16_t)(((b[0] & 0x0FU) << 8) | b[1]);
  return 1;
}

static int probe(void)
{
  recover();
  int ok = start() && wbyte(AS5600_ADDR << 1);
  stop();
  return ok;
}

/* Time how long a released line takes to read high, in DWT cycles. */
static uint32_t rise_cycles(const HdrPin *p)
{
  lo(p);
  uint32_t t = DWT->CYCCNT;
  while ((DWT->CYCCNT - t) < 168U * 5U)
  {
  }
  rel(p);
  uint32_t t0 = DWT->CYCCNT;
  while (!rd(p) && (DWT->CYCCNT - t0) < 168U * 100U)
  {
  }
  return DWT->CYCCNT - t0;
}

int as5600_detect(void)
{
  uint32_t mhz = SystemCoreClock / 1000000U;
  found = 0;
  fast = 0;
  half_cycles = mhz * 4U; /* slow and safe while scanning */

  for (unsigned i = 0; i < HDR_COUNT; i++)
  {
    pin_od(&hdr[i]);
  }
  HAL_Delay(5);

  for (unsigned i = 0; i < HDR_COUNT && !found; i++)
  {
    for (unsigned j = 0; j < HDR_COUNT && !found; j++)
    {
      if (i == j || !rd(&hdr[i]) || !rd(&hdr[j]))
      {
        continue;
      }
      scl = &hdr[i];
      sda = &hdr[j];
      if (probe())
      {
        found = 1;
      }
    }
  }

  for (unsigned i = 0; i < HDR_COUNT; i++)
  {
    if (!found || (&hdr[i] != scl && &hdr[i] != sda))
    {
      pin_float(&hdr[i]);
    }
  }
  if (!found)
  {
    return 0;
  }

  /* Pick the bit time from the measured rise time of both lines. */
  uint32_t rise = rise_cycles(sda);
  uint32_t rise_scl = rise_cycles(scl);
  if (rise_scl > rise)
  {
    rise = rise_scl;
  }
  half_cycles = rise * 2U + mhz;          /* rise x2 + 1 us margin */
  if (half_cycles < mhz * 3U / 2U)
  {
    half_cycles = mhz * 3U / 2U;          /* never faster than ~330 kHz */
  }

  /* Slow filter 2x, fast filter off: lower latency for motor control.
   * Volatile setting only; the chip is never burned. */
  uint8_t conf[2];
  if (read_regs(REG_CONF_H, conf, 2))
  {
    conf[0] = (uint8_t)((conf[0] & ~0x1FU) | 0x03U); /* SF = 11, FTH = 000 */
    (void)write_regs(REG_CONF_H, conf, 2);
  }

  /* Verify pointer-less reads against full reads. */
  uint16_t ref = 0;
  uint16_t v = 0;
  if (read_angle_full(&ref))
  {
    fast = 1;
    for (int k = 0; k < 8 && fast; k++)
    {
      if (!read_angle_fast(&v))
      {
        fast = 0;
      }
      else
      {
        int d = (int)v - (int)ref;
        if (d > 2048)
        {
          d -= 4096;
        }
        if (d < -2048)
        {
          d += 4096;
        }
        if (d > 40 || d < -40)
        {
          fast = 0;
        }
      }
    }
    if (!fast)
    {
      (void)read_angle_full(&ref); /* leaves the pointer on RAW ANGLE */
    }
  }

  uint32_t t0 = DWT->CYCCNT;
  (void)as5600_read_raw(&v);
  read_us = (DWT->CYCCNT - t0) / mhz;
  return 1;
}

int as5600_read_raw(uint16_t *raw)
{
  if (!found)
  {
    return 0;
  }
  return fast ? read_angle_fast(raw) : read_angle_full(raw);
}

int as5600_read_diag(uint8_t *status, uint8_t *agc, uint16_t *magnitude, uint16_t *conf)
{
  uint8_t b[2];
  if (!found)
  {
    return 0;
  }
  int ok = read_regs(REG_STATUS, status, 1) && read_regs(REG_AGC, agc, 1);
  if (ok && read_regs(REG_MAGNITUDE_H, b, 2))
  {
    *magnitude = (uint16_t)(((b[0] & 0x0FU) << 8) | b[1]);
  }
  if (ok && read_regs(REG_CONF_H, b, 2))
  {
    *conf = (uint16_t)((b[0] << 8) | b[1]);
  }
  /* Put the pointer back on RAW ANGLE for pointer-less reads. */
  uint16_t dummy;
  (void)read_angle_full(&dummy);
  return ok;
}

int as5600_found(void)            { return found; }
int as5600_fast_mode(void)        { return fast; }
uint32_t as5600_read_time_us(void) { return read_us; }
const char *as5600_scl_name(void) { return found ? scl->name : "-"; }
const char *as5600_sda_name(void) { return found ? sda->name : "-"; }
