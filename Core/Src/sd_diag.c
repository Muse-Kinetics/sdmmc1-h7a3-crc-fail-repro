/* Diagnostic instrumentation and the minimal FAT mount/list-root
 * reproduction for the SDMMC_ERROR_DATA_CRC_FAIL repro documented in
 * README.md / FINDINGS.md. Kept out of main.c so main.c stays close to
 * stock CubeMX output - see main.c's USER CODE blocks for the (short)
 * list of calls into here. */

#include "sd_diag.h"
#include "main.h"
#include "fatfs.h"
#include <stdio.h>
#include <string.h>

extern SD_HandleTypeDef hsd1;
extern UART_HandleTypeDef hcom_uart[];

/* Retargets printf to the ST-LINK VCP (USART3 / BSP COM1) so all of the
 * below is observable without a debugger attached. */
int _write(int file, char *ptr, int len)
{
	(void)file;
	HAL_UART_Transmit(&hcom_uart[COM1], (uint8_t *)ptr, (uint16_t)len, 1000);
	return len;
}

static uint32_t sdTestBuf[512 / 4];

static void SD_ReadBlocks_Diag(uint32_t *pData, uint32_t ReadAddr);
static uint8_t SD_ReadBlocks_Diag_Quiet(uint32_t *pData, uint32_t ReadAddr, uint32_t *outSTA, uint32_t *outDCOUNT);
static void PrintHexDump(const uint8_t *buf);
static uint8_t EnsureCardReady(void);
static const char *FresultName(FRESULT fr);

/* ------------------------------------------------------------------ */
/* Minimal reproduction                                                */
/* ------------------------------------------------------------------ */

void SD_Repro_MountAndListRoot(void)
{
	FRESULT fr;
	DIR dir;
	FILINFO fno;

	printf("\r\n[SD-REPRO] f_mount(\"%s\")...\r\n", SDPath);
	fr = f_mount(&SDFatFS, SDPath, 1); /* 1 = mount now, not lazily */
	printf("[SD-REPRO] f_mount() = %d (%s)\r\n", (int)fr, FresultName(fr));
	if (fr != FR_OK)
	{
		return;
	}

	printf("[SD-REPRO] f_opendir(\"/\")...\r\n");
	fr = f_opendir(&dir, "/");
	if (fr != FR_OK)
	{
		printf("[SD-REPRO] f_opendir() failed = %d (%s)\r\n", (int)fr, FresultName(fr));
		return;
	}

	printf("[SD-REPRO] Root directory listing:\r\n");
	for (;;)
	{
		fr = f_readdir(&dir, &fno);
		if (fr != FR_OK || fno.fname[0] == 0)
		{
			break;
		}
		printf("[SD-REPRO]   %s%s\r\n", fno.fname, (fno.fattrib & AM_DIR) ? "/" : "");
	}
	f_closedir(&dir);
	printf("[SD-REPRO] done\r\n");
}

/* ------------------------------------------------------------------ */
/* Diagnostic helpers - see FINDINGS.md for what each one showed       */
/* ------------------------------------------------------------------ */

