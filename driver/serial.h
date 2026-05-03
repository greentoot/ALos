#ifndef DRIVER_SERIAL_H
#define DRIVER_SERIAL_H

#ifdef __cplusplus
extern "C" {
#endif

void serial_init(void);
int serial_is_ready(void);
int serial_char_available(void);
int serial_read_char(char *out);
void serial_write_char(char c);
void serial_write(const char *s);
void serial_write_line(const char *s);

#ifdef __cplusplus
}
#endif

#endif
