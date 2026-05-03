#ifndef DRIVER_AUDIO_H
#define DRIVER_AUDIO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void audio_init(void);
int  audio_is_ready(void);
int  audio_is_enabled(void);
int  audio_has_ac97_controller(void);
uint16_t audio_ac97_vendor_id(void);
uint16_t audio_ac97_device_id(void);
void audio_set_enabled(int enabled);
void audio_stop(void);
void audio_tone_on(uint32_t freq_hz);
void audio_beep(uint32_t freq_hz, uint32_t duration_ms);
void audio_play_boot_jingle(void);
void audio_play_test_pattern(void);

#ifdef __cplusplus
}
#endif

#endif
