/* Copyright 2026 Eric Molitor (EMulator)
 * SPDX-License-Identifier: Apache-2.0
 *
 * BLE HID keyboard; see ble_hid.h. Built on the GAP peripheral role and the
 * HID over GATT glue in ble/ (from WCH's HID_Keyboard example), with the
 * profile scheme of WCH's tri-mode mouse: one bond block per profile
 * (ble.SNVAddr) and a static random address per profile, bumped on every
 * fresh pairing so a host holding a stale bond sees a new device.
 */

#include "CONFIG.h"
#include "HAL.h"
#include "battservice.h"
#include "ble_diag.h"
#include "ble_hid.h"
#include "hiddev.h"
#include "hidkbdservice.h"
#include "keyboard_uart.h"
#include "rtc_clock.h"
#include "transport_mode.h"

/* The library's working copy of its init config (WCH's tri-mode mouse
 * points ble.SNVAddr at a profile's bond block the same way). Not declared
 * by CH59xBLE_LIB.h. */
extern bleConfig_t ble;

/* LE Secure Connections: needs BLE_BUFF_MAX_LEN above 69 (the Makefile
 * sets it with this). */
#ifndef KBD_BLE_LESC
#define KBD_BLE_LESC 1
#endif

#ifndef KBD_BLE_NAME
#define KBD_BLE_NAME "OpenController $"
#endif

#define ST_PAIRING       0x31
#define ST_CONNECTED     0x32
#define ST_DISCONNECTED  0x33
#define ST_SELECTED      0x34
#define ST_RECONNECTING  0x35
#define ST_REJECTED      0x36
#define ST_BATTERY_OK    0x23

/* Task events. */
#define EVT_ADV_SLOW      0x0001
#define EVT_PAIR_TIMEOUT  0x0002
#define EVT_CONN_PARAM    0x0004
#define EVT_SEC_TIMEOUT   0x0008

/* Advertising, 0.625 ms units: 20 ms for pairing and the first 30 s of a
 * reconnect, then 211.25 ms (Apple's accessory guidelines list both). */
#define ADV_FAST_INT        32
#define ADV_SLOW_INT        338
#define ADV_FAST_MS         30000u
#define PAIR_WINDOW_MS      120000u
/* A link that isn't encrypted by then is not one of our hosts. */
#define SEC_TIMEOUT_MS      30000u
/* Ask for 15 ms, latency 30, 4 s supervision once the host has settled.
 * Apple: interval >= 15 ms, latency <= 30, timeout 2-6 s, and
 * interval * (latency + 1) * 3 < timeout (1.4 s here). */
#define CONN_INT            12
#define CONN_LATENCY        30
#define CONN_TIMEOUT        400
#define CONN_PARAM_DELAY_MS 5000u

/* Reports wait here for the link; ones not delivered within this long are
 * dropped (the next report carries the keyboard's state again). */
#define QUEUE_LEN           8u
#define QUEUE_MAX_AGE_US    10000000u

#define HID_KBD_REPORT_LEN  8u

typedef enum {
    ADV_NONE,
    ADV_PAIRING,
    ADV_RECONNECT_FAST,
    ADV_RECONNECT_SLOW,
} adv_kind_t;

static uint8_t task_id = INVALID_TASK_ID;
static uint8_t profile;
static uint8_t generation;

static uint8_t gap_started;
static uint8_t selected = 1;      /* a BLE boot means the host chose BLE */
static uint8_t pairing;           /* a pairing window is open */
static uint8_t pending_pair;      /* pair once the current link is down */
static uint8_t pending_unpair;
static uint8_t quiescing;
static uint8_t reported_secure;   /* 5B 32 sent for this link */
static uint8_t link_up;           /* between link established and its end */
static uint8_t boot_reported;

static adv_kind_t adv_running = ADV_NONE;  /* what the controller is doing */
static adv_kind_t adv_slow;                /* reconnect has dropped to slow */

static uint8_t public_addr[B_ADDR_LEN];
static uint8_t last_led;

/* Keyboard, consumer and system reports, in the order the host sent them. */
typedef struct {
    uint8_t id;    /* HID_RPT_ID_* */
    uint8_t len;
    uint8_t data[HID_KBD_REPORT_LEN];
} queued_report_t;

static queued_report_t queue[QUEUE_LEN];
static uint8_t  queue_head;
static uint8_t  queue_count;
static uint32_t queue_since;

