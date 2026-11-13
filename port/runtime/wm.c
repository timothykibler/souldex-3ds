// The DS wireless manager (WM) as a radio with nobody in range. The game's menus and its comm
// system bring wireless up in the background (listening for beacons, Mystery Gift, the Pokewalker
// menu); any call failing is treated as a broken link and ends in the communication error screen.
// Here every call succeeds: asynchronous ones return WM_ERRCODE_OPERATING and complete through
// their callback at the next frame (as the ARM7 would answer), scans find no parent, and a parent
// never gets a child.
#include <string.h>
#include <nitro.h>
#include "port.h"

static WMStatus sStatus;
static WMCallbackFunc sIndCallback;

// ---------------------------------------------------------------- callbacks, delivered next frame
#define MAX_PENDING 16
typedef struct {
    WMCallbackFunc cb;
    union {
        WMCallback base;
        WMStartParentCallback parent;
        WMStartScanCallback scan;
        WMStartConnectCallback connect;
        WMMeasureChannelCallback measure;
        WMStartMPCallback mp;
        u8 raw[512];
    } u;
} Pending;
static Pending sPending[MAX_PENDING];
static int sNumPending;

static void *queue(WMCallbackFunc cb, WMApiid id) {
    if (!cb || sNumPending >= MAX_PENDING) return NULL;
    Pending *p = &sPending[sNumPending++];
    p->cb = cb;
    memset(&p->u, 0, sizeof(p->u));
    p->u.base.apiid = (u16)id;
    p->u.base.errcode = WM_ERRCODE_SUCCESS;
    return &p->u;
}

static WMErrCode done(WMCallbackFunc cb, WMApiid id, u16 state) {
    sStatus.state = state;
    queue(cb, id);
    return WM_ERRCODE_OPERATING;
}

void port_wm_frame(void) {
    // (callbacks may queue more: those run next frame)
    static Pending run[MAX_PENDING];
    int n = sNumPending;
    memcpy(run, sPending, sizeof(Pending) * (size_t)n);
    sNumPending = 0;
    for (int i = 0; i < n; i++) run[i].cb(&run[i].u);
}

// ---------------------------------------------------------------- set-up and power
WMErrCode WM_Init(void *wmSysBuf, u16 dmaNo) { (void)wmSysBuf; (void)dmaNo; sStatus.state = WM_STATE_STOP; return WM_ERRCODE_SUCCESS; }
WMErrCode WM_Initialize(void *wmSysBuf, WMCallbackFunc callback, u16 dmaNo) {
    (void)wmSysBuf; (void)dmaNo;
    return done(callback, WM_APIID_INITIALIZE, WM_STATE_IDLE);
}
WMErrCode WM_InitializeForListening(void *wmSysBuf, WMCallbackFunc callback, u16 dmaNo, BOOL blink) {
    (void)blink;
    return WM_Initialize(wmSysBuf, callback, dmaNo);
}
WMErrCode WM_Enable(WMCallbackFunc callback) { return done(callback, WM_APIID_ENABLE, WM_STATE_STOP); }
WMErrCode WM_Disable(WMCallbackFunc callback) { return done(callback, WM_APIID_DISABLE, WM_STATE_READY); }
WMErrCode WM_PowerOn(WMCallbackFunc callback) { return done(callback, WM_APIID_POWER_ON, WM_STATE_IDLE); }
WMErrCode WM_PowerOff(WMCallbackFunc callback) { return done(callback, WM_APIID_POWER_OFF, WM_STATE_STOP); }
WMErrCode WM_Reset(WMCallbackFunc callback) { return done(callback, WM_APIID_RESET, WM_STATE_IDLE); }
WMErrCode WM_End(WMCallbackFunc callback) { return done(callback, WM_APIID_END, WM_STATE_READY); }
WMErrCode WM_Finish(void) { sStatus.state = WM_STATE_READY; return WM_ERRCODE_SUCCESS; }
WMErrCode WM_SetIndCallback(WMCallbackFunc callback) { sIndCallback = callback; return WM_ERRCODE_SUCCESS; }
WMErrCode WM_SetPortCallback(u16 port, WMCallbackFunc callback, void *arg) { (void)port; (void)callback; (void)arg; return WM_ERRCODE_SUCCESS; }
WMErrCode WM_SetLifeTime(WMCallbackFunc callback, u16 tableNumber, u16 camLifeTime, u16 frameLifeTime, u16 mpLifeTime) {
    (void)tableNumber; (void)camLifeTime; (void)frameLifeTime; (void)mpLifeTime;
    return done(callback, WM_APIID_SET_LIFETIME, sStatus.state);
}
const WMStatus *WMi_GetStatusAddress(void) { return &sStatus; }

