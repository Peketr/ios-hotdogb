#ifndef GNSS_H
#define GNSS_H
#include "link.h"
#define LOC_SERVICE	0x10	/* QMI LOC on the modem */
#define GNSS_PORT	0x21	/* our client port */
void gnss_new_server(uint32_t node, uint32_t port, uint32_t instance);
void gnss_poll(void);		/* from the idle hook: starts the session, writes the fix file */
void gnss_handle(const struct qrtr_hdr *h, const uint8_t *d, uint32_t n);
#endif
