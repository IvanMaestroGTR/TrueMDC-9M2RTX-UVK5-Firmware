/* Copyright 2023 Dual Tachyon
 * https://github.com/DualTachyon
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 *     Unless required by applicable law or agreed to in writing, software
 *     distributed under the License is distributed on an "AS IS" BASIS,
 *     WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *     See the License for the specific language governing permissions and
 *     limitations under the License.
 */
#include <stdint.h>
#include <string.h>
#include "app/mdc1200.h"
#include "app/dtmf.h"

#if defined(ENABLE_FMRADIO)
#include "app/fm.h"
#endif

#include "audio.h"
#include "bsp/dp32g030/gpio.h"
#include "dcs.h"
#include "driver/backlight.h"
#ifdef ENABLE_MESSENGER
#endif
#ifdef ENABLE_DOPPLER
#include "app/doppler.h"
#endif
#if defined(ENABLE_FMRADIO)
#include "driver/bk1080.h"
#endif

#include "driver/bk4819.h"
#include "driver/gpio.h"
#include "driver/system.h"
#include "driver/st7565.h"
#include "frequencies.h"
#include "functions.h"
#include "helper/battery.h"
#include "misc.h"
#include "radio.h"
#include "settings.h"
#include "ui/status.h"
#include "ui/ui.h"
#include "ui/main.h"

FUNCTION_Type_t gCurrentFunction;


bool FUNCTION_IsRx()
{
    return gCurrentFunction == FUNCTION_MONITOR ||
           gCurrentFunction == FUNCTION_INCOMING ||
           gCurrentFunction == FUNCTION_RECEIVE;
}

void FUNCTION_Init(void)
{
    g_CxCSS_TAIL_Found = false;
    g_CDCSS_Lost       = false;
    g_CTCSS_Lost       = false;

    g_SquelchLost      = false;

    gFlagTailToneEliminationComplete   = false;
    gTailToneEliminationCountdown_10ms = 0;
    gFoundCTCSS                        = false;
    gFoundCDCSS                        = false;
    gFoundCTCSSCountdown_10ms          = 0;
    gFoundCDCSSCountdown_10ms          = 0;
    gEndOfRxDetectedMaybe              = false;

    gCurrentCodeType = (gRxVfo->Modulation != MODULATION_FM) ? CODE_TYPE_OFF : gRxVfo->pRX->CodeType;

#ifdef ENABLE_VOX
    g_VOX_Lost     = false;
#endif

#ifdef ENABLE_DTMF_CALLING
    DTMF_clear_RX();
#endif

#ifdef ENABLE_NOAA
    gNOAACountdown_10ms = 0;

	if (IS_NOAA_CHANNEL(gRxVfo->CHANNEL_SAVE)) {
		gCurrentCodeType = CODE_TYPE_CONTINUOUS_TONE;
	}
#endif

    gUpdateStatus = true;
}
void FUNCTION_Foreground(const FUNCTION_Type_t PreviousFunction) {
#ifdef ENABLE_DTMF_CALLING
    if (gDTMF_ReplyState != DTMF_REPLY_NONE)
        RADIO_PrepareCssTX();
#endif
    if (PreviousFunction == FUNCTION_TRANSMIT) {
        ST7565_FixInterfGlitch();
        gVFO_RSSI_bar_level[0] = 0;
        gVFO_RSSI_bar_level[1] = 0;
    } else if (PreviousFunction != FUNCTION_RECEIVE) {
        return;
    }

#if defined(ENABLE_FMRADIO)
    if (gFmRadioMode)
        gFM_RestoreCountdown_10ms = fm_restore_countdown_10ms;
#endif

#ifdef ENABLE_DTMF_CALLING
    if (gDTMF_CallState == DTMF_CALL_STATE_CALL_OUT ||
        gDTMF_CallState == DTMF_CALL_STATE_RECEIVED ||
        gDTMF_CallState == DTMF_CALL_STATE_RECEIVED_STAY)
    {
        gDTMF_auto_reset_time_500ms = gEeprom.DTMF_auto_reset_time * 2;
    }
#endif
    gUpdateStatus = true;
}