// ---------------------------------------------------------------- queries
u16 WM_GetAllowedChannel(void) { return 0x2082; }  // channels 1, 7 and 13
u16 WM_GetDispersionBeaconPeriod(void) { return 200; }
u16 WM_GetDispersionScanPeriod(void) { return 30; }
WMLinkLevel WM_GetLinkLevel(void) { return WM_LINK_LEVEL_0; }
u16 WM_GetNextTgid(void) { static u16 tgid; return ++tgid; }
WMErrCode WM_MeasureChannel(WMCallbackFunc callback, u16 ccaMode, u16 edThreshold, u16 channel, u16 measureTime) {
    (void)ccaMode; (void)edThreshold; (void)measureTime;
    WMMeasureChannelCallback *c = queue(callback, WM_APIID_MEASURE_CHANNEL);
    if (c) { c->channel = channel; c->ccaBusyRatio = 0; }
    return WM_ERRCODE_OPERATING;
}

// ---------------------------------------------------------------- scanning: nobody there
WMErrCode WM_StartScan(WMCallbackFunc callback, const WMScanParam *param) {
    sStatus.state = WM_STATE_SCAN;
    WMStartScanCallback *c = queue(callback, WM_APIID_START_SCAN);
    if (c) { c->state = WM_STATECODE_PARENT_NOT_FOUND; c->channel = param ? param->channel : 1; }
    return WM_ERRCODE_OPERATING;
}
WMErrCode WM_EndScan(WMCallbackFunc callback) { return done(callback, WM_APIID_END_SCAN, WM_STATE_IDLE); }
WMErrCode WM_StartConnectEx(WMCallbackFunc callback, const WMBssDesc *pInfo, const u8 *ssid, BOOL powerSave, u16 authMode) {
    (void)pInfo; (void)ssid; (void)powerSave; (void)authMode;
    WMStartConnectCallback *c = queue(callback, WM_APIID_START_CONNECT);
    if (c) { c->errcode = WM_ERRCODE_FAILED; c->state = WM_STATECODE_DISCONNECTED; }
    return WM_ERRCODE_OPERATING;
}
WMErrCode WM_Disconnect(WMCallbackFunc callback, u16 aid) { (void)aid; return done(callback, WM_APIID_DISCONNECT, WM_STATE_IDLE); }

// ---------------------------------------------------------------- being a parent: no child comes
WMErrCode WM_SetParentParameter(WMCallbackFunc callback, const WMParentParam *pparaBuf) {
    (void)pparaBuf;
    return done(callback, WM_APIID_SET_P_PARAM, sStatus.state);
}
WMErrCode WM_StartParent(WMCallbackFunc callback) {
    sStatus.state = WM_STATE_PARENT;
    WMStartParentCallback *c = queue(callback, WM_APIID_START_PARENT);
    if (c) c->state = WM_STATECODE_PARENT_START;
    return WM_ERRCODE_OPERATING;
}
WMErrCode WM_EndParent(WMCallbackFunc callback) { return done(callback, WM_APIID_END_PARENT, WM_STATE_IDLE); }
WMErrCode WM_SetGameInfo(WMCallbackFunc callback, const u16 *userGameInfo, u16 userGameInfoSize, u32 ggid, u16 tgid, u8 attr) {
    (void)userGameInfo; (void)userGameInfoSize; (void)ggid; (void)tgid; (void)attr;
    return done(callback, WM_APIID_SET_GAMEINFO, sStatus.state);
}
WMErrCode WM_SetEntry(WMCallbackFunc callback, BOOL enabled) { (void)enabled; return done(callback, WM_APIID_SET_ENTRY, sStatus.state); }
WMErrCode WM_StartMP(WMCallbackFunc callback, u16 *recvBuf, u16 recvBufSize, u16 *sendBuf, u16 sendBufSize, u16 mpFreq) {
    (void)recvBuf; (void)recvBufSize; (void)sendBuf; (void)sendBufSize; (void)mpFreq;
    sStatus.state = sStatus.state == WM_STATE_PARENT ? WM_STATE_MP_PARENT : WM_STATE_MP_CHILD;
    WMStartMPCallback *c = queue(callback, WM_APIID_START_MP);
    if (c) c->state = WM_STATECODE_MP_START;
    return WM_ERRCODE_OPERATING;
}
WMErrCode WM_EndMP(WMCallbackFunc callback) { return done(callback, WM_APIID_END_MP, WM_STATE_PARENT); }
WMErrCode WM_SetMPDataToPortEx(WMCallbackFunc callback, void *arg, const u16 *sendData, u16 sendDataSize, u16 destBitmap, u16 port, u16 prio) {
    (void)arg; (void)sendData; (void)sendDataSize; (void)destBitmap; (void)port; (void)prio;
    return done(callback, WM_APIID_SET_MP_DATA, sStatus.state);
}