void SD_Diag_DumpCardInfo(void)
{
	if (EnsureCardReady() != MSD_OK) { return; }

	HAL_SD_CardCIDTypeDef cid;
	HAL_SD_CardInfoTypeDef info;

	if (HAL_SD_GetCardCID(&hsd1, &cid) != HAL_OK)
	{
		printf("[SD-DIAG][CID] HAL_SD_GetCardCID failed\r\n");
	}
	else
	{
		char prodName[6] = {0};
		prodName[0] = (char)((cid.ProdName1 >> 24) & 0xFF);
		prodName[1] = (char)((cid.ProdName1 >> 16) & 0xFF);
		prodName[2] = (char)((cid.ProdName1 >> 8) & 0xFF);
		prodName[3] = (char)(cid.ProdName1 & 0xFF);
		prodName[4] = (char)cid.ProdName2;
		for (int i = 0; i < 5; i++) { if (prodName[i] < 0x20 || prodName[i] > 0x7E) prodName[i] = '.'; }
		printf("[SD-DIAG][CID] ManufacturerID=0x%02X OEM/AppID=0x%04X ProdName=\"%s\" Rev=0x%02X SN=0x%08lX MfgDate=0x%03X (month=%lu year=%lu)\r\n",
			   cid.ManufacturerID, cid.OEM_AppliID, prodName, cid.ProdRev, (unsigned long)cid.ProdSN,
			   cid.ManufactDate, (unsigned long)(cid.ManufactDate & 0xF), (unsigned long)(2000 + ((cid.ManufactDate >> 4) & 0xFF)));
	}

	if (HAL_SD_GetCardInfo(&hsd1, &info) != HAL_OK)
	{
		printf("[SD-DIAG][INFO] HAL_SD_GetCardInfo failed\r\n");
	}
	else
	{
		const char *typeStr = (info.CardType == CARD_SDSC) ? "SDSC" : (info.CardType == CARD_SDHC_SDXC) ? "SDHC/SDXC" : "OTHER";
		const char *speedStr = (info.CardSpeed == CARD_NORMAL_SPEED) ? "Normal(<=12.5MB/s)" : (info.CardSpeed == CARD_HIGH_SPEED) ? "High(<=25MB/s)" : (info.CardSpeed == CARD_ULTRA_HIGH_SPEED) ? "UHS-I" : "unknown";
		printf("[SD-DIAG][INFO] Type=%s Version=%s Class=%lu RCA=0x%04lX BlockNbr=%lu BlockSize=%lu LogBlockNbr=%lu LogBlockSize=%lu Speed=%s\r\n",
			   typeStr, (info.CardVersion == CARD_V2_X) ? "2.x" : "1.x", (unsigned long)info.Class,
			   (unsigned long)info.RelCardAdd, (unsigned long)info.BlockNbr, (unsigned long)info.BlockSize,
			   (unsigned long)info.LogBlockNbr, (unsigned long)info.LogBlockSize, speedStr);
	}
	printf("[SD-DIAG][CSD raw] %08lX %08lX %08lX %08lX\r\n",
		   (unsigned long)hsd1.CSD[0], (unsigned long)hsd1.CSD[1], (unsigned long)hsd1.CSD[2], (unsigned long)hsd1.CSD[3]);
}

/* Register-level single-block read: byte-for-byte re-implementation of
 * HAL_SD_ReadBlocks()'s polling loop, printing raw hsd1.Instance->STA
 * and DCOUNT at the exact moment the loop exits - BEFORE
 * __HAL_SD_CLEAR_FLAG() wipes the sticky STA bits (which stock
 * HAL_SD_ReadBlocks() does internally, hiding RXOVERR behind its
 * if/else-if DCRCFAIL-first priority). See FINDINGS.md, "The critical
 * failure point". */
void SD_Diag_RawBlockRead(uint32_t lba)
{
	if (EnsureCardReady() != MSD_OK) { return; }

	memset(sdTestBuf, 0xAA, sizeof(sdTestBuf)); /* poison so a failed/partial read is obvious */
	SD_ReadBlocks_Diag(sdTestBuf, lba);
	printf("[SD-DIAG] LBA %lu: hsd1.ErrorCode=0x%08lX, CardState=%lu\r\n",
		   (unsigned long)lba, (unsigned long)hsd1.ErrorCode, (unsigned long)HAL_SD_GetCardState(&hsd1));
	PrintHexDump((uint8_t *)sdTestBuf);
}

/* IDMA-mode read of the same block - discriminates whether the
 * corruption is specific to HAL_SD_ReadBlocks()'s CPU-polling FIFO
 * drain or lives further upstream. See FINDINGS.md. */