static uint8_t name[GAP_DEVICE_NAME_LEN + 1];
static uint8_t name_len;

static uint8_t adv_data[] = {
    0x02, GAP_ADTYPE_FLAGS, GAP_ADTYPE_FLAGS_GENERAL | GAP_ADTYPE_FLAGS_BREDR_NOT_SUPPORTED,
    0x03, GAP_ADTYPE_APPEARANCE, LO_UINT16(GAP_APPEARE_HID_KEYBOARD), HI_UINT16(GAP_APPEARE_HID_KEYBOARD),
    0x03, GAP_ADTYPE_16BIT_COMPLETE, LO_UINT16(HID_SERV_UUID), HI_UINT16(HID_SERV_UUID),
};
static uint8_t scan_rsp[2 + GAP_DEVICE_NAME_LEN];

static hidDevCfg_t hid_cfg = {
    0,
    HID_FEATURE_FLAGS,
};

static void adv_apply(void);
static void hid_state_cb(gapRole_States_t new_state, gapRoleEvent_t *event);
static void hid_pair_state_cb(uint16_t conn, uint8_t state, uint8_t status);
static uint8_t hid_report_cb(uint8_t id, uint8_t type, uint16_t uuid,
                             uint8_t oper, uint16_t *len, uint8_t *data);
static void hid_evt_cb(uint8_t evt);
static void hid_param_cb(uint16_t conn, uint16_t interval, uint16_t latency, uint16_t timeout);

static hidDevCB_t hid_cbs = {
    hid_report_cb,
    hid_evt_cb,
    NULL,
    hid_state_cb,
    hid_pair_state_cb,
    hid_param_cb,
};

#if RF_DIAG_COUNTERS
static uint32_t pair_started_at;
#endif

static uint8_t bonded(void)
{
    uint8_t count = 0;

    GAPBondMgr_GetParameter(GAPBOND_BOND_COUNT, &count);
    return count > 0;
}

static void set_pairing_allowed(uint8_t allowed)
{
    uint8_t mode = allowed ? GAPBOND_PAIRING_MODE_WAIT_FOR_REQ
                           : GAPBOND_PAIRING_MODE_NO_PAIRING;

    GAPBondMgr_SetParameter(GAPBOND_PERI_PAIRING_MODE, sizeof(mode), &mode);
}

static void queue_clear(void)
{
    queue_head = 0;
    queue_count = 0;
}

/* --- name ------------------------------------------------------------- */

static void name_apply(void)
{
    uint8_t shown[GAP_DEVICE_NAME_LEN];

    for (uint8_t i = 0; i < name_len; i++) {
        shown[i] = name[i] == '$' ? (uint8_t)('1' + profile) : name[i];
    }
    GGS_SetParameter(GGS_DEVICE_NAME_ATT, name_len, shown);

    scan_rsp[0] = (uint8_t)(name_len + 1u);
    scan_rsp[1] = GAP_ADTYPE_LOCAL_NAME_COMPLETE;
    tmos_memcpy(&scan_rsp[2], shown, name_len);
    GAPRole_SetParameter(GAPROLE_SCAN_RSP_DATA, (uint16_t)(name_len + 2u), scan_rsp);
}

void BleHid_SetName(const uint8_t *new_name, uint8_t len)
{
    if (len == 0) {
        return;
    }
    if (len > GAP_DEVICE_NAME_LEN) {
        len = GAP_DEVICE_NAME_LEN;
    }
    tmos_memcpy(name, new_name, len);
    name_len = len;
    if (gap_started) {
        name_apply();
    }
}

/* --- address ---------------------------------------------------------- */

/* Static random address for this profile and generation, derived from the
 * chip's own address. The two most significant bits must be 1 for a static
 * address; the library doesn't enforce that. */
static void address_apply(void)
{
    uint8_t addr[B_ADDR_LEN];

    tmos_memcpy(addr, public_addr, B_ADDR_LEN);
    addr[4] ^= (uint8_t)(0x11u * (profile + 1u));
    addr[3] ^= generation;
    addr[5] |= 0xC0u;
    GAP_ConfigDeviceAddr(ADDRTYPE_STATIC, addr);
}

/* --- advertising ------------------------------------------------------ */

