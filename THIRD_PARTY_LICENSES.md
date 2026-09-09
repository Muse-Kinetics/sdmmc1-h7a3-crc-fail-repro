# Third-party licenses

This project is a STM32CubeMX-generated project. Most of the tree under
`Drivers/`, `Middlewares/`, and part of `Core/`/`FATFS/` is
STMicroelectronics-authored scaffold/driver code, not original to this
repo, and keeps its own upstream licensing rather than the top-level
[LICENSE](LICENSE) (MIT) that covers our own additions.

| Component | Path | License |
|---|---|---|
| STM32H7xx HAL/LL drivers | `Drivers/STM32H7xx_HAL_Driver/` | BSD-3-Clause — see `Drivers/STM32H7xx_HAL_Driver/LICENSE.txt` |
| CMSIS core | `Drivers/CMSIS/` | Apache-2.0 — see `Drivers/CMSIS/LICENSE.txt` |
| CMSIS device (STM32H7xx) | `Drivers/CMSIS/Device/ST/STM32H7xx/` | Apache-2.0 — see `Drivers/CMSIS/Device/ST/STM32H7xx/LICENSE.txt` |
| Nucleo BSP | `Drivers/BSP/STM32H7xx_Nucleo/` | BSD-3-Clause (same terms as the HAL driver package) |
| FatFs middleware | `Middlewares/Third_Party/FatFs/` | BSD-style, per ChaN's FatFs license (see file headers) |
| CubeMX-generated scaffold (`main.c`, `stm32h7xx_it.c`, `stm32h7xx_hal_msp.c`, `system_stm32h7xx.c`, linker scripts, startup assembly, FATFS glue) | `Core/`, `FATFS/`, `*.ld`, `startup_*.s` | Templated by CubeMX from ST's own templates; same BSD-3-Clause terms as the HAL package |

Each component directory carries its own `LICENSE.txt` (or, for FatFs,
license text in the file headers) per ST's own packaging convention —
those files are the authoritative terms and are kept in place unmodified.

Our own contributions — the diagnostic test harness added to `main.c`
(`SD_BlockReadTest`, `SD_ReadBlocks_Diag`, `SD_CardInfo_Dump`,
`SD_ReadBlocks_DMA_Test`, all inside `USER CODE` regions), the CMake
build wiring, and `README.md` — are licensed under [LICENSE](LICENSE)
(MIT).
