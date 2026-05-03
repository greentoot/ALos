#ifndef KERNEL_JACK_JACK_DATA_H
#define KERNEL_JACK_JACK_DATA_H
#include <stdint.h>
/* Embedded VM files table in .rodata */
typedef struct {
    const char *name;
    const char *data;
    uint32_t    size;
} JackDataEntry;
extern const JackDataEntry jack_data_table[];
extern const int           jack_data_count;
/* Called at boot: load embedded VM files into vm_store */
void jack_data_mount(void);
#endif