static adv_kind_t adv_wanted(void)
{
    if (!gap_started || quiescing || !selected || HidDev_Connected()) {
        return ADV_NONE;
    }
    if (pending_pair || pending_unpair) {
        return ADV_NONE;
    }
    if (pairing) {
        return ADV_PAIRING;
    }
    if (bonded()) {
        return adv_slow ? ADV_RECONNECT_SLOW : ADV_RECONNECT_FAST;
    }
    return ADV_NONE;
}

static void adv_enable(uint8_t on)
{
    GAPRole_SetParameter(GAPROLE_ADVERT_ENABLED, sizeof(on), &on);
}

/* Bring the controller to adv_wanted(). Parameters only take effect when
 * advertising starts, so a change of kind stops it first; the state
 * callback calls back here once it has stopped. */
static void adv_apply(void)
{
    adv_kind_t want = adv_wanted();
    uint8_t    filter;
    uint16_t   interval;

    if (want == adv_running) {
        return;
    }
    if (adv_running != ADV_NONE) {
        adv_enable(FALSE);
        return;
    }
    if (want == ADV_NONE) {
        return;
    }

    /* A bonded profile answers scans from its host only, but takes a
     * connection from anyone: a host with a resolvable private address may
     * not resolve against the white list (WCH's mouse does the same). A
     * stranger that connects can't pair and can't read an attribute, and
     * is dropped after SEC_TIMEOUT_MS. */
    filter = (want == ADV_PAIRING) ? GAP_FILTER_POLICY_ALL : GAP_FILTER_POLICY_WHITE_SCAN;
    interval = (want == ADV_RECONNECT_SLOW) ? ADV_SLOW_INT : ADV_FAST_INT;

    set_pairing_allowed(want == ADV_PAIRING);
    GAPRole_SetParameter(GAPROLE_ADV_FILTER_POLICY, sizeof(filter), &filter);
    GAP_SetParamValue(TGAP_DISC_ADV_INT_MIN, interval);
    GAP_SetParamValue(TGAP_DISC_ADV_INT_MAX, interval);
    adv_enable(TRUE);
    adv_running = want;

    if (want == ADV_RECONNECT_FAST) {
        tmos_start_task(task_id, EVT_ADV_SLOW, MS1_TO_SYSTEM_TIME(ADV_FAST_MS));
    }
}

/* --- link state ------------------------------------------------------- */

static void report_link_state(void)
{
    if (HidDev_Secure()) {
        KeyboardUart_SendStatus(ST_CONNECTED);
    } else if (pairing || pending_pair) {
        KeyboardUart_SendStatus(ST_PAIRING);
    } else if (selected && bonded()) {
        KeyboardUart_SendStatus(ST_RECONNECTING);
    } else {
        KeyboardUart_SendStatus(ST_REJECTED);
    }
}

static void do_pair(void)
{
    pending_pair = 0;
    pairing = 1;
    selected = 1;
    adv_slow = 0;
    GAPBondMgr_SetParameter(GAPBOND_ERASE_ALLBONDS, 0, NULL);
    TransportMode_BumpGeneration(profile);
    generation = TransportMode_Generation(profile);
    address_apply();
    tmos_start_task(task_id, EVT_PAIR_TIMEOUT, MS1_TO_SYSTEM_TIME(PAIR_WINDOW_MS));
    adv_apply();
}

static void do_unpair(void)
{
    pending_unpair = 0;
    GAPBondMgr_SetParameter(GAPBOND_ERASE_ALLBONDS, 0, NULL);
    TransportMode_BumpGeneration(profile);
    generation = TransportMode_Generation(profile);
    address_apply();
    adv_apply();
}

static void on_started(void)
{
    gap_started = 1;
    GAPRole_GetParameter(GAPROLE_BD_ADDR, public_addr);
    address_apply();
    name_apply();
    adv_apply();

    if (!boot_reported) {
        boot_reported = 1;
        KeyboardUart_SendStatus(ST_SELECTED);
        report_link_state();
    }
}

static void on_disconnected(void)
{
    reported_secure = 0;
    tmos_stop_task(task_id, EVT_CONN_PARAM);
    tmos_stop_task(task_id, EVT_SEC_TIMEOUT);
    /* A pair or unpair that ended the link already said where it stands,
     * and after an A6 11 the host has left Bluetooth. */
    if (!quiescing && selected && !pending_pair && !pending_unpair) {
        KeyboardUart_SendStatus(ST_DISCONNECTED);
    }
    if (pending_unpair) {
        do_unpair();
    }
    if (pending_pair) {
        do_pair();
        return;
    }
    adv_slow = 0;
    adv_apply();
    if (!quiescing && selected && adv_running != ADV_NONE && !pairing) {
        KeyboardUart_SendStatus(ST_RECONNECTING);
    }
}