void FUNCTION_PowerSave() {
    gPowerSave_10ms = gEeprom.BATTERY_SAVE * 10;
    gPowerSaveCountdownExpired = false;

    gRxIdleMode = true;

    gMonitor = false;

    BK4819_DisableVox();
    BK4819_Sleep();

    BK4819_ToggleGpioOut(BK4819_GPIO0_PIN28_RX_ENABLE, false);

    gUpdateStatus = true;

    if (gScreenToDisplay != DISPLAY_MENU)     // 1of11 .. don't close the menu
        GUI_SelectNextDisplay(DISPLAY_MAIN);
}

void FUNCTION_Transmit() {
    // if DTMF is enabled when TX'ing, it changes the TX audio filtering !! .. 1of11

#if defined(ENABLE_MESSENGER) || defined(ENABLE_MDC1200)
    enable_msg_rx(false);
#endif


    BK4819_DisableDTMF();

#ifdef ENABLE_DTMF_CALLING
    // clear the DTMF RX buffer
    DTMF_clear_RX();
#endif

    // clear the DTMF RX live decoder buffer
    gDTMF_RX_live_timeout = 0;
    memset(gDTMF_RX_live, 0, sizeof(gDTMF_RX_live));

#if defined(ENABLE_FMRADIO)
    if (gFmRadioMode)
        BK1080_Init(0, false);
#endif

#ifdef ENABLE_ALARM
    if (gAlarmState == ALARM_STATE_SITE_ALARM)
    {
        GUI_DisplayScreen();

        AUDIO_AudioPathOff();

        SYSTEM_DelayMs(20);
        BK4819_PlayTone(500, 0);
        SYSTEM_DelayMs(2);

        AUDIO_AudioPathOn();

        gEnableSpeaker = true;

        SYSTEM_DelayMs(60);
        BK4819_ExitTxMute();

        gAlarmToneCounter = 0;
        return;
    }
#endif

    gUpdateStatus = true;

    GUI_DisplayScreen();

    RADIO_SetTxParameters();

    // turn the RED LED on
    BK4819_ToggleGpioOut(BK4819_GPIO5_PIN1_RED, true);

    if (gEeprom.PTT_ID == PTT_ID_MODE_TMID_PRE ||
        gEeprom.PTT_ID == PTT_ID_MODE_TMID_BOTH) {
        BK4819_MuteMic();
        SYSTEM_DelayMs(gEeprom.REPEATER_TAIL_TONE_ELIMINATION * 100);
#ifdef ENABLE_MDC1200
        BK4819_SendTMIDBeacon(gEeprom.MDC1200_PREAMBLE_WHEN != MDC_PREAMBLE_WHEN_POST);
#else
        BK4819_SendTMIDBeacon(false);
#endif
        BK4819_UnmuteMic();
        SYSTEM_DelayMs(20);
    }

    // Process manually queued DTMF keying only. Automatic DTMF PTT-ID modes
    // are intentionally removed; the callsign beacon never falls through to TX-UP DTMF.
    DTMF_Reply(false);
#ifdef ENABLE_MDC1200
    const bool useFleetSyncPTTID = (gEeprom.PTT_ID == PTT_ID_MODE_FLEETSYNC_PRE ||
                                   gEeprom.PTT_ID == PTT_ID_MODE_FLEETSYNC_POST ||
                                   gEeprom.PTT_ID == PTT_ID_MODE_FLEETSYNC_BOTH);
    const bool hasPreIdPTTID = (gEeprom.PTT_ID == PTT_ID_MODE_MDC_PRE ||
                               gEeprom.PTT_ID == PTT_ID_MODE_MDC_BOTH ||
                               gEeprom.PTT_ID == PTT_ID_MODE_FLEETSYNC_PRE ||
                               gEeprom.PTT_ID == PTT_ID_MODE_FLEETSYNC_BOTH);

    if (hasPreIdPTTID) {
//        BK4819_start_tone(740, 60, true, true);
//                            SYSTEM_DelayMs(120);
//                            BK4819_stop_tones(true);    //off as they get quite annoying sometimes...
        // Mute mic before PRE-ID transmission
        BK4819_MuteMic();
        
        // Delay before sending the PRE-ID packet (200-1000 ms).
#ifdef ENABLE_FLEETSYNC
        if (useFleetSyncPTTID)
            SYSTEM_DelayMs(gEeprom.FLEETSYNC_PTT_ID_DELAY * 100);
        else
#endif
            SYSTEM_DelayMs(gEeprom.REPEATER_TAIL_TONE_ELIMINATION * 100);
        
        // Determine preamble duration based on MDC preamble settings
        // Skip 1 and 9 - if value is 1, treat as 0 (OFF)
        uint8_t preamble_duration = (gEeprom.MDC1200_PREAMBLE_WHEN == MDC_PREAMBLE_WHEN_POST) ? 
                                     0 : gEeprom.MDC1200_PREAMBLE_DURATION;
        if (preamble_duration == 1) {
            preamble_duration = 0;  // Remove 1 preamble entirely
        }
#ifdef ENABLE_FLEETSYNC
        if (useFleetSyncPTTID)
            BK4819_send_FleetSync(gEeprom.FLEETSYNC_FLEET, gEeprom.FLEETSYNC_UNIT, false);
        else
#endif
            BK4819_send_MDC1200(1, 0x80, gEeprom.MDC1200_ID, preamble_duration);

#ifdef ENABLE_MDC1200_SIDE_BEEP
        BK4819_UnmuteMic();
        //BK4819_ToggleGpioOut(BK4819_GPIO6_PIN2_GREEN, true);
        
#endif
    } else {
         //Play side tone sequence only when PRE MDC ID is not enabled
#ifdef ENABLE_MDC1200_SIDE_BEEP
    BK4819_UnmuteMic();
        //BK4819_ToggleGpioOut(BK4819_GPIO6_PIN2_GREEN, true);
#endif
    }
#endif

    // Automatic DTMF PTT-ID has been removed, so retain the normal
    // talk-permit tone independently of manual DTMF keying.
    if ((gEeprom.field38_0x33 & 7) != TALK_PERMIT_TONE_OFF &&
        gEeprom.BOOT_BEEP_CONTROL) {
        BK4819_PlayTalkPermitToneTx(gEeprom.field38_0x33 & 7, false);
    }

#ifdef ENABLE_MESSENGER
    #ifdef ENABLE_MDC1200

    }
    #endif

