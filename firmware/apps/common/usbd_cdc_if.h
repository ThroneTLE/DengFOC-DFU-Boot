#ifndef __USBD_CDC_IF_H
#define __USBD_CDC_IF_H

#include "usbd_cdc.h"

extern USBD_CDC_ItfTypeDef USBD_Interface_fops_FS;

/* Blocking-with-timeout write; drops data if the host does not read. */
void cdc_write(const char *buf, uint32_t len);
/* Non-blocking: starts one transfer of up to CDC_TX_MAX bytes if the IN
 * endpoint is idle. Returns 1 if queued, 0 if busy or not connected. */
#define CDC_TX_MAX 512U
int cdc_try_write(const void *buf, uint32_t len);
/* Returns -1 when no byte is pending. */
int cdc_getc(void);

#endif /* __USBD_CDC_IF_H */
