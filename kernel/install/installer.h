#ifndef KERNEL_INSTALL_INSTALLER_H
#define KERNEL_INSTALL_INSTALLER_H

#ifdef __cplusplus
extern "C" {
#endif

void installer_list_disks(void);
void installer_run_ui(void);
int  installer_payload_available(void);

#ifdef __cplusplus
}
#endif

#endif
