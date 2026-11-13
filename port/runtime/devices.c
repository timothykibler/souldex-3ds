// The DS devices the ARM7 looked after: buttons, the clock, power and the microphone.
#include <string.h>
#include <time.h>
#include <nitro.h>
#include "port.h"

// ---------------------------------------------------------------- buttons
// REG_KEYINPUT (A, B, SELECT, START, the D-pad, R, L: active low) and the ARM7's extra buttons
// word (X, Y, pen down, hinge: HW_BUTTON_XY_BUF, active low but the hinge).
// Scripted presses from debug.txt ("press <frame> <A|B|X|Y|L|R|START|SELECT|UP|DOWN|LEFT|RIGHT>"),
// each held for 8 frames: for testing without hands on the buttons.
typedef struct { u32 frame, until, period; u16 key; u16 xy; } Press;
static Press sPress[64];
static int sNumPress;
// "press F KEY": KEY held for 8 frames from frame F; "mash F T P KEY": the same every P frames from F to T.
void port_input_mash(u32 frame, u32 until, u32 period, const char *name);
void port_input_script(u32 frame, const char *name) { port_input_mash(frame, frame + 1, 0, name); }
void port_input_mash(u32 frame, u32 until, u32 period, const char *name) {
    static const struct { const char *n; u16 key, xy; } k[] = {
        { "A", 1, 0 }, { "B", 2, 0 }, { "SELECT", 4, 0 }, { "START", 8, 0 }, { "RIGHT", 16, 0 }, { "LEFT", 32, 0 },
        { "UP", 64, 0 }, { "DOWN", 128, 0 }, { "R", 256, 0 }, { "L", 512, 0 }, { "X", 0, 0x400 }, { "Y", 0, 0x800 } };
    for (size_t i = 0; i < sizeof(k) / sizeof(k[0]) && sNumPress < 64; i++) {
        if (!strcmp(k[i].n, name)) { sPress[sNumPress++] = (Press){ frame, until, period, k[i].key, k[i].xy }; return; }
    }
}

void port_input_update(void) {
    u16 keyinput, xy;
    port_ctr_buttons(&keyinput, &xy);
    for (int i = 0; i < sNumPress; i++) {
        const Press *p = &sPress[i];
        if (port_frame_count < p->frame || port_frame_count >= p->until + 8) continue;
        u32 ph = p->period ? (port_frame_count - p->frame) % p->period : port_frame_count - p->frame;
        if (ph < 8) {
            keyinput &= (u16)~sPress[i].key;
            xy &= (u16)~sPress[i].xy;
        }
    }
    *(vu16 *)REG_KEYINPUT_ADDR = keyinput;
    *(vu16 *)HW_BUTTON_XY_BUF = xy;
    void port_touch_update(void);
    port_touch_update();
}

// ---------------------------------------------------------------- clock (the 3DS's own time)
void RTC_Init(void) {}

static void now(RTCDate *date, RTCTime *t) {
    time_t s = time(NULL);
    struct tm tm;
    gmtime_r(&s, &tm);  // (the 3DS keeps local time in its clock)
    if (date) {
        date->year = (u32)(tm.tm_year % 100);
        date->month = (u32)(tm.tm_mon + 1);
        date->day = (u32)tm.tm_mday;
        date->week = (RTCWeek)tm.tm_wday;
    }
    if (t) {
        t->hour = (u32)tm.tm_hour;
        t->minute = (u32)tm.tm_min;
        t->second = (u32)tm.tm_sec;
    }
}
RTCResult RTC_GetDate(RTCDate *date) { now(date, NULL); return RTC_RESULT_SUCCESS; }
RTCResult RTC_GetTime(RTCTime *t) { now(NULL, t); return RTC_RESULT_SUCCESS; }
RTCResult RTC_GetDateTime(RTCDate *date, RTCTime *t) { now(date, t); return RTC_RESULT_SUCCESS; }
RTCResult RTC_GetDateTimeAsync(RTCDate *date, RTCTime *t, RTCCallback callback, void *arg) {
    now(date, t);
    if (callback) callback(RTC_RESULT_SUCCESS, arg);
    return RTC_RESULT_SUCCESS;
}