void SD_Diag_DmaReadTest(uint32_t lba)
{
	if (EnsureCardReady() != MSD_OK) { return; }

	memset(sdTestBuf, 0xAA, sizeof(sdTestBuf));
	uint8_t st = BSP_SD_ReadBlocks_DMA(sdTestBuf, lba, 1);
	if (st != MSD_OK)
	{
		printf("[SD-DIAG][DMA] BSP_SD_ReadBlocks_DMA() call itself FAILED (0x%02X), hsd1.ErrorCode=0x%08lX\r\n", st, (unsigned long)hsd1.ErrorCode);
		return;
	}

	uint32_t start = HAL_GetTick();
	while (hsd1.State != HAL_SD_STATE_READY)
	{
		if ((HAL_GetTick() - start) >= 5000U)
		{
			printf("[SD-DIAG][DMA] timed out waiting for completion, hsd1.State=%d\r\n", (int)hsd1.State);
			return;
		}
	}
	printf("[SD-DIAG][DMA] complete: hsd1.ErrorCode=0x%08lX, CardState=%lu\r\n",
		   (unsigned long)hsd1.ErrorCode, (unsigned long)HAL_SD_GetCardState(&hsd1));
	PrintHexDump((uint8_t *)sdTestBuf);
}

/* SDMMC1 RX delay-block (DLYB_SDMMC1) sweep - H7-specific, never
 * exercised by BSP_SD_Init()'s normal-speed path (stock HAL only
 * enables/configures it for UHS-mode tuning, see SD_HighSpeed() in
 * stm32h7xx_hal_sd.c). Sweeps the full range the silicon supports (12
 * phase selects x 128 delay units = 1536 points), each a full
 * single-block read at LBA 8192. See FINDINGS.md. */
void SD_Diag_DlybSweep(void)
{
	if (EnsureCardReady() != MSD_OK) { return; }

	printf("[SD-DIAG] --- SDMMC1 RX delay-block (DLYB_SDMMC1) sweep: %u phases x %u units ---\r\n",
		   (unsigned)DLYB_MAX_SELECT, (unsigned)DLYB_MAX_UNIT);

	uint32_t clkcrSaved = hsd1.Instance->CLKCR;
	/* Route RX sampling through the feedback clock / delay block path -
	 * mirrors exactly what stock HAL_SD does for SDR104 tuning. */
	MODIFY_REG(hsd1.Instance->CLKCR, SDMMC_CLKCR_SELCLKRX, SDMMC_CLKCR_SELCLKRX_1);

	uint32_t successes = 0;
	uint32_t totalPoints = 0;
	for (uint32_t phase = 0; phase < DLYB_MAX_SELECT; phase++)
	{
		for (uint32_t unit = 0; unit < DLYB_MAX_UNIT; unit++)
		{
			DelayBlock_Configure(DLYB_SDMMC1, phase, unit);
			memset(sdTestBuf, 0xAA, sizeof(sdTestBuf));
			uint32_t sta = 0, dcount = 0;
			uint8_t ok = SD_ReadBlocks_Diag_Quiet(sdTestBuf, 8192, &sta, &dcount);
			totalPoints++;
			if (ok)
			{
				successes++;
				printf("[SD-DIAG][DLYB] phase=%lu unit=%lu: CLEAN READ (STA=0x%08lX DCOUNT=%lu)\r\n",
					   (unsigned long)phase, (unsigned long)unit, (unsigned long)sta, (unsigned long)dcount);
			}
		}
		printf("[SD-DIAG][DLYB] phase=%lu/%u sweep done, %lu clean so far\r\n",
			   (unsigned long)phase, (unsigned)(DLYB_MAX_SELECT - 1), (unsigned long)successes);
	}
	printf("[SD-DIAG] --- DLYB sweep complete: %lu/%lu points clean ---\r\n",
		   (unsigned long)successes, (unsigned long)totalPoints);

	/* Restore: leave the peripheral bypassed, matching every other
	 * result in this repro. */
	DelayBlock_Disable(DLYB_SDMMC1);
	hsd1.Instance->CLKCR = clkcrSaved;
}

/* ------------------------------------------------------------------ */
/* Private helpers                                                     */
/* ------------------------------------------------------------------ */

static uint8_t EnsureCardReady(void)
{
	if (hsd1.State == HAL_SD_STATE_READY)
	{
		return MSD_OK;
	}
	uint8_t st = BSP_SD_Init();
	if (st != MSD_OK)
	{
		printf("[SD-DIAG] BSP_SD_Init() failed (0x%02X)\r\n", st);
	}
	return st;
}

