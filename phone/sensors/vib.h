#ifndef VIB_H
#define VIB_H
int vib_init(void);	/* probe + configure the PM8150B haptics (and its boost supply); 0 = ready */
int vib_set(int on);	/* start / stop a constant VMAX play */
int vib_status(void);	/* HAP_STATUS1 (bit 1 busy, bit 3 short circuit), -1 on error */
#endif