// ---------------------------------------------------------------- power
void PM_Init(void) {}
void PM_AppendPostSleepCallback(PMSleepCallbackInfo *info) { (void)info; }
void PM_AppendPreSleepCallback(PMSleepCallbackInfo *info) { (void)info; }
void PM_PrependPreSleepCallback(PMSleepCallbackInfo *info) { (void)info; }
void PM_DeletePostSleepCallback(PMSleepCallbackInfo *info) { (void)info; }
void PM_DeletePreSleepCallback(PMSleepCallbackInfo *info) { (void)info; }
u32 PM_ForceToPowerOff(void) { port_fatal("power off"); }
u32 PM_GetBackLight(PMBackLightSwitch *top, PMBackLightSwitch *bottom) {
    if (top) *top = PM_BACKLIGHT_ON;
    if (bottom) *bottom = PM_BACKLIGHT_ON;
    return PM_RESULT_SUCCESS;
}
u32 PM_GetLEDPattern(PMLEDPattern *p) { if (p) *p = PM_LED_PATTERN_ON; return PM_RESULT_SUCCESS; }
void PM_GoSleepMode(PMWakeUpTrigger trigger, PMLogic logic, u16 keyPattern) { (void)trigger; (void)logic; (void)keyPattern; }
u32 PM_SetAmp(PMAmpSwitch sw) { (void)sw; return PM_RESULT_SUCCESS; }
u32 PM_SetAmpGain(PMAmpGain gain) { (void)gain; return PM_RESULT_SUCCESS; }
u32 PM_SetBackLight(PMLCDTarget target, PMBackLightSwitch status) { (void)target; (void)status; return PM_RESULT_SUCCESS; }
BOOL PM_SetLCDPower(PMLCDPower sw) { (void)sw; return TRUE; }
u32 PMi_SendLEDPatternCommand(PMLEDPattern pattern) { (void)pattern; return PM_RESULT_SUCCESS; }

// ---------------------------------------------------------------- microphone (none yet)
void MIC_Init(void) {}
MICResult MIC_StartAutoSampling(const MICAutoParam *param) { (void)param; return MIC_RESULT_ILLEGAL_STATUS; }
MICResult MIC_StartAutoSamplingAsync(const MICAutoParam *param, MICCallback callback, void *arg) {
    (void)param; (void)callback; (void)arg; return MIC_RESULT_ILLEGAL_STATUS;
}
MICResult MIC_StopAutoSampling(void) { return MIC_RESULT_SUCCESS; }
void *MIC_GetLastSamplingAddress(void) { return NULL; }

// ---------------------------------------------------------------- DS Protect: an honest cartridge
u32 DSProt_DetectFlashcart(void *callback) { (void)callback; return 0; }
u32 DSProt_DetectEmulator(void *callback) { (void)callback; return 0; }
u32 DSProt_DetectDummy(void *callback) { (void)callback; return 0; }
static u32 not_detected(void *callback) { if (callback) ((void (*)(void))callback)(); return 1; }
u32 DSProt_DetectNotFlashcart(void *callback) { return not_detected(callback); }
u32 DSProt_DetectNotEmulator(void *callback) { return not_detected(callback); }
u32 DSProt_DetectNotDummy(void *callback) { return not_detected(callback); }

// ---------------------------------------------------------------- touch screen
// The DS touch screen sits 1:1 in the middle of the 3DS's bottom screen (320x240 around 256x192).
// Points are handed out already "calibrated": raw and screen coordinates are the same.
static TPData sTouch;
static TPData *sAutoBufs;
static u16 sAutoSize, sAutoIndex;