static void hid_state_cb(gapRole_States_t new_state, gapRoleEvent_t *event)
{
    new_state &= GAPROLE_STATE_ADV_MASK;

    /* Whatever state the role moves to from a connection, the link is
     * gone. */
    if (link_up && new_state != GAPROLE_CONNECTED) {
        BLE_DIAG(ble_diag.disconnects++);
        if (event->gap.opcode == GAP_LINK_TERMINATED_EVENT) {
            BLE_DIAG(ble_diag.last_disconnect_reason = event->linkTerminate.reason);
        }
        link_up = 0;
        adv_running = ADV_NONE;
        on_disconnected();
        return;
    }

    switch (new_state) {
    case GAPROLE_STARTED:
        if (!gap_started) {
            on_started();
            return;
        }
        break;

    case GAPROLE_ADVERTISING:
        if (adv_running == ADV_NONE) {
            adv_enable(FALSE);   /* not ours (a role restart): stop it */
        }
        return;

    case GAPROLE_CONNECTED:
        if (event->gap.opcode == GAP_LINK_ESTABLISHED_EVENT) {
            BLE_DIAG(ble_diag.connects++);
            BLE_DIAG(ble_diag.conn_interval = event->linkCmpl.connInterval);
            BLE_DIAG(ble_diag.conn_latency = event->linkCmpl.connLatency);
            BLE_DIAG(ble_diag.conn_timeout = event->linkCmpl.connTimeout);
            link_up = 1;
            adv_running = ADV_NONE;   /* the peripheral role stops on connect */
            tmos_stop_task(task_id, EVT_ADV_SLOW);
            tmos_start_task(task_id, EVT_SEC_TIMEOUT, MS1_TO_SYSTEM_TIME(SEC_TIMEOUT_MS));
        }
        return;

    default:
        break;
    }

    /* Advertising stopped (or never ran). An address change waits for this:
     * the controller takes a new address only while it isn't advertising. */
    adv_running = ADV_NONE;
    if (!HidDev_Connected()) {
        if (pending_unpair) {
            do_unpair();
        }
        if (pending_pair) {
            do_pair();
            return;
        }
    }
    adv_apply();
}

static void hid_param_cb(uint16_t conn, uint16_t interval, uint16_t latency, uint16_t timeout)
{
    (void)conn;
    (void)interval;
    (void)latency;
    (void)timeout;
    BLE_DIAG(ble_diag.conn_interval = interval);
    BLE_DIAG(ble_diag.conn_latency = latency);
    BLE_DIAG(ble_diag.conn_timeout = timeout);
}

static void hid_pair_state_cb(uint16_t conn, uint8_t state, uint8_t status)
{
    (void)conn;

#if RF_DIAG_COUNTERS
    if (state == GAPBOND_PAIRING_STATE_STARTED) {
        pair_started_at = RtcClock_Now();
    } else if (state == GAPBOND_PAIRING_STATE_COMPLETE) {
        ble_diag.pair_ticks = RtcClock_Now() - pair_started_at;
        ble_diag.last_pair_status = status;
        if (status == SUCCESS) {
            ble_diag.pair_ok++;
        } else {
            ble_diag.pair_fail++;
        }
    }
#endif

    if (state == GAPBOND_PAIRING_STATE_COMPLETE && status != SUCCESS) {
        /* A failed attempt inside the pairing window leaves it open for
         * the host to try again; outside it, the host's old bond is stale
         * or it isn't our host. */
        if (!pairing) {
            KeyboardUart_SendStatus(ST_REJECTED);
        }
        return;
    }
    if ((state == GAPBOND_PAIRING_STATE_COMPLETE || state == GAPBOND_PAIRING_STATE_BONDED)
            && status == SUCCESS && !reported_secure) {
        reported_secure = 1;
        if (pairing) {
            pairing = 0;
            tmos_stop_task(task_id, EVT_PAIR_TIMEOUT);
            set_pairing_allowed(FALSE);
        }
        tmos_stop_task(task_id, EVT_SEC_TIMEOUT);
        tmos_start_task(task_id, EVT_CONN_PARAM, MS1_TO_SYSTEM_TIME(CONN_PARAM_DELAY_MS));
        KeyboardUart_SendStatus(ST_CONNECTED);
        KeyboardUart_SendStatus(ST_BATTERY_OK);
        /* As the 2.4 GHz link does on connect: start from all LEDs off
         * until this host writes its own state. */
        last_led = 0;
        KeyboardUart_SendLed(0x00);
    }
}

