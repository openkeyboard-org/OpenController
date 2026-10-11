/********************************** (C) COPYRIGHT *******************************
 * File Name          : devinfoservice.h
 * Author             : WCH
 * Version            : V1.0
 * Date               : 2018/12/10
 * Description        : Device Information service
 *********************************************************************************
 * Copyright (c) 2021 Nanjing Qinheng Microelectronics Co., Ltd.
 * Attention: This software (modified or not) and binary are used for
 * microcontroller manufactured by Nanjing Qinheng Microelectronics.
 *******************************************************************************/

/* Modified for OpenController: see devinfoservice.c. */

#ifndef DEVINFOSERVICE_H
#define DEVINFOSERVICE_H

#define DEVINFO_PNP_ID_LEN 7

/* Registers the Device Information service with the GATT server. */
extern bStatus_t DevInfo_AddService(void);

#endif /* DEVINFOSERVICE_H */