void port_touch_update(void) {
    u16 x, y;
    if (port_ctr_touch(&x, &y)) {
        sTouch.x = x;
        sTouch.y = y;
        sTouch.touch = TP_TOUCH_ON;
        sTouch.validity = TP_VALIDITY_VALID;
    } else {
        sTouch.touch = TP_TOUCH_OFF;
        sTouch.validity = TP_VALIDITY_VALID;
    }
    if (sAutoBufs && sAutoSize) {
        sAutoIndex = (u16)((sAutoIndex + 1) % sAutoSize);
        sAutoBufs[sAutoIndex] = sTouch;
    }
}

void TP_Init(void) {}
BOOL TP_GetUserInfo(TPCalibrateParam *calibrate) { memset(calibrate, 0, sizeof(*calibrate)); return TRUE; }
void TP_SetCalibrateParam(const TPCalibrateParam *param) { (void)param; }
void TP_RequestAutoSamplingStartAsync(u16 vcount, u16 frequence, TPData samplingBufs[], u16 bufSize) {
    (void)vcount; (void)frequence;
    sAutoBufs = samplingBufs;
    sAutoSize = bufSize;
    sAutoIndex = 0;
}
void TP_RequestAutoSamplingStopAsync(void) { sAutoBufs = NULL; sAutoSize = 0; }
void TP_RequestSamplingAsync(void) {}
u32 TP_GetSamplingResult(TPData *result) { *result = sTouch; return 0; }
u32 TP_WaitRawResult(TPData *result) { *result = sTouch; return 0; }
u32 TP_WaitCalibratedResult(TPData *result) { *result = sTouch; return 0; }
void TP_WaitBusy(TPRequestCommandFlag command_flgs) { (void)command_flgs; }
u32 TP_CheckError(TPRequestCommandFlag command) { (void)command; return 0; }
u16 TP_GetLatestIndexInAuto(void) { return sAutoIndex; }
void TP_GetLatestRawPointInAuto(TPData *result) { *result = sTouch; }
void TP_GetCalibratedPoint(TPData *disp, const TPData *raw) { *disp = *raw; }
void TP_GetUnCalibratedPoint(u16 *raw_x, u16 *raw_y, u16 dx, u16 dy) { *raw_x = dx; *raw_y = dy; }

// ---------------------------------------------------------------- PXI: the ARM7 link, with nobody on the other end
void PXI_Init(void) {}
void PXI_SetFifoRecvCallback(int fifotag, PXIFifoCallback callback) { (void)fifotag; (void)callback; }
BOOL PXI_IsCallbackReady(int fifotag, PXIProc proc) { (void)fifotag; (void)proc; return TRUE; }
// Sound command lists (SND_FlushCommand) are acknowledged at once, as the ARM7 sound driver would
// once it had run them: until phase 3 runs that driver here, nothing plays, but nothing waits.
int PXI_SendWordByFifo(int fifotag, u32 data, BOOL err) {
    (void)data; (void)err;
    if (fifotag == PXI_FIFO_TAG_SOUND && SNDi_SharedWork) SNDi_SharedWork->finishCommandTag++;
    return 0;
}

// ---------------------------------------------------------------- wireless driver (WVR)
// The game loads the DS wireless driver into VRAM at its menus (and only then talks to WM, whose
// calls fail as if no partner is there). Loading it succeeds at once: the callback runs before
// the call returns "operating", which is what the game's state flags expect.
WVRResult WVR_StartUpAsync(GXVRamARM7 vram, WVRCallbackFunc callback, void *arg) {
    (void)vram;
    if (callback) callback(arg, WVR_RESULT_SUCCESS);
    return WVR_RESULT_OPERATING;
}
WVRResult WVR_TerminateAsync(WVRCallbackFunc callback, void *arg) {
    if (callback) callback(arg, WVR_RESULT_SUCCESS);
    return WVR_RESULT_OPERATING;
}