/* --- HID callbacks ---------------------------------------------------- */

static uint8_t hid_report_cb(uint8_t id, uint8_t type, uint16_t uuid,
                             uint8_t oper, uint16_t *len, uint8_t *data)
{
    uint8_t status = SUCCESS;

    if (oper == HID_DEV_OPER_WRITE) {
        if ((uuid == REPORT_UUID || uuid == BOOT_KEY_OUTPUT_UUID)
                && type == HID_REPORT_TYPE_OUTPUT) {
            if (*len != 1) {
                return ATT_ERR_INVALID_VALUE_SIZE;
            }
            if (data[0] != last_led) {
                last_led = data[0];
                KeyboardUart_SendLed(last_led);
            }
        }
        status = Hid_SetParameter(id, type, uuid, (uint8_t)*len, data);
    } else if (oper == HID_DEV_OPER_READ) {
        status = Hid_GetParameter(id, type, uuid, len, data);
    }
    return status;
}

static void hid_evt_cb(uint8_t evt)
{
    (void)evt;   /* suspend and boot-protocol changes need nothing here */
}

/* --- task ------------------------------------------------------------- */

static uint16_t task_event(uint8_t id, uint16_t events)
{
    (void)id;

    if (events & SYS_EVENT_MSG) {
        uint8_t *msg = tmos_msg_receive(task_id);

        if (msg != NULL) {
            tmos_msg_deallocate(msg);
        }
        return events ^ SYS_EVENT_MSG;
    }
    if (events & EVT_ADV_SLOW) {
        adv_slow = 1;
        adv_apply();
        return events ^ EVT_ADV_SLOW;
    }
    if (events & EVT_PAIR_TIMEOUT) {
        if (pairing && !HidDev_Secure()) {
            pairing = 0;
            adv_apply();
            KeyboardUart_SendStatus(ST_REJECTED);
        }
        return events ^ EVT_PAIR_TIMEOUT;
    }
    if (events & EVT_CONN_PARAM) {
        if (HidDev_Connected()) {
            GAPRole_PeripheralConnParamUpdateReq(HidDev_ConnHandle(), CONN_INT, CONN_INT,
                                                 CONN_LATENCY, CONN_TIMEOUT, task_id);
        }
        return events ^ EVT_CONN_PARAM;
    }
    if (events & EVT_SEC_TIMEOUT) {
        if (HidDev_Connected() && !HidDev_Secure()) {
            GAPRole_TerminateLink(HidDev_ConnHandle());
        }
        return events ^ EVT_SEC_TIMEOUT;
    }
    return 0;
}

/* --- public ----------------------------------------------------------- */

