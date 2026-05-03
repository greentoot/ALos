#ifndef KERNEL_NDS_NDS_BACKEND_H
#define KERNEL_NDS_NDS_BACKEND_H

#include <stdint.h>

typedef struct {
    char title[13];
    char game_code[5];
    char maker_code[3];
    uint32_t arm9_rom_offset;
    uint32_t arm9_entry;
    uint32_t arm9_ram_address;
    uint32_t arm9_size;
    uint32_t arm7_rom_offset;
    uint32_t arm7_entry;
    uint32_t arm7_ram_address;
    uint32_t arm7_size;
    uint8_t unit_code;
    uint8_t device_capacity;
} NdsRomInfo;

typedef struct {
    NdsRomInfo rom;
    uint32_t rom_size;
    uint32_t bios7_size;
    uint32_t bios9_size;
    uint32_t firmware_size;
    uint32_t keycfg_size;
    int rom_valid;
    int assets_ready;
} NdsBackendStatus;

typedef enum {
    NDS_BRIDGE_EVENT_NONE = 0,
    NDS_BRIDGE_EVENT_READY = 1,
    NDS_BRIDGE_EVENT_STARTED = 2,
    NDS_BRIDGE_EVENT_CLOSED = 3,
    NDS_BRIDGE_EVENT_ERROR = 4
} NdsBridgeEventKind;

typedef struct {
    NdsBridgeEventKind kind;
    uint32_t request_id;
    char detail[96];
} NdsBridgeEvent;

void nds_rom_info_init(NdsRomInfo *info);
int nds_rom_info_parse(const void *rom_data, uint32_t rom_size, NdsRomInfo *info);

void nds_backend_status_init(NdsBackendStatus *status);

int nds_backend_probe_assets(uint32_t *bios7_size,
                             uint32_t *bios9_size,
                             uint32_t *firmware_size,
                             uint32_t *keycfg_size);

int nds_backend_host_bridge_available(void);
int nds_backend_bridge_ready(void);
int nds_backend_internal_ready(void);
int nds_backend_can_launch(void);

void nds_bridge_event_init(NdsBridgeEvent *event);

int nds_backend_preflight(const char *rom_name,
                          const void *rom_data,
                          uint32_t rom_size,
                          NdsBackendStatus *status,
                          char *errbuf,
                          uint32_t errbuf_size);

int nds_backend_begin_session(const char *rom_name,
                              const void *rom_data,
                              uint32_t rom_size,
                              uint32_t *out_request_id,
                              char *errbuf,
                              uint32_t errbuf_size);

int nds_backend_poll_bridge_event(NdsBridgeEvent *event);

int nds_backend_wait_session(const char *rom_name,
                             const void *rom_data,
                             uint32_t rom_size,
                             char *errbuf,
                             uint32_t errbuf_size);

int nds_backend_run_probe(const char *rom_name,
                          const void *rom_data,
                          uint32_t rom_size,
                          uint32_t frames,
                          char *errbuf,
                          uint32_t errbuf_size);

#endif
