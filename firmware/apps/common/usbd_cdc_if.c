#include <string.h>
#include "usbd_cdc_if.h"

extern USBD_HandleTypeDef hUsbDeviceFS;

static uint8_t rx_packet[CDC_DATA_FS_MAX_PACKET_SIZE];
static uint8_t tx_buf[CDC_TX_MAX];
static volatile uint8_t ring[512];
static volatile uint16_t ring_head;
static volatile uint16_t ring_tail;
static uint8_t line_coding[7] = {0x00, 0xC2, 0x01, 0x00, 0x00, 0x00, 0x08}; /* 115200 8N1 */

static int8_t CDC_Init_FS(void)
{
  USBD_CDC_SetTxBuffer(&hUsbDeviceFS, tx_buf, 0);
  USBD_CDC_SetRxBuffer(&hUsbDeviceFS, rx_packet);
  return USBD_OK;
}

static int8_t CDC_DeInit_FS(void)
{
  return USBD_OK;
}

static int8_t CDC_Control_FS(uint8_t cmd, uint8_t *pbuf, uint16_t length)
{
  if (cmd == CDC_SET_LINE_CODING && length >= sizeof(line_coding))
  {
    memcpy(line_coding, pbuf, sizeof(line_coding));
  }
  else if (cmd == CDC_GET_LINE_CODING && length >= sizeof(line_coding))
  {
    memcpy(pbuf, line_coding, sizeof(line_coding));
  }
  return USBD_OK;
}

static int8_t CDC_Receive_FS(uint8_t *Buf, uint32_t *Len)
{
  for (uint32_t i = 0; i < *Len; i++)
  {
    uint16_t next = (uint16_t)((ring_head + 1U) % sizeof(ring));
    if (next != ring_tail)
    {
      ring[ring_head] = Buf[i];
      ring_head = next;
    }
  }
  USBD_CDC_SetRxBuffer(&hUsbDeviceFS, rx_packet);
  USBD_CDC_ReceivePacket(&hUsbDeviceFS);
  return USBD_OK;
}

static int8_t CDC_TransmitCplt_FS(uint8_t *Buf, uint32_t *Len, uint8_t epnum)
{
  (void)Buf;
  (void)Len;
  (void)epnum;
  return USBD_OK;
}

USBD_CDC_ItfTypeDef USBD_Interface_fops_FS =
{
  CDC_Init_FS,
  CDC_DeInit_FS,
  CDC_Control_FS,
  CDC_Receive_FS,
  CDC_TransmitCplt_FS
};

static int tx_wait_idle(uint32_t timeout_ms)
{
  uint32_t t0 = HAL_GetTick();
  for (;;)
  {
    USBD_CDC_HandleTypeDef *hcdc = (USBD_CDC_HandleTypeDef *)hUsbDeviceFS.pClassData;
    if (hUsbDeviceFS.dev_state != USBD_STATE_CONFIGURED || hcdc == NULL)
    {
      return 0;
    }
    if (hcdc->TxState == 0U)
    {
      return 1;
    }
    if ((HAL_GetTick() - t0) > timeout_ms)
    {
      return 0;
    }
  }
}

void cdc_write(const char *buf, uint32_t len)
{
  while (len > 0U)
  {
    uint32_t n = (len > sizeof(tx_buf)) ? sizeof(tx_buf) : len;
    if (!tx_wait_idle(200U))
    {
      return;
    }
    memcpy(tx_buf, buf, n);
    USBD_CDC_SetTxBuffer(&hUsbDeviceFS, tx_buf, (uint32_t)n);
    if (USBD_CDC_TransmitPacket(&hUsbDeviceFS) != USBD_OK)
    {
      return;
    }
    buf += n;
    len -= n;
  }
  (void)tx_wait_idle(200U);
}

int cdc_try_write(const void *buf, uint32_t len)
{
  USBD_CDC_HandleTypeDef *hcdc = (USBD_CDC_HandleTypeDef *)hUsbDeviceFS.pClassData;
  if (hUsbDeviceFS.dev_state != USBD_STATE_CONFIGURED || hcdc == NULL ||
      hcdc->TxState != 0U || len == 0U || len > sizeof(tx_buf))
  {
    return 0;
  }
  memcpy(tx_buf, buf, len);
  USBD_CDC_SetTxBuffer(&hUsbDeviceFS, tx_buf, len);
  return USBD_CDC_TransmitPacket(&hUsbDeviceFS) == USBD_OK;
}

int cdc_getc(void)
{
  if (ring_tail == ring_head)
  {
    return -1;
  }
  int c = ring[ring_tail];
  ring_tail = (uint16_t)((ring_tail + 1U) % sizeof(ring));
  return c;
}
