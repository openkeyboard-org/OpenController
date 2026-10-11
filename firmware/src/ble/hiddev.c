/********************************** (C) COPYRIGHT *******************************
 * File Name          : hiddev.c
 * Author             : WCH
 * Version            : V1.0
 * Date               : 2018/12/10
 * Description        : HID device task (HID over GATT glue)
 *********************************************************************************
 * Copyright (c) 2021 Nanjing Qinheng Microelectronics Co., Ltd.
 * Attention: This software (modified or not) and binary are used for
 * microcontroller manufactured by Nanjing Qinheng Microelectronics.
 *******************************************************************************/

/* Modified for OpenController (Copyright 2026 Eric Molitor (EMulator),
 * Apache-2.0): taken from the HID_Keyboard example of openwch/ch592
 * (EVT/EXAM/BLE/HID_Keyboard/Profile). The advertising policy, the battery
 * measurement task and the Scan Parameters service are removed - the
 * application (ble_hid.c) owns advertising and is told the battery level by
 * the keyboard - and a report is sent only on an encrypted link, never
 * starting advertising on its own. The pairing state is passed on to the
 * application, and HidDev_ReportReady() lets it hold reports until the host
 * has enabled notifications. */

#include "CONFIG.h"
#include "battservice.h"
#include "devinfoservice.h"
#include "hiddev.h"

#define START_DEVICE_EVT 0x0001

uint8_t hidDevTaskId;

static gapRole_States_t hidDevGapState = GAPROLE_INIT;
static uint8_t          hidDevConnSecure = FALSE;
static uint16_t         gapConnHandle = GAP_CONNHANDLE_INIT;

static hidRptMap_t *pHidDevRptTbl;
static uint8_t      hidDevRptTblLen;
static hidDevCB_t  *pHidDevCB;
static hidDevCfg_t *pHidDevCfg;

static void hidDevDisconnected(void);
static void hidDevGapStateCB(gapRole_States_t newState, gapRoleEvent_t *pEvent);
static void hidDevParamUpdateCB(uint16_t connHandle, uint16_t connInterval,
                                uint16_t connSlaveLatency, uint16_t connTimeout);
static void hidDevPairStateCB(uint16_t connHandle, uint8_t state, uint8_t status);
static void hidDevPasscodeCB(uint8_t *deviceAddr, uint16_t connectionHandle,
                             uint8_t uiInputs, uint8_t uiOutputs);
static hidRptMap_t *hidDevRptByHandle(uint16_t handle);
static hidRptMap_t *hidDevRptById(uint8_t id, uint8_t type);
static hidRptMap_t *hidDevRptByCccdHandle(uint16_t handle);
static uint8_t      hidDevSendNoti(uint16_t handle, uint8_t len, uint8_t *pData);

static gapRolesCBs_t hidDev_PeripheralCBs = {
    hidDevGapStateCB,
    NULL,
    hidDevParamUpdateCB,
};

static gapBondCBs_t hidDevBondCB = {
    hidDevPasscodeCB,
    hidDevPairStateCB,
    NULL,
};

void HidDev_Init(void)
{
    hidDevTaskId = TMOS_ProcessEventRegister(HidDev_ProcessEvent);

    {
        uint8_t syncWL = TRUE;
        GAPBondMgr_SetParameter(GAPBOND_AUTO_SYNC_WL, sizeof(uint8_t), &syncWL);
    }

    GGS_AddService(GATT_ALL_SERVICES);
    GATTServApp_AddService(GATT_ALL_SERVICES);
    DevInfo_AddService();
    Batt_AddService();

    tmos_set_event(hidDevTaskId, START_DEVICE_EVT);
}

uint16_t HidDev_ProcessEvent(uint8_t task_id, uint16_t events)
{
    (void)task_id;

    if (events & SYS_EVENT_MSG) {
        uint8_t *pMsg;

        if ((pMsg = tmos_msg_receive(hidDevTaskId)) != NULL) {
            tmos_msg_deallocate(pMsg);
        }
        return (events ^ SYS_EVENT_MSG);
    }

    if (events & START_DEVICE_EVT) {
        GAPRole_PeripheralStartDevice(hidDevTaskId, &hidDevBondCB, &hidDev_PeripheralCBs);
        return (events ^ START_DEVICE_EVT);
    }

    return 0;
}

void HidDev_Register(hidDevCfg_t *pCfg, hidDevCB_t *pCBs)
{
    pHidDevCB = pCBs;
    pHidDevCfg = pCfg;
}

