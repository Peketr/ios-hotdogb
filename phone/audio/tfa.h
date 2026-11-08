/* The speaker amps (TFA9874 at 0x34 / 0x35): start from the container's profile, mute / unmute (AMPE). */
#ifndef AUDIO_TFA_H
#define AUDIO_TFA_H
#include <stdint.h>
int tfa_start(uint8_t addr, const char *profile);	/* 0 = clocks stable, muted */
void tfa_mute(uint8_t addr, int mute);
#endif