#endif
#if defined(ENABLE_ALARM) || defined(ENABLE_TX1750)
    if (gAlarmState != ALARM_STATE_OFF) {
#ifdef ENABLE_TX1750
        if (gAlarmState == ALARM_STATE_TX1750)
            BK4819_TransmitTone(true, 1750);
#endif

#ifdef ENABLE_ALARM
        if (gAlarmState == ALARM_STATE_TXALARM)
            BK4819_TransmitTone(true, 500);

        gAlarmToneCounter = 0;
#endif

        SYSTEM_DelayMs(2);
        AUDIO_AudioPathOn();
        gEnableSpeaker = true;

        return;
    }
#endif

//    if (gSetting_backlight_on_tx_rx & BACKLIGHT_ON_TR_TX) {
    BACKLIGHT_TurnOn();
//    }

}


void FUNCTION_Select(FUNCTION_Type_t Function) {
    const FUNCTION_Type_t PreviousFunction = gCurrentFunction;
    const bool bWasPowerSave = PreviousFunction == FUNCTION_POWER_SAVE;

    gCurrentFunction = Function;

    if (bWasPowerSave && Function != FUNCTION_POWER_SAVE) {
        BK4819_Conditional_RX_TurnOn_and_GPIO6_Enable();
        gRxIdleMode = false;
        UI_DisplayStatus();
    }

    switch (Function) {
        case FUNCTION_FOREGROUND:
            FUNCTION_Foreground(PreviousFunction);
            return;

        case FUNCTION_POWER_SAVE:
            FUNCTION_PowerSave();
            return;

        case FUNCTION_TRANSMIT:
            FUNCTION_Transmit();
            break;

        case FUNCTION_MONITOR:
            gMonitor = true;
            break;

        case FUNCTION_INCOMING:
        case FUNCTION_RECEIVE:
        case FUNCTION_BAND_SCOPE:
        default:
            break;
    }

    gBatterySaveCountdown_10ms = battery_save_count_10ms;
    gSchedulePowerSave = false;

#if defined(ENABLE_FMRADIO)
    gFM_RestoreCountdown_10ms = 0;
#endif
}