static const char *FresultName(FRESULT fr)
{
	static const char *names[] = {
		"FR_OK", "FR_DISK_ERR", "FR_INT_ERR", "FR_NOT_READY", "FR_NO_FILE",
		"FR_NO_PATH", "FR_INVALID_NAME", "FR_DENIED", "FR_EXIST",
		"FR_INVALID_OBJECT", "FR_WRITE_PROTECTED", "FR_INVALID_DRIVE",
		"FR_NOT_ENABLED", "FR_NO_FILESYSTEM", "FR_MKFS_ABORTED", "FR_TIMEOUT",
		"FR_LOCKED", "FR_NOT_ENOUGH_CORE", "FR_TOO_MANY_OPEN_FILES",
		"FR_INVALID_PARAMETER",
	};
	if ((unsigned)fr < (sizeof(names) / sizeof(names[0])))
	{
		return names[fr];
	}
	return "?";
}

static void PrintHexDump(const uint8_t *buf)
{
	for (uint32_t row = 0; row < 512 / 16; row++)
	{
		uint32_t base = row * 16;
		printf("[SD-DIAG] [%3lu..%3lu]: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X\r\n",
			   (unsigned long)base, (unsigned long)(base + 15),
			   buf[base+0], buf[base+1], buf[base+2], buf[base+3],
			   buf[base+4], buf[base+5], buf[base+6], buf[base+7],
			   buf[base+8], buf[base+9], buf[base+10], buf[base+11],
			   buf[base+12], buf[base+13], buf[base+14], buf[base+15]);
		HAL_Delay(15); /* don't outrun the UART */
	}
}

