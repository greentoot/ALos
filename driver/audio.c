#include "audio.h"
#include "timer.h"

#define PIT_CH2_PORT       0x42
#define PIT_CMD_PORT       0x43
#define SPEAKER_CTRL_PORT  0x61
#define PIT_BASE_HZ        1193180u

static int g_audio_ready = 0;
static int g_audio_enabled = 1;
static int g_audio_ac97_present = 0;
static uint16_t g_audio_ac97_vendor = 0xFFFFu;
static uint16_t g_audio_ac97_device = 0xFFFFu;

static inline uint8_t inb(uint16_t port) {
    uint8_t v;
    __asm__ volatile ("inb %1,%0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline void outb(uint16_t port, uint8_t v) {
    __asm__ volatile ("outb %0,%1" :: "a"(v), "Nd"(port));
}

static inline uint32_t inl(uint16_t port) {
    uint32_t v;
    __asm__ volatile ("inl %1,%0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline void outl(uint16_t port, uint32_t v) {
    __asm__ volatile ("outl %0,%1" :: "a"(v), "Nd"(port));
}

static uint32_t pci_config_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t address =
        0x80000000u |
        ((uint32_t)bus << 16) |
        ((uint32_t)slot << 11) |
        ((uint32_t)func << 8) |
        (offset & 0xFCu);
    outl(0xCF8, address);
    return inl(0xCFC);
}

static void audio_detect_virtual_audio(void) {
    g_audio_ac97_present = 0;
    g_audio_ac97_vendor = 0xFFFFu;
    g_audio_ac97_device = 0xFFFFu;

    for (uint16_t bus = 0; bus < 256; ++bus) {
        for (uint8_t slot = 0; slot < 32; ++slot) {
            uint32_t vendor_device = pci_config_read32((uint8_t)bus, slot, 0, 0x00);
            uint16_t vendor0 = (uint16_t)(vendor_device & 0xFFFFu);
            if (vendor0 == 0xFFFFu) continue;

            uint32_t header = pci_config_read32((uint8_t)bus, slot, 0, 0x0C);
            uint8_t header_type = (uint8_t)((header >> 16) & 0xFFu);
            uint8_t fn_count = (header_type & 0x80u) ? 8u : 1u;

            for (uint8_t func = 0; func < fn_count; ++func) {
                vendor_device = pci_config_read32((uint8_t)bus, slot, func, 0x00);
                vendor0 = (uint16_t)(vendor_device & 0xFFFFu);
                if (vendor0 == 0xFFFFu) continue;

                uint16_t device = (uint16_t)((vendor_device >> 16) & 0xFFFFu);
                uint32_t class_reg = pci_config_read32((uint8_t)bus, slot, func, 0x08);
                uint8_t class_code = (uint8_t)((class_reg >> 24) & 0xFFu);
                uint8_t subclass = (uint8_t)((class_reg >> 16) & 0xFFu);

                if (class_code == 0x04u && subclass == 0x01u) {
                    g_audio_ac97_present = 1;
                    g_audio_ac97_vendor = vendor0;
                    g_audio_ac97_device = device;
                    return;
                }
            }
        }
    }
}

static void audio_sleep_ms(uint32_t duration_ms) {
    uint32_t start = timer_ms();
    while ((uint32_t)(timer_ms() - start) < duration_ms) {
        __asm__ volatile ("hlt");
    }
}

void audio_init(void) {
    audio_detect_virtual_audio();
    g_audio_ready = 1;
    g_audio_enabled = 1;
    audio_stop();
}

int audio_is_ready(void) {
    return g_audio_ready;
}

int audio_is_enabled(void) {
    return g_audio_ready && g_audio_enabled;
}

int audio_has_ac97_controller(void) {
    return g_audio_ac97_present;
}

uint16_t audio_ac97_vendor_id(void) {
    return g_audio_ac97_vendor;
}

uint16_t audio_ac97_device_id(void) {
    return g_audio_ac97_device;
}

void audio_set_enabled(int enabled) {
    g_audio_enabled = enabled ? 1 : 0;
    if (!g_audio_enabled) audio_stop();
}

void audio_stop(void) {
    uint8_t val = inb(SPEAKER_CTRL_PORT);
    outb(SPEAKER_CTRL_PORT, (uint8_t)(val & ~0x03u));
}

void audio_tone_on(uint32_t freq_hz) {
    uint32_t divisor;
    uint8_t val;
    if (!audio_is_enabled() || freq_hz == 0) return;

    divisor = PIT_BASE_HZ / freq_hz;
    if (divisor == 0) divisor = 1;
    if (divisor > 65535u) divisor = 65535u;

    outb(PIT_CMD_PORT, 0xB6);
    outb(PIT_CH2_PORT, (uint8_t)(divisor & 0xFFu));
    outb(PIT_CH2_PORT, (uint8_t)((divisor >> 8) & 0xFFu));

    val = inb(SPEAKER_CTRL_PORT);
    outb(SPEAKER_CTRL_PORT, (uint8_t)(val | 0x03u));
}

void audio_beep(uint32_t freq_hz, uint32_t duration_ms) {
    if (!audio_is_enabled()) return;
    audio_tone_on(freq_hz);
    audio_sleep_ms(duration_ms);
    audio_stop();
}

void audio_play_boot_jingle(void) {
    if (!audio_is_enabled()) return;
    audio_beep(659, 45);
    audio_sleep_ms(10);
    audio_beep(784, 45);
    audio_sleep_ms(10);
    audio_beep(988, 65);
}

void audio_play_test_pattern(void) {
    if (!audio_is_enabled()) return;
    audio_beep(440, 80);
    audio_sleep_ms(20);
    audio_beep(554, 80);
    audio_sleep_ms(20);
    audio_beep(659, 80);
    audio_sleep_ms(20);
    audio_beep(880, 120);
}