void HidDev_RegisterReports(uint8_t numReports, hidRptMap_t *pRpt)
{
    pHidDevRptTbl = pRpt;
    hidDevRptTblLen = numReports;
}

uint8_t HidDev_ReportReady(uint8_t id, uint8_t type)
{
    hidRptMap_t     *pRpt;
    gattAttribute_t *pAttr;
    uint16_t         retHandle;

    if (hidDevGapState != GAPROLE_CONNECTED || !hidDevConnSecure) {
        return FALSE;
    }
    if ((pRpt = hidDevRptById(id, type)) == NULL || pRpt->cccdHandle == 0) {
        return FALSE;
    }
    if ((pAttr = GATT_FindHandle(pRpt->cccdHandle, &retHandle)) == NULL) {
        return FALSE;
    }
    return (GATTServApp_ReadCharCfg(gapConnHandle, (gattCharCfg_t *)pAttr->pValue)
            & GATT_CLIENT_CFG_NOTIFY) != 0;
}

uint8_t HidDev_Report(uint8_t id, uint8_t type, uint8_t len, uint8_t *pData)
{
    hidRptMap_t *pRpt;

    if (!HidDev_ReportReady(id, type)) {
        return bleNotReady;
    }
    if ((pRpt = hidDevRptById(id, type)) == NULL) {
        return bleNoResources;
    }
    return hidDevSendNoti(pRpt->handle, len, pData);
}

uint8_t HidDev_Connected(void)
{
    return hidDevGapState == GAPROLE_CONNECTED;
}

uint8_t HidDev_Secure(void)
{
    return hidDevGapState == GAPROLE_CONNECTED && hidDevConnSecure;
}

uint16_t HidDev_ConnHandle(void)
{
    return gapConnHandle;
}

void HidDev_Close(void)
{
    uint8_t param;

    if (hidDevGapState == GAPROLE_CONNECTED) {
        GAPRole_TerminateLink(gapConnHandle);
    } else {
        param = FALSE;
        GAPRole_SetParameter(GAPROLE_ADVERT_ENABLED, sizeof(uint8_t), &param);
    }
}

bStatus_t HidDev_ReadAttrCB(uint16_t connHandle, gattAttribute_t *pAttr,
                            uint8_t *pValue, uint16_t *pLen, uint16_t offset, uint16_t maxLen, uint8_t method)
{
    bStatus_t    status = SUCCESS;
    hidRptMap_t *pRpt;
    uint16_t     uuid = BUILD_UINT16(pAttr->type.uuid[0], pAttr->type.uuid[1]);

    (void)connHandle;
    (void)method;

    /* Only the report map is long enough to need a blob read. */
    if (offset > 0 && uuid != REPORT_MAP_UUID) {
        return (ATT_ERR_ATTR_NOT_LONG);
    }

    if (uuid == REPORT_UUID || uuid == BOOT_KEY_INPUT_UUID ||
        uuid == BOOT_KEY_OUTPUT_UUID || uuid == BOOT_MOUSE_INPUT_UUID) {
        if ((pRpt = hidDevRptByHandle(pAttr->handle)) != NULL) {
            status = (*pHidDevCB->reportCB)(pRpt->id, pRpt->type, uuid,
                                            HID_DEV_OPER_READ, pLen, pValue);
        } else {
            *pLen = 0;
        }
    } else if (uuid == REPORT_MAP_UUID) {
        if (offset >= hidReportMapLen) {
            status = ATT_ERR_INVALID_OFFSET;
        } else {
            *pLen = MIN(maxLen, (hidReportMapLen - offset));
            tmos_memcpy(pValue, pAttr->pValue + offset, *pLen);
        }
    } else if (uuid == HID_INFORMATION_UUID) {
        *pLen = HID_INFORMATION_LEN;
        tmos_memcpy(pValue, pAttr->pValue, HID_INFORMATION_LEN);
    } else if (uuid == GATT_REPORT_REF_UUID) {
        *pLen = HID_REPORT_REF_LEN;
        tmos_memcpy(pValue, pAttr->pValue, HID_REPORT_REF_LEN);
    } else if (uuid == PROTOCOL_MODE_UUID) {
        *pLen = HID_PROTOCOL_MODE_LEN;
        pValue[0] = pAttr->pValue[0];
    } else if (uuid == GATT_EXT_REPORT_REF_UUID) {
        *pLen = HID_EXT_REPORT_REF_LEN;
        tmos_memcpy(pValue, pAttr->pValue, HID_EXT_REPORT_REF_LEN);
    }

    return (status);
}

