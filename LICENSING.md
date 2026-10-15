# Licensing

GPL-3.0-only (`LICENSE`), as Felucca, SLOOP and Dinghy. Whoever gets a firmware built from this gets its source too.

| What                                                                                                                       | Who                                                                                                                                                                                                                                                                                                 |
| -------------------------------------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `platform/hal/`, `platform/loader/`, `platform/src/`, `platform/crt0.S`, `platform/app.ld`, `tools/fm1pkg_make.py`, `tools/lz4blk.py`, `web/fm1ota.js`, `web/fm1pkg.js`, the FM-1 parts of `tools/build.py` | Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments ([Felucca](https://github.com/hugelton/Felucca)), with the changes of [SLOOP](https://github.com/isod89/sloop-fm1) (isod89), as [Dinghy](https://github.com/zvenson/dinghy) (zvenson) and [X0X](https://github.com/charlesvestal/fm1-x0x) (Charles Vestal) carry them |
| `platform/fm1.c`                                                                                                            | Dinghy's `platform.c` (zvenson), rewired to `fm1.h`                                                                                                                                                                                                                                                 |
| `platform/fm1.h`, `platform/common.c`, `platform/web.c`, `web/` (the page, the emulator), `fm1`, `Dockerfile`, `src/main.c` | foma                                                                                                                                                                                                                                                                                                |

## Third-party material

| What                                                                                    | Licence     | Where                                                      |
| --------------------------------------------------------------------------------------- | ----------- | ---------------------------------------------------------- |
| Terminus font (ter-u16n), rendered into `platform/gen/felucca_font.h`                    | SIL OFL 1.1 | `platform/gen/Terminus-LICENSE.txt`                        |
| `platform/sdk/uboot.boot`, `cfg_tool.bin`, `eq_cfg_hw.bin` from JieLi's AC79NN_SDK_V1.2.1 | Apache-2.0  | <https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK>            |
| JieLi's toolchain, downloaded into the Docker image                                     | JieLi's     | <https://pkgman.jieliapp.com/s/linux-toolchain>            |

"M-VAVE" and "FM-1" belong to their owners; "Felucca" and "Hügelton Instruments" to Hügelton Instruments. This project is not made or endorsed by them.