static void SD_ReadBlocks_Diag(uint32_t *pData, uint32_t ReadAddr)
{
	SDMMC_DataInitTypeDef config;
	uint32_t errorstate;
	uint32_t tickstart = HAL_GetTick();
	uint32_t count;
	uint32_t data;
	uint32_t dataremaining;
	uint32_t add = ReadAddr;
	uint8_t *tempbuff = (uint8_t *)pData;
	uint32_t fifoReads = 0;

	if (hsd1.State != HAL_SD_STATE_READY)
	{
		printf("[SD-DIAG] hsd1.State not READY (%d), aborting\r\n", (int)hsd1.State);
		return;
	}

	hsd1.ErrorCode = HAL_SD_ERROR_NONE;
	hsd1.State = HAL_SD_STATE_BUSY;
	hsd1.Instance->DCTRL = 0U;

	if (hsd1.SdCard.CardType != CARD_SDHC_SDXC)
	{
		add *= BLOCKSIZE;
	}

	config.DataTimeOut   = SDMMC_DATATIMEOUT;
	config.DataLength    = BLOCKSIZE;
	config.DataBlockSize = SDMMC_DATABLOCK_SIZE_512B;
	config.TransferDir   = SDMMC_TRANSFER_DIR_TO_SDMMC;
	config.TransferMode  = SDMMC_TRANSFER_MODE_BLOCK;
	config.DPSM          = SDMMC_DPSM_DISABLE;
	(void)SDMMC_ConfigData(hsd1.Instance, &config);
	__SDMMC_CMDTRANS_ENABLE(hsd1.Instance);

	hsd1.Context = SD_CONTEXT_READ_SINGLE_BLOCK;
	errorstate = SDMMC_CmdReadSingleBlock(hsd1.Instance, add);
	if (errorstate != HAL_SD_ERROR_NONE)
	{
		printf("[SD-DIAG] SDMMC_CmdReadSingleBlock errorstate=0x%08lX\r\n", (unsigned long)errorstate);
		__HAL_SD_CLEAR_FLAG(&hsd1, SDMMC_STATIC_FLAGS);
		hsd1.State = HAL_SD_STATE_READY;
		return;
	}

	dataremaining = config.DataLength;
	while (!__HAL_SD_GET_FLAG(&hsd1, SDMMC_FLAG_RXOVERR | SDMMC_FLAG_DCRCFAIL | SDMMC_FLAG_DTIMEOUT | SDMMC_FLAG_DATAEND))
	{
		if (__HAL_SD_GET_FLAG(&hsd1, SDMMC_FLAG_RXFIFOHF) && (dataremaining >= SDMMC_FIFO_SIZE))
		{
			for (count = 0U; count < (SDMMC_FIFO_SIZE / 4U); count++)
			{
				data = SDMMC_ReadFIFO(hsd1.Instance);
				*tempbuff = (uint8_t)(data & 0xFFU); tempbuff++;
				*tempbuff = (uint8_t)((data >> 8U) & 0xFFU); tempbuff++;
				*tempbuff = (uint8_t)((data >> 16U) & 0xFFU); tempbuff++;
				*tempbuff = (uint8_t)((data >> 24U) & 0xFFU); tempbuff++;
			}
			dataremaining -= SDMMC_FIFO_SIZE;
			fifoReads++;
		}

		if ((HAL_GetTick() - tickstart) >= 5000U)
		{
			printf("[SD-DIAG] software timeout, STA=0x%08lX DCOUNT=%lu fifoReads=%lu\r\n",
				   (unsigned long)hsd1.Instance->STA, (unsigned long)hsd1.Instance->DCOUNT, (unsigned long)fifoReads);
			__HAL_SD_CLEAR_FLAG(&hsd1, SDMMC_STATIC_FLAGS);
			hsd1.State = HAL_SD_STATE_READY;
			hsd1.Context = SD_CONTEXT_NONE;
			return;
		}
	}
	__SDMMC_CMDTRANS_DISABLE(hsd1.Instance);

	/* *** The diagnostic moment: raw silicon state, nothing cleared yet *** */
	printf("[SD-DIAG] loop exit: STA=0x%08lX DCOUNT=%lu dataremaining=%lu fifoReads=%lu (DCRCFAIL=%d RXOVERR=%d DTIMEOUT=%d DATAEND=%d)\r\n",
		   (unsigned long)hsd1.Instance->STA, (unsigned long)hsd1.Instance->DCOUNT,
		   (unsigned long)dataremaining, (unsigned long)fifoReads,
		   __HAL_SD_GET_FLAG(&hsd1, SDMMC_FLAG_DCRCFAIL) ? 1 : 0,
		   __HAL_SD_GET_FLAG(&hsd1, SDMMC_FLAG_RXOVERR) ? 1 : 0,
		   __HAL_SD_GET_FLAG(&hsd1, SDMMC_FLAG_DTIMEOUT) ? 1 : 0,
		   __HAL_SD_GET_FLAG(&hsd1, SDMMC_FLAG_DATAEND) ? 1 : 0);

	if (__HAL_SD_GET_FLAG(&hsd1, SDMMC_FLAG_DCRCFAIL)) { hsd1.ErrorCode |= HAL_SD_ERROR_DATA_CRC_FAIL; }
	if (__HAL_SD_GET_FLAG(&hsd1, SDMMC_FLAG_RXOVERR)) { hsd1.ErrorCode |= HAL_SD_ERROR_RX_OVERRUN; }
	if (__HAL_SD_GET_FLAG(&hsd1, SDMMC_FLAG_DTIMEOUT)) { hsd1.ErrorCode |= HAL_SD_ERROR_DATA_TIMEOUT; }
	__HAL_SD_CLEAR_FLAG(&hsd1, SDMMC_STATIC_FLAGS);
	hsd1.State = HAL_SD_STATE_READY;
	hsd1.Context = SD_CONTEXT_NONE;
}

/* Same polling read as SD_ReadBlocks_Diag(), stripped of per-call printf
 * so it's cheap to run hundreds of times in SD_Diag_DlybSweep(). Returns
 * 1 iff the transfer completed with DATAEND and none of
 * DCRCFAIL/RXOVERR/DTIMEOUT. */