bStatus_t HidDev_WriteAttrCB(uint16_t connHandle, gattAttribute_t *pAttr,
                             uint8_t *pValue, uint16_t len, uint16_t offset, uint8_t method)
{
    uint16_t     uuid;
    bStatus_t    status = SUCCESS;
    hidRptMap_t *pRpt;

    (void)method;

    if (offset > 0) {
        return (ATT_ERR_ATTR_NOT_LONG);
    }

    uuid = BUILD_UINT16(pAttr->type.uuid[0], pAttr->type.uuid[1]);

    if (uuid == REPORT_UUID || uuid == BOOT_KEY_OUTPUT_UUID) {
        if ((pRpt = hidDevRptByHandle(pAttr->handle)) != NULL) {
            status = (*pHidDevCB->reportCB)(pRpt->id, pRpt->type, uuid,
                                            HID_DEV_OPER_WRITE, &len, pValue);
        }
    } else if (uuid == HID_CTRL_PT_UUID) {
        if (len == 1) {
            if (pValue[0] == HID_CMD_SUSPEND || pValue[0] == HID_CMD_EXIT_SUSPEND) {
                (*pHidDevCB->evtCB)((pValue[0] == HID_CMD_SUSPEND) ? HID_DEV_SUSPEND_EVT : HID_DEV_EXIT_SUSPEND_EVT);
            } else {
                status = ATT_ERR_INVALID_VALUE;
            }
        } else {
            status = ATT_ERR_INVALID_VALUE_SIZE;
        }
    } else if (uuid == GATT_CLIENT_CHAR_CFG_UUID) {
        status = GATTServApp_ProcessCCCWriteReq(connHandle, pAttr, pValue, len,
                                                offset, GATT_CLIENT_CFG_NOTIFY);
        if (status == SUCCESS) {
            uint16_t charCfg = BUILD_UINT16(pValue[0], pValue[1]);

            if ((pRpt = hidDevRptByCccdHandle(pAttr->handle)) != NULL) {
                (*pHidDevCB->reportCB)(pRpt->id, pRpt->type, uuid,
                                       (charCfg == GATT_CLIENT_CFG_NOTIFY) ? HID_DEV_OPER_ENABLE : HID_DEV_OPER_DISABLE,
                                       &len, pValue);
            }
        }
    } else if (uuid == PROTOCOL_MODE_UUID) {
        if (len == HID_PROTOCOL_MODE_LEN) {
            if (pValue[0] == HID_PROTOCOL_MODE_BOOT || pValue[0] == HID_PROTOCOL_MODE_REPORT) {
                pAttr->pValue[0] = pValue[0];
                (*pHidDevCB->evtCB)((pValue[0] == HID_PROTOCOL_MODE_BOOT) ? HID_DEV_SET_BOOT_EVT : HID_DEV_SET_REPORT_EVT);
            } else {
                status = ATT_ERR_INVALID_VALUE;
            }
        } else {
            status = ATT_ERR_INVALID_VALUE_SIZE;
        }
    }

    return (status);
}

/* Reset the CCCDs of a link that went away, so a later host starts from
 * "notifications off" (a bonded host's values come back with its bond). */
static void hidDevHandleConnStatusCB(uint16_t connHandle, uint8_t changeType)
{
    uint8_t          i;
    hidRptMap_t     *p = pHidDevRptTbl;
    uint16_t         retHandle;
    gattAttribute_t *pAttr;

    if (connHandle == LOOPBACK_CONNHANDLE) {
        return;
    }
    if ((changeType == LINKDB_STATUS_UPDATE_REMOVED) ||
        ((changeType == LINKDB_STATUS_UPDATE_STATEFLAGS) && (!linkDB_Up(connHandle)))) {
        for (i = hidDevRptTblLen; i > 0; i--, p++) {
            if (p->cccdHandle != 0) {
                if ((pAttr = GATT_FindHandle(p->cccdHandle, &retHandle)) != NULL) {
                    GATTServApp_InitCharCfg(connHandle, (gattCharCfg_t *)pAttr->pValue);
                }
            }
        }
    }
}

