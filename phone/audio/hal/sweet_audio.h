/*
 * SweetAudio.driver <-> audiod (phone/audio): the speaker's samples, 48 kHz 16-bit signed little-endian stereo
 * (left, then right), written by the HAL plug-in into a FIFO that audiod creates and reads. Names as in
 * ios-topaz's redwood_audio.h (MIT), which the plug-in's code uses.
 */
#ifndef SWEET_AUDIO_H
#define SWEET_AUDIO_H

#define REDWOOD_AUDIO_DEVICE	"/var/local/audio.fifo"
#define REDWOOD_AUDIO_RATE	48000
#define REDWOOD_AUDIO_CHANNELS	2
#define REDWOOD_AUDIO_FRAME	4	/* bytes */
#define SWEET_AUDIO_LOG		"/var/local/sweetaudio.log"

#endif
