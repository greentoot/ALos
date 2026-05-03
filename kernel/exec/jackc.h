#ifndef KERNEL_EXEC_JACKC_H
#define KERNEL_EXEC_JACKC_H

#include <stdint.h>

/* Compile one Jack source file to Hack VM code.
 * Returns 0 on success, <0 on error.
 */
int jackc_compile(const char *src_name,
                  const char *src_code,
                  char *out_vm,
                  uint32_t out_cap,
                  uint32_t *out_len,
                  char *errbuf,
                  uint32_t errcap);

#endif