void BleHid_Init(uint8_t booted_profile, uint8_t booted_generation)
{
    profile = booted_profile;
    generation = booted_generation;

    /* Before the role starts: the bond manager loads this profile's bonds
     * from here. */
    ble.SNVAddr = TRANSPORT_SNV_OFF(profile);

    {
        uint8_t        stored_len;
        const uint8_t *stored = TransportMode_Name(&stored_len);

        if (stored_len != 0) {
            BleHid_SetName(stored, stored_len);
        } else {
            BleHid_SetName((const uint8_t *)KBD_BLE_NAME, (uint8_t)(sizeof(KBD_BLE_NAME) - 1u));
        }
    }

    GAPRole_PeripheralInit();
    HidDev_Init();

    task_id = TMOS_ProcessEventRegister(task_event);

    {
        uint8_t off = FALSE;
        GAPRole_SetParameter(GAPROLE_ADVERT_ENABLED, sizeof(off), &off);
        GAPRole_SetParameter(GAPROLE_ADVERT_DATA, sizeof(adv_data), adv_data);
    }
    {
        /* Restore the IRK this profile bonded with, as WCH's mouse does;
         * the library otherwise makes up a new one each boot. */
        uint8_t irk[KEYLEN];
        uint8_t all_ff = 0xFF;
        uint8_t any = 0;

        if (tmos_snv_read(BLE_NVID_IRK, KEYLEN, irk) == SUCCESS) {
            for (uint8_t i = 0; i < KEYLEN; i++) {
                all_ff &= irk[i];
                any |= irk[i];
            }
            if (any != 0 && all_ff != 0xFF) {
                GAPRole_SetParameter(GAPROLE_IRK, KEYLEN, irk);
            }
        }
    }
    {
        uint32_t passkey = 0;
        uint8_t  mitm = FALSE;
        uint8_t  io = GAPBOND_IO_CAP_NO_INPUT_NO_OUTPUT;
        uint8_t  bond = TRUE;
        uint8_t  on = TRUE;
        uint8_t  off = FALSE;

        GAPBondMgr_SetParameter(GAPBOND_PERI_DEFAULT_PASSCODE, sizeof(passkey), &passkey);
        GAPBondMgr_SetParameter(GAPBOND_PERI_MITM_PROTECTION, sizeof(mitm), &mitm);
        GAPBondMgr_SetParameter(GAPBOND_PERI_IO_CAPABILITIES, sizeof(io), &io);
        GAPBondMgr_SetParameter(GAPBOND_PERI_BONDING_ENABLED, sizeof(bond), &bond);
        /* A full table must not silently erase the bond it holds. */
        GAPBondMgr_SetParameter(GAPBOND_ERASE_AUTO, sizeof(off), &off);
        GAPBondMgr_SetParameter(GAPBOND_AUTO_SYNC_RL, sizeof(on), &on);
#if KBD_BLE_LESC
        GAPBondMgr_SetParameter(GAPBOND_PERI_SC_PROTECTION, sizeof(on), &on);
#endif
        set_pairing_allowed(FALSE);
    }
    {
        gapPeriConnectParams_t params = {CONN_INT, CONN_INT, CONN_LATENCY, CONN_TIMEOUT};
        GGS_SetParameter(GGS_PERI_CONN_PARAM_ATT, sizeof(params), &params);
    }

    Hid_AddService();
    HidDev_Register(&hid_cfg, &hid_cbs);
}

void BleHid_Select(void)
{
    selected = 1;
    quiescing = 0;
    KeyboardUart_SendStatus(ST_SELECTED);
    if (!gap_started) {
        return;   /* on_started reports the state */
    }
    adv_apply();
    report_link_state();
}

void BleHid_Idle(void)
{
    selected = 0;
    pairing = 0;
    pending_pair = 0;
    tmos_stop_task(task_id, EVT_PAIR_TIMEOUT);
    queue_clear();
    if (HidDev_Connected()) {
        GAPRole_TerminateLink(HidDev_ConnHandle());
    } else {
        adv_apply();
    }
}

void BleHid_Pair(void)
{
    selected = 1;
    queue_clear();
    if (HidDev_Connected()) {
        pending_pair = 1;
        GAPRole_TerminateLink(HidDev_ConnHandle());
    } else if (adv_running != ADV_NONE) {
        pending_pair = 1;
        adv_apply();          /* stops it; the state callback then pairs */
    } else {
        do_pair();
    }
    KeyboardUart_SendStatus(ST_PAIRING);
}

void BleHid_Unpair(void)
{
    pairing = 0;
    pending_pair = 0;
    tmos_stop_task(task_id, EVT_PAIR_TIMEOUT);
    queue_clear();
    if (HidDev_Connected()) {
        pending_unpair = 1;
        GAPRole_TerminateLink(HidDev_ConnHandle());
    } else if (adv_running != ADV_NONE) {
        pending_unpair = 1;
        adv_apply();          /* stops it; the state callback then unpairs */
    } else {
        do_unpair();
    }
    KeyboardUart_SendStatus(ST_DISCONNECTED);
}

/* Every report carries the full state of its kind, so when the queue is
 * full the oldest report that a newer one of the same kind supersedes can
 * go: the host still ends in every kind's final state, and a media key
 * can't be left held. With three kinds in eight slots there always is one. */
static void queue_drop_superseded(void)
{
    for (uint8_t i = 0; i < queue_count; i++) {
        uint8_t slot = (uint8_t)((queue_head + i) % QUEUE_LEN);

        for (uint8_t j = (uint8_t)(i + 1u); j < queue_count; j++) {
            if (queue[(queue_head + j) % QUEUE_LEN].id == queue[slot].id) {
                for (uint8_t k = i; k + 1u < queue_count; k++) {
                    queue[(queue_head + k) % QUEUE_LEN] = queue[(queue_head + k + 1u) % QUEUE_LEN];
                }
                queue_count--;
                BLE_DIAG(ble_diag.reports_dropped++);
                return;
            }
        }
    }
}

