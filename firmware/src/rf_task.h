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
void RF_ClearBond(void);
uint8_t RF_HasBond(void);

void RF_QueueHIDReport(const uint8_t report[8]);

/* Poll from the main loop: advances the connected-mode listen channel on the
 * time-based hop (no-op unless connected). */
void RF_ConnectedTick(void);

uint8_t RF_GetState(void);
int8_t RF_GetRSSI(void);

#endif