static void hidDevDisconnected(void)
{
    Batt_HandleConnStatusCB(gapConnHandle, LINKDB_STATUS_UPDATE_REMOVED);
    hidDevHandleConnStatusCB(gapConnHandle, LINKDB_STATUS_UPDATE_REMOVED);

    hidDevConnSecure = FALSE;
    hidProtocolMode = HID_PROTOCOL_MODE_REPORT;
    gapConnHandle = GAP_CONNHANDLE_INIT;
}

static void hidDevGapStateCB(gapRole_States_t newState, gapRoleEvent_t *pEvent)
{
    newState &= GAPROLE_STATE_ADV_MASK;

    if (newState == GAPROLE_CONNECTED) {
        gapEstLinkReqEvent_t *event = (gapEstLinkReqEvent_t *)pEvent;

        gapConnHandle = event->connectionHandle;
        hidDevConnSecure = FALSE;
    } else if (hidDevGapState == GAPROLE_CONNECTED) {
        hidDevDisconnected();
    }

    hidDevGapState = newState;

    if (pHidDevCB && pHidDevCB->pfnStateChange) {
        (*pHidDevCB->pfnStateChange)(newState, pEvent);
    }
}

static void hidDevParamUpdateCB(uint16_t connHandle, uint16_t connInterval,
                                uint16_t connSlaveLatency, uint16_t connTimeout)
{
    if (pHidDevCB && pHidDevCB->paramUpdateCB) {
        (*pHidDevCB->paramUpdateCB)(connHandle, connInterval, connSlaveLatency, connTimeout);
    }
}

static void hidDevPairStateCB(uint16_t connHandle, uint8_t state, uint8_t status)
{
    /* COMPLETE ends a fresh pairing; BONDED ends the re-encryption of a
     * bonded host's reconnect. Either way the link is now encrypted. */
    if ((state == GAPBOND_PAIRING_STATE_COMPLETE || state == GAPBOND_PAIRING_STATE_BONDED)
            && status == SUCCESS) {
        hidDevConnSecure = TRUE;
    }

    if (pHidDevCB && pHidDevCB->pairStateCB) {
        (*pHidDevCB->pairStateCB)(connHandle, state, status);
    }
}

static void hidDevPasscodeCB(uint8_t *deviceAddr, uint16_t connectionHandle,
                             uint8_t uiInputs, uint8_t uiOutputs)
{
    if (pHidDevCB && pHidDevCB->passcodeCB) {
        (*pHidDevCB->passcodeCB)(deviceAddr, connectionHandle, uiInputs, uiOutputs);
    } else {
        uint32_t passkey;

        GAPBondMgr_GetParameter(GAPBOND_PERI_DEFAULT_PASSCODE, &passkey);
        GAPBondMgr_PasscodeRsp(connectionHandle, SUCCESS, passkey);
    }
}

static hidRptMap_t *hidDevRptByHandle(uint16_t handle)
{
    uint8_t      i;
    hidRptMap_t *p = pHidDevRptTbl;

    for (i = hidDevRptTblLen; i > 0; i--, p++) {
        if (p->handle == handle && p->mode == hidProtocolMode) {
            return p;
        }
    }
    return NULL;
}

static hidRptMap_t *hidDevRptByCccdHandle(uint16_t handle)
{
    uint8_t      i;
    hidRptMap_t *p = pHidDevRptTbl;

    for (i = hidDevRptTblLen; i > 0; i--, p++) {
        if (p->cccdHandle == handle) {
            return p;
        }
    }
    return NULL;
}

static hidRptMap_t *hidDevRptById(uint8_t id, uint8_t type)
{
    uint8_t      i;
    hidRptMap_t *p = pHidDevRptTbl;

    for (i = hidDevRptTblLen; i > 0; i--, p++) {
        if (p->id == id && p->type == type && p->mode == hidProtocolMode) {
            return p;
        }
    }
    return NULL;
}

static uint8_t hidDevSendNoti(uint16_t handle, uint8_t len, uint8_t *pData)
{
    uint8_t              status;
    attHandleValueNoti_t noti;

    noti.pValue = GATT_bm_alloc(gapConnHandle, ATT_HANDLE_VALUE_NOTI, len, NULL, 0);
    if (noti.pValue == NULL) {
        return bleMemAllocError;
    }
    noti.handle = handle;
    noti.len = len;
    tmos_memcpy(noti.pValue, pData, len);
    status = GATT_Notification(gapConnHandle, &noti, FALSE);
    if (status != SUCCESS) {
        GATT_bm_free((gattMsg_t *)&noti, ATT_HANDLE_VALUE_NOTI);
    }
    return status;
}