static void queue_push(uint8_t id, const uint8_t *data, uint8_t len)
{
    queued_report_t *entry;

    if (queue_count == 0) {
        queue_since = RtcClock_Now();
    }
    if (queue_count == QUEUE_LEN) {
        queue_drop_superseded();
    }
    entry = &queue[(queue_head + queue_count) % QUEUE_LEN];
    queue_count++;
    entry->id = id;
    entry->len = len;
    tmos_memcpy(entry->data, data, len);

    /* A key on a bonded profile that isn't advertising (after an A6 11,
     * say) is the cue to reconnect, as on the 2.4 GHz link. */
    if (!HidDev_Connected() && selected) {
        adv_apply();
    }
}

void BleHid_QueueReport(const uint8_t report[8])
{
    queue_push(HID_RPT_ID_KEY_IN, report, HID_KBD_REPORT_LEN);
}

void BleHid_QueueConsumer(uint16_t usage)
{
    uint8_t data[2] = {LO_UINT16(usage), HI_UINT16(usage)};

    queue_push(HID_RPT_ID_CONSUMER_IN, data, sizeof(data));
}

void BleHid_QueueSystem(uint8_t bits)
{
    /* The vendor frame's bitmap (Power Down, Sleep, Wake Up) as the one
     * usage the report carries. */
    uint16_t usage = (bits & 0x01u) ? 0x81u : (bits & 0x02u) ? 0x82u : (bits & 0x04u) ? 0x83u : 0u;
    uint8_t  data[2] = {LO_UINT16(usage), HI_UINT16(usage)};

    queue_push(HID_RPT_ID_SYSTEM_IN, data, sizeof(data));
}

void BleHid_Service(void)
{
#if RF_DIAG_COUNTERS
    ble_diag.adv_running = (uint8_t)adv_running;
    ble_diag.flags = (uint8_t)((link_up ? BLE_DIAG_FLAG_LINKED : 0u)
                               | (HidDev_Secure() ? BLE_DIAG_FLAG_ENCRYPTED : 0u)
                               | (pairing ? BLE_DIAG_FLAG_PAIRING : 0u));
    ble_diag.generation = generation;
#endif
    while (queue_count) {
        queued_report_t *entry = &queue[queue_head];

        if (!HidDev_ReportReady(HID_RPT_ID_KEY_IN, HID_REPORT_TYPE_INPUT)) {
            if ((uint32_t)(RtcClock_Now() - queue_since) > RTC_CLOCK_TICKS_US(QUEUE_MAX_AGE_US)) {
                BLE_DIAG(ble_diag.reports_dropped += queue_count);
                queue_clear();
            }
            return;
        }
        if (!HidDev_ReportReady(entry->id, HID_REPORT_TYPE_INPUT)) {
            /* The link is up but the host hasn't subscribed to this kind
             * (or is in boot protocol): it must not hold up the keys. */
            BLE_DIAG(ble_diag.reports_dropped++);
        } else if (HidDev_Report(entry->id, HID_REPORT_TYPE_INPUT,
                                 entry->len, entry->data) != SUCCESS) {
            return;   /* no buffer this event: try again next pass */
        } else {
            BLE_DIAG(ble_diag.reports_sent++);
        }
        queue_head = (uint8_t)((queue_head + 1u) % QUEUE_LEN);
        queue_count--;
        queue_since = RtcClock_Now();
    }
}

void BleHid_Quiesce(void)
{
    quiescing = 1;
    pairing = 0;
    pending_pair = 0;
    queue_clear();
    if (HidDev_Connected()) {
        GAPRole_TerminateLink(HidDev_ConnHandle());
    } else {
        adv_apply();
    }
}

uint8_t BleHid_Quiet(void)
{
    return !HidDev_Connected() && adv_running == ADV_NONE;
}

void BleHid_LedResync(void)
{
    KeyboardUart_SendLed(last_led);
}

void BleHid_SetBattery(uint8_t percent)
{
    if (percent <= 100u) {
        (void)Batt_SetLevel(percent);
    }
}
