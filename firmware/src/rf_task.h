/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 OpenController contributors
 */
#ifndef RF_TASK_H
#define RF_TASK_H

#include <stdint.h>

#define RF_STATE_IDLE       0
#define RF_STATE_PAIRING    1
#define RF_STATE_CONNECTED  2

void RF_TaskInit(void);

uint8_t RF_Select2G4(void);
void RF_EnterPairing(void);
void RF_Disconnect(void);
void RF_FlushBondSave(void);
uint8_t RF_ClearBond(void);   /* 1 = the stored bond is provably gone */
uint8_t RF_HasBond(void);

void RF_QueueHIDReport(const uint8_t report[8]);

/* Poll from the main loop: advances the connected-mode listen channel on the
 * time-based hop (no-op unless connected). */
void RF_ConnectedTick(void);

uint8_t RF_GetState(void);
int8_t RF_GetRSSI(void);

#if KBD_RF_CRYPT
/* Attach a 16-byte link key to the current bond and activate encryption.
 * Returns 0 if there is no bond, or the key is all-zero / all-0xFF.
 *
 * BRING-UP SCAFFOLD, and deliberately not reachable from any shipped command
 * path -- see the definition. Both ends are provisioned the same key out of
 * band until the key-establishment handshake replaces this. */
uint8_t RF_ProvisionLinkKey(const uint8_t key[16]);

/* Link-encryption counters in .diag_safe, exposed so the bench UART dump can
 * read them out of band. Reading these over SWD is unsound on this part: an
 * attach resets a running application, and minichlink's CH5xx memory reads can
 * return all zeros for a valid address (firmware/README.md). Both produced
 * false conclusions in this investigation. */
extern volatile uint32_t kbd_crypt_seal_miss;
extern volatile uint32_t kbd_crypt_tx_sealed;
extern volatile uint32_t kbd_crypt_sess_bad;
extern volatile uint32_t kbd_crypt_sess_ok;
extern volatile uint32_t kbd_crypt_sess_rx;
#endif

#endif
