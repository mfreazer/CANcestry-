/*
 * T2 target-build compatibility shim (H-08, issue #55, finding F-16).
 *
 * v1.0.0's platform/cortex_m/watchdog.c (read-only) defines the sim-only
 * static s_sim_rcc_csr only for host builds:
 *
 *   #if !defined(__arm__) && !defined(__thumb__)
 *   static uint32_t s_sim_rcc_csr = 0u;
 *   ...
 *   #endif
 *
 * but its test-only cancestry_watchdog_sim_tick() references s_sim_rcc_csr
 * WITHOUT a platform guard. The tag has never been compiled for the ARM
 * target (v1.0.0 CI builds the host simulated path), so the gap never
 * surfaced; the T2 off-tree ELF is the first target build.
 *
 * This header is force-included ONLY into watchdog.c for the target build
 * (build_firmware.sh) and supplies the missing sim static so the unchanged
 * source compiles. cancestry_watchdog_sim_tick is never called by the T2
 * driver and s_sim_rcc_csr is inert in the T2 scenario: the scripted
 * Renode IWDG sets RCC_CSR.IWDGRSTF itself (iwdg_model.py), and the
 * driver detects the post-reset boot through the platform-persisted
 * shadow (rcc_model.py), not through this sim static.
 */
#ifndef CANCESTRY_T2_TARGET_COMPAT_H
#define CANCESTRY_T2_TARGET_COMPAT_H

#include <stdint.h>

#if defined(__arm__) || defined(__thumb__)
static uint32_t s_sim_rcc_csr = 0u;
#endif

#endif /* CANCESTRY_T2_TARGET_COMPAT_H */
