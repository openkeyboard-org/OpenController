/********************************** (C) COPYRIGHT *******************************
 * File Name          : devinfoservice.c
 * Author             : WCH
 * Version            : V1.0
 * Date               : 2018/12/10
 * Description        : Device Information service
 *********************************************************************************
 * Copyright (c) 2021 Nanjing Qinheng Microelectronics Co., Ltd.
 * Attention: This software (modified or not) and binary are used for
 * microcontroller manufactured by Nanjing Qinheng Microelectronics.
 *******************************************************************************/

/* Modified for OpenController (Copyright 2026 Eric Molitor (EMulator),
 * Apache-2.0): cut down to the characteristics a keyboard needs - the PnP ID
 * HOGP requires, plus the manufacturer and model names hosts show - with the
 * values supplied by the board profile. Every attribute costs table RAM. */

#include "CONFIG.h"
#include "devinfoservice.h"

#ifndef KBD_BLE_MANUFACTURER
#define KBD_BLE_MANUFACTURER "OpenController"
#endif
#ifndef KBD_BLE_MODEL
#define KBD_BLE_MODEL "OpenController"
#endif
/* PnP ID: vendor-ID source 2 is the USB Implementers Forum, so the board's
 * USB IDs can be reused as they are. */
#ifndef KBD_BLE_PNP_VID_SOURCE
#define KBD_BLE_PNP_VID_SOURCE 0x02
#endif
#ifndef KBD_BLE_PNP_VID
#define KBD_BLE_PNP_VID 0x0000
#endif
#ifndef KBD_BLE_PNP_PID
#define KBD_BLE_PNP_PID 0x0000
#endif
#ifndef KBD_BLE_PNP_VER
#define KBD_BLE_PNP_VER 0x0000
#endif

static const uint8_t devInfoServUUID[ATT_BT_UUID_SIZE] = {
    LO_UINT16(DEVINFO_SERV_UUID), HI_UINT16(DEVINFO_SERV_UUID)};
static const uint8_t devInfoModelNumberUUID[ATT_BT_UUID_SIZE] = {
    LO_UINT16(MODEL_NUMBER_UUID), HI_UINT16(MODEL_NUMBER_UUID)};
static const uint8_t devInfoMfrNameUUID[ATT_BT_UUID_SIZE] = {
    LO_UINT16(MANUFACTURER_NAME_UUID), HI_UINT16(MANUFACTURER_NAME_UUID)};
static const uint8_t devInfoPnpIdUUID[ATT_BT_UUID_SIZE] = {
    LO_UINT16(PNP_ID_UUID), HI_UINT16(PNP_ID_UUID)};

static const gattAttrType_t devInfoService = {ATT_BT_UUID_SIZE, devInfoServUUID};

static uint8_t       devInfoModelNumberProps = GATT_PROP_READ;
static const uint8_t devInfoModelNumber[] = KBD_BLE_MODEL;

static uint8_t       devInfoMfrNameProps = GATT_PROP_READ;
static const uint8_t devInfoMfrName[] = KBD_BLE_MANUFACTURER;

static uint8_t       devInfoPnpIdProps = GATT_PROP_READ;
static const uint8_t devInfoPnpId[DEVINFO_PNP_ID_LEN] = {
    KBD_BLE_PNP_VID_SOURCE,
    LO_UINT16(KBD_BLE_PNP_VID), HI_UINT16(KBD_BLE_PNP_VID),
    LO_UINT16(KBD_BLE_PNP_PID), HI_UINT16(KBD_BLE_PNP_PID),
    LO_UINT16(KBD_BLE_PNP_VER), HI_UINT16(KBD_BLE_PNP_VER),
};

static gattAttribute_t devInfoAttrTbl[] = {
    {{ATT_BT_UUID_SIZE, primaryServiceUUID}, GATT_PERMIT_READ, 0, (uint8_t *)&devInfoService},

    {{ATT_BT_UUID_SIZE, characterUUID}, GATT_PERMIT_READ, 0, &devInfoModelNumberProps},
    {{ATT_BT_UUID_SIZE, devInfoModelNumberUUID}, GATT_PERMIT_READ, 0, (uint8_t *)devInfoModelNumber},

    {{ATT_BT_UUID_SIZE, characterUUID}, GATT_PERMIT_READ, 0, &devInfoMfrNameProps},
    {{ATT_BT_UUID_SIZE, devInfoMfrNameUUID}, GATT_PERMIT_READ, 0, (uint8_t *)devInfoMfrName},

    {{ATT_BT_UUID_SIZE, characterUUID}, GATT_PERMIT_READ, 0, &devInfoPnpIdProps},
    {{ATT_BT_UUID_SIZE, devInfoPnpIdUUID}, GATT_PERMIT_READ, 0, (uint8_t *)devInfoPnpId},
};

static bStatus_t devInfo_ReadAttrCB(uint16_t connHandle, gattAttribute_t *pAttr,
                                    uint8_t *pValue, uint16_t *pLen, uint16_t offset, uint16_t maxLen, uint8_t method);

static gattServiceCBs_t devInfoCBs = {
    devInfo_ReadAttrCB,
    NULL,
    NULL,
};

bStatus_t DevInfo_AddService(void)
{
    return GATTServApp_RegisterService(devInfoAttrTbl,
                                       GATT_NUM_ATTRS(devInfoAttrTbl),
                                       GATT_MAX_ENCRYPT_KEY_SIZE,
                                       &devInfoCBs);
}

static bStatus_t devInfo_read(const uint8_t *value, uint16_t size,
                              uint8_t *pValue, uint16_t *pLen, uint16_t offset, uint16_t maxLen)
{
    if (offset >= size) {
        return ATT_ERR_INVALID_OFFSET;
    }
    *pLen = MIN(maxLen, (size - offset));
    tmos_memcpy(pValue, &value[offset], *pLen);
    return SUCCESS;
}

static bStatus_t devInfo_ReadAttrCB(uint16_t connHandle, gattAttribute_t *pAttr,
                                    uint8_t *pValue, uint16_t *pLen, uint16_t offset, uint16_t maxLen, uint8_t method)
{
    uint16_t uuid = BUILD_UINT16(pAttr->type.uuid[0], pAttr->type.uuid[1]);

    (void)connHandle;
    (void)method;

    switch (uuid) {
    case MODEL_NUMBER_UUID:   /* strings go out without their NUL */
        return devInfo_read(devInfoModelNumber, sizeof(devInfoModelNumber) - 1,
                            pValue, pLen, offset, maxLen);
    case MANUFACTURER_NAME_UUID:
        return devInfo_read(devInfoMfrName, sizeof(devInfoMfrName) - 1,
                            pValue, pLen, offset, maxLen);
    case PNP_ID_UUID:
        return devInfo_read(devInfoPnpId, sizeof(devInfoPnpId),
                            pValue, pLen, offset, maxLen);
    default:
        *pLen = 0;
        return ATT_ERR_ATTR_NOT_FOUND;
    }
}
