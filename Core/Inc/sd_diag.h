#ifndef SD_DIAG_H
#define SD_DIAG_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* The minimal reproduction: mounts the FAT volume on the SD card and
 * lists the root directory over the ST-LINK VCP (115200 8N1). This is
 * the "should just work" baseline that fails - see README.md. */
void SD_Repro_MountAndListRoot(void);

/* Diagnostic/profiling helpers used while investigating the failure
 * above. Not called by default - see the commented-out calls in
 * main.c's USER CODE WHILE block. Full context and results for each one
 * are in FINDINGS.md. */
void SD_Diag_DumpCardInfo(void);
void SD_Diag_RawBlockRead(uint32_t lba);
void SD_Diag_DmaReadTest(uint32_t lba);
void SD_Diag_DlybSweep(void);

#ifdef __cplusplus
}
#endif

#endif /* SD_DIAG_H */