static uint8_t SD_ReadBlocks_Diag_Quiet(uint32_t *pData, uint32_t ReadAddr, uint32_t *outSTA, uint32_t *outDCOUNT)
{
	SDMMC_DataInitTypeDef config;
	uint32_t errorstate;
	uint32_t tickstart = HAL_GetTick();
	uint32_t count;
	uint32_t data;
	uint32_t dataremaining;
	uint32_t add = ReadAddr;
	uint8_t *tempbuff = (uint8_t *)pData;

	if (hsd1.State != HAL_SD_STATE_READY)
	{
		return 0;
	}

	hsd1.ErrorCode = HAL_SD_ERROR_NONE;
	hsd1.State = HAL_SD_STATE_BUSY;
	hsd1.Instance->DCTRL = 0U;

	if (hsd1.SdCard.CardType != CARD_SDHC_SDXC)
	{
		add *= BLOCKSIZE;
	}

	config.DataTimeOut   = SDMMC_DATATIMEOUT;
	config.DataLength    = BLOCKSIZE;
	config.DataBlockSize = SDMMC_DATABLOCK_SIZE_512B;
	config.TransferDir   = SDMMC_TRANSFER_DIR_TO_SDMMC;
	config.TransferMode  = SDMMC_TRANSFER_MODE_BLOCK;
	config.DPSM          = SDMMC_DPSM_DISABLE;
	(void)SDMMC_ConfigData(hsd1.Instance, &config);
	__SDMMC_CMDTRANS_ENABLE(hsd1.Instance);

	hsd1.Context = SD_CONTEXT_READ_SINGLE_BLOCK;
	errorstate = SDMMC_CmdReadSingleBlock(hsd1.Instance, add);
	if (errorstate != HAL_SD_ERROR_NONE)
	{
		__HAL_SD_CLEAR_FLAG(&hsd1, SDMMC_STATIC_FLAGS);
		hsd1.State = HAL_SD_STATE_READY;
		hsd1.Context = SD_CONTEXT_NONE;
		return 0;
	}

	dataremaining = config.DataLength;
	while (!__HAL_SD_GET_FLAG(&hsd1, SDMMC_FLAG_RXOVERR | SDMMC_FLAG_DCRCFAIL | SDMMC_FLAG_DTIMEOUT | SDMMC_FLAG_DATAEND))
	{
		if (__HAL_SD_GET_FLAG(&hsd1, SDMMC_FLAG_RXFIFOHF) && (dataremaining >= SDMMC_FIFO_SIZE))
		{
			for (count = 0U; count < (SDMMC_FIFO_SIZE / 4U); count++)
			{
				data = SDMMC_ReadFIFO(hsd1.Instance);
				*tempbuff = (uint8_t)(data & 0xFFU); tempbuff++;
				*tempbuff = (uint8_t)((data >> 8U) & 0xFFU); tempbuff++;
				*tempbuff = (uint8_t)((data >> 16U) & 0xFFU); tempbuff++;
				*tempbuff = (uint8_t)((data >> 24U) & 0xFFU); tempbuff++;
			}
			dataremaining -= SDMMC_FIFO_SIZE;
		}

		if ((HAL_GetTick() - tickstart) >= 1000U)
		{
			__HAL_SD_CLEAR_FLAG(&hsd1, SDMMC_STATIC_FLAGS);
			hsd1.State = HAL_SD_STATE_READY;
			hsd1.Context = SD_CONTEXT_NONE;
			if (outSTA) { *outSTA = hsd1.Instance->STA; }
			if (outDCOUNT) { *outDCOUNT = hsd1.Instance->DCOUNT; }
			return 0;
		}
	}
	__SDMMC_CMDTRANS_DISABLE(hsd1.Instance);

	uint32_t sta = hsd1.Instance->STA;
	uint32_t dcount = hsd1.Instance->DCOUNT;
	uint8_t ok = (uint8_t)(__HAL_SD_GET_FLAG(&hsd1, SDMMC_FLAG_DATAEND) &&
						   !__HAL_SD_GET_FLAG(&hsd1, SDMMC_FLAG_DCRCFAIL) &&
						   !__HAL_SD_GET_FLAG(&hsd1, SDMMC_FLAG_RXOVERR) &&
						   !__HAL_SD_GET_FLAG(&hsd1, SDMMC_FLAG_DTIMEOUT));

	__HAL_SD_CLEAR_FLAG(&hsd1, SDMMC_STATIC_FLAGS);
	hsd1.State = HAL_SD_STATE_READY;
	hsd1.Context = SD_CONTEXT_NONE;
	if (outSTA) { *outSTA = sta; }
	if (outDCOUNT) { *outDCOUNT = dcount; }
	return ok;
}
