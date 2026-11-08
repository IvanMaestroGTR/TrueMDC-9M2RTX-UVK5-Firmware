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
#include <stdio.h>
#include "app/mdc1200.h"
#ifdef ENABLE_FLEETSYNC
#include "app/fleetsync.h"
#endif
#include <string.h>
#include "driver/uart.h"
#include "ui/helper.h"

#if !defined(ENABLE_OVERLAY)

#include "ARMCM0.h"

#endif

#include "app/dtmf.h"
#include "app/generic.h"
#include "app/menu.h"
#include "app/scanner.h"
#include "audio.h"
#include "board.h"
#include "bsp/dp32g030/gpio.h"
#include "driver/backlight.h"
#include "driver/bk4819.h"
#include "driver/eeprom.h"
#include "driver/gpio.h"
#include "driver/keyboard.h"
#include "frequencies.h"
#include "helper/battery.h"
#include "misc.h"
#include "radio.h"
#include "settings.h"

#if defined(ENABLE_OVERLAY)
#include "sram-overlay.h"
#endif

#include "ui/inputbox.h"
#include "ui/menu.h"
#include "ui/menu.h"
#include "ui/ui.h"

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(x) (sizeof(x) / sizeof(x[0]))
#endif

static uint32_t MENU_GetTxPowerCalibrationAddress(void)
{
    const uint32_t band = FREQUENCY_GetBand(gTxVfo->pTX->Frequency);
    const uint32_t power = (gTxVfo->OUTPUT_POWER == OUTPUT_POWER_LOW1) ? OUTPUT_POWER_LOW : gTxVfo->OUTPUT_POWER;
    const uint32_t lower = frequencyBandTable[band].lower;
    const uint32_t upper = frequencyBandTable[band].upper;
    const uint32_t frequency = gTxVfo->pTX->Frequency;
    const uint32_t point = frequency < lower + ((upper - lower) / 4) ? 0 :
                            frequency > lower + (3 * (upper - lower) / 4) ? 2 : 1;

    return 0x1ED0 + (band * 16) + (power * 3) + point;
}

//uint8_t gUnlockAllTxConfCnt;

#ifdef ENABLE_F_CAL_MENU
void writeXtalFreqCal(const int32_t value, const bool update_eeprom)
{
    BK4819_WriteRegister(BK4819_REG_3B, 22656 + value);

    if (update_eeprom)
    {
        struct
        {
            int16_t  BK4819_XtalFreqLow;
            uint16_t EEPROM_1F8A;
            uint16_t EEPROM_1F8C;
            uint8_t  VOLUME_GAIN;
            uint8_t  DAC_GAIN;
        } __attribute__((packed)) misc;

        gEeprom.BK4819_XTAL_FREQ_LOW = value;

        // radio 1 .. 04 00 46 00 50 00 2C 0E
        // radio 2 .. 05 00 46 00 50 00 2C 0E
        //
        EEPROM_ReadBuffer(0x1F88, &misc, 8);
        misc.BK4819_XtalFreqLow = value;
        EEPROM_WriteBuffer(0x1F88, &misc,8);
    }
}
#endif


void MENU_StartCssScan(void) {
    SCANNER_Start(true);
    gUpdateStatus = true;
    gCssBackgroundScan = true;

    gRequestDisplayScreen = DISPLAY_MENU;
}
void MENU_CssScanFound(void) {
    if (gScanCssResultType == CODE_TYPE_DIGITAL || gScanCssResultType == CODE_TYPE_REVERSE_DIGITAL) {
        gMenuCursor = UI_MENU_GetMenuIdx(MENU_R_DCS);
    } else if (gScanCssResultType == CODE_TYPE_CONTINUOUS_TONE) {
        gMenuCursor = UI_MENU_GetMenuIdx(MENU_R_CTCS);
    }

    MENU_ShowCurrentSetting();

    gUpdateStatus = true;
    gUpdateDisplay = true;
}

void MENU_StopCssScan(void) {
    gCssBackgroundScan = false;

#ifdef ENABLE_VOICE
    gAnotherVoiceID       = VOICE_ID_SCANNING_STOP;
#endif
    gUpdateDisplay = true;
    gUpdateStatus = true;
}

int MENU_GetLimits(uint8_t menu_id, int32_t *pMin, int32_t *pMax) {
    switch (menu_id) {
        case MENU_SQL:
            *pMin = 0;
            *pMax = 9;
            break;

        case MENU_STEP:
            *pMin = 0;
            *pMax = STEP_N_ELEM - 1;
            break;

        case MENU_ABR:
            *pMin = 0;
            *pMax = ARRAY_SIZE(gSubMenu_BACKLIGHT) - 1;
            break;

//		case MENU_ABR_MIN:
//			*pMin = 0;
//			*pMax = 9;
//			break;

        case MENU_ABR_MAX:
            *pMin = 1;
            *pMax = 10;
            break;

        case MENU_F_LOCK:
            *pMin = 0;
            *pMax = ARRAY_SIZE(gSubMenu_F_LOCK) - 1;
            break;

        case MENU_MDF:
            *pMin = 0;
            *pMax = ARRAY_SIZE(gSubMenu_MDF) - 1;
            break;

//		case MENU_TXP:
//			*pMin = 0;
//			*pMax = ARRAY_SIZE(gSubMenu_TXP) - 1;
//			break;

        case MENU_SFT_D:
            *pMin = 0;
            *pMax = ARRAY_SIZE(gSubMenu_SFT_D) - 1;
            break;

        case MENU_TDR:
            *pMin = 0;
            *pMax = ARRAY_SIZE(gSubMenu_RXMode) - 1;
            break;

#ifdef ENABLE_VOICE
            case MENU_VOICE:
                *pMin = 0;
                *pMax = ARRAY_SIZE(gSubMenu_VOICE) - 1;
                break;
#endif

        case MENU_PTT_ID:
            *pMin = 0;
            *pMax = ARRAY_SIZE(gSubMenu_PTT_ID) - 1;
            break;

        case MENU_SC_REV:
            *pMin = 0;
            *pMax = ARRAY_SIZE(gSubMenu_SC_REV) - 1;
            break;

#ifdef ENABLE_MDC1200
        case MENU_MDC_PREAMBLE_DURATION:
            *pMin = 0;
            *pMax = 7;  // 0-7 for preamble options (OFF, 1, 3, 4, 5, 6, 7, 8)
            break;

        case MENU_MDC_PREAMBLE_WHEN:
            *pMin = 0;
            *pMax = ARRAY_SIZE(gSubMenu_MDC_PREAMBLE_WHEN) - 1;
            break;
#endif

        case MENU_R_DCS:
        case MENU_T_DCS:
            *pMin = 0;
            *pMax = 208;
            //*pMax = (104 * 2);
            break;

        case MENU_R_CTCS:
        case MENU_T_CTCS:
            *pMin = 0;
            *pMax = 50;
            break;

        case MENU_STE:
            *pMin = 0;
            *pMax = ARRAY_SIZE(gSubMenu_STE) - 1;
            break;

#ifdef ENABLE_CUSTOM_SIDEFUNCTIONS

            case MENU_W_N:
                *pMin = 0;
                *pMax = ARRAY_SIZE(gSubMenu_W_N) - 1;
                break;
#endif
#ifdef ENABLE_ALARM
            case MENU_AL_MOD:
                *pMin = 0;
                *pMax = ARRAY_SIZE(gSubMenu_AL_MOD) - 1;
                break;
#endif

        case MENU_RESET:
            *pMin = 0;
            *pMax = ARRAY_SIZE(gSubMenu_RESET) - 1;
            break;

        case MENU_COMPAND:
//		case MENU_ABR_ON_TX_RX:
            *pMin = 0;
            *pMax = ARRAY_SIZE(gSubMenu_RX_TX) - 1;
            break;

#ifdef ENABLE_AM_FIX_TEST1
            case MENU_AM_FIX_TEST1:
                *pMin = 0;
                *pMax = ARRAY_SIZE(gSubMenu_AM_fix_test1) - 1;
                break;
#endif

#ifdef ENABLE_F_CAL_MENU
            case MENU_F_CALI:
                *pMin = -50;
                *pMax = +50;
                break;
#endif

        case MENU_BATCAL:
            *pMin = 1600;
            *pMax = 2200;
            break;

        case MENU_TX_CAL:
            *pMin = 0;
            *pMax = 150;
            break;

        case MENU_BATTYP:
            *pMin = 0;
            *pMax = 1;
            break;

        case MENU_RX_LM:
            *pMin = 0;
            *pMax = 2;  // 0=OFF, 1=SOLID, 2=BLINK
            break;

        case MENU_TALK_PERMIT_TONE:
            *pMin = TALK_PERMIT_TONE_OFF;
            *pMax = TALK_PERMIT_TONE_AUTO;
            break;

#ifdef ENABLE_CUSTOM_SIDEFUNCTIONS
            case MENU_F1SHRT:
            case MENU_F2SHRT:
                *pMin = 0;

#ifdef ENABLE_SIDEFUNCTIONS_SEND
                *pMax = gSubMenu_SIDEFUNCTIONS_size-3;

#else
                *pMax =gSubMenu_SIDEFUNCTIONS_size-1;

#endif
                 break;
            case MENU_F1LONG:
            case MENU_F2LONG:
            case MENU_MLONG:
                *pMin = 0;


                *pMax = gSubMenu_SIDEFUNCTIONS_size-1;

                break;
#endif

        default:
            return -1;
    }

    return 0;
}

void MENU_AcceptSetting(void) {
    int32_t Min;
    int32_t Max;
    FREQ_Config_t *pConfig = &gTxVfo->freq_config_RX;

    if (!MENU_GetLimits(UI_MENU_GetCurrentMenuId(), &Min, &Max)) {
        if (gSubMenuSelection < Min) gSubMenuSelection = Min;
        else if (gSubMenuSelection > Max) gSubMenuSelection = Max;
    }
    char a = gSubMenuSelection;//UART_Send(a,1);
    UART_Send((uint8_t *) &a, 1);

    switch (UI_MENU_GetCurrentMenuId()) {
        default:
            return;

        case MENU_SQL:
            gEeprom.SQUELCH_LEVEL = gSubMenuSelection;
            gVfoConfigureMode = VFO_CONFIGURE;
            break;

        case MENU_STEP:
            gTxVfo->STEP_SETTING = FREQUENCY_GetStepIdxFromSortedIdx(gSubMenuSelection);
            if (IS_FREQ_CHANNEL(gTxVfo->CHANNEL_SAVE)) {
                gRequestSaveChannel = 1;
                return;
            }
            return;

//		case MENU_TXP:
//			gTxVfo->OUTPUT_POWER = gSubMenuSelection;
//			gRequestSaveChannel = 1;
//			return;

        case MENU_T_DCS:
            pConfig = &gTxVfo->freq_config_TX;

            // Fallthrough

        case MENU_R_DCS: {
            if (gSubMenuSelection == 0) {
                if (pConfig->CodeType == CODE_TYPE_CONTINUOUS_TONE) {
                    return;
                }
                pConfig->Code = 0;
                pConfig->CodeType = CODE_TYPE_OFF;
            } else if (gSubMenuSelection < 105) {
                pConfig->CodeType = CODE_TYPE_DIGITAL;
                pConfig->Code = gSubMenuSelection - 1;
            } else {
                pConfig->CodeType = CODE_TYPE_REVERSE_DIGITAL;
                pConfig->Code = gSubMenuSelection - 105;
            }

            gRequestSaveChannel = 1;
            return;
        }
        case MENU_T_CTCS:
            pConfig = &gTxVfo->freq_config_TX;
            [[fallthrough]];
        case MENU_R_CTCS: {
            if (gSubMenuSelection == 0) {
                if (pConfig->CodeType != CODE_TYPE_CONTINUOUS_TONE) {
                    return;
                }
                pConfig->Code = 0;
                pConfig->CodeType = CODE_TYPE_OFF;

            } else {
                pConfig->Code = gSubMenuSelection - 1;
                pConfig->CodeType = CODE_TYPE_CONTINUOUS_TONE;

            }

            gRequestSaveChannel = 1;
            return;
        }

        case MENU_SFT_D:
            gTxVfo->TX_OFFSET_FREQUENCY_DIRECTION = gSubMenuSelection;
            gRequestSaveChannel = 1;
            return;

        case MENU_OFFSET:
            gTxVfo->TX_OFFSET_FREQUENCY = gSubMenuSelection;
            gRequestSaveChannel = 1;
            return;
#ifdef ENABLE_CUSTOM_SIDEFUNCTIONS

            case MENU_W_N:
                gTxVfo->CHANNEL_BANDWIDTH = gSubMenuSelection;
                gRequestSaveChannel       = 1;
                return;
#endif
        case MENU_BCL:
            gTxVfo->BUSY_CHANNEL_LOCK = gSubMenuSelection;
            gRequestSaveChannel = 1;
            return;

        case MENU_MEM_CH:
            gTxVfo->CHANNEL_SAVE = gSubMenuSelection;
#if 0
            gEeprom.MrChannel[0] = gSubMenuSelection;
#else
            gEeprom.MrChannel[gEeprom.TX_VFO] = gSubMenuSelection;
#endif
            gRequestSaveChannel = 2;
            gVfoConfigureMode = VFO_CONFIGURE_RELOAD;
            gFlagResetVfos = true;
            return;
        case MENU_AX25_CALLSIGN:
            for (unsigned int i = 0; i < 6; ++i) {
                char c = edit[i];
                if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')))
                    c = ' ';
                gEeprom.AX25_CALLSIGN[i] = c;
            }
            gEeprom.AX25_CALLSIGN[6] = '\0';
            gEeprom.AX25_SSID = (edit[7] >= '0' && edit[7] <= '9') ?
                                (uint8_t)(edit[7] - '0') :
                                (uint8_t)(edit[7] - 'A' + 10);
            gRequestSaveSettings = true;
            return;
#ifdef ENABLE_MDC1200
            case MENU_MDC_ID:
#ifdef ENABLE_MDC1200_EDIT
        gEeprom.MDC1200_ID=extractHex(edit);
#endif

            return;
#endif
#ifdef ENABLE_FLEETSYNC
        case MENU_FLEETSYNC_UNIT:
            gEeprom.FLEETSYNC_FLEET = (uint16_t)((edit[0] - '0') * 100u +
                                                 (edit[1] - '0') * 10u +
                                                 (edit[2] - '0'));
            gEeprom.FLEETSYNC_UNIT = (uint16_t)((edit[4] - '0') * 1000u +
                                                (edit[5] - '0') * 100u +
                                                (edit[6] - '0') * 10u +
                                                (edit[7] - '0'));
            if (gEeprom.FLEETSYNC_FLEET < FLEETSYNC_FLEET_MIN)
                gEeprom.FLEETSYNC_FLEET = FLEETSYNC_FLEET_MIN;
            else if (gEeprom.FLEETSYNC_FLEET > FLEETSYNC_FLEET_MAX)
                gEeprom.FLEETSYNC_FLEET = FLEETSYNC_FLEET_MAX;
            if (gEeprom.FLEETSYNC_UNIT < FLEETSYNC_UNIT_MIN)
                gEeprom.FLEETSYNC_UNIT = FLEETSYNC_UNIT_MIN;
            else if (gEeprom.FLEETSYNC_UNIT > FLEETSYNC_UNIT_MAX)
                gEeprom.FLEETSYNC_UNIT = FLEETSYNC_UNIT_MAX;
            break;
#endif
        case MENU_MEM_NAME: //���뷨
            // trailing trim
            for (int i = MAX_EDIT_INDEX - 1; i >= 0; i--) {
                if (edit[i] != ' ' && edit[i] != '_' && edit[i] != 0x00 && edit[i] != 0xff)
                    break;
                edit[i] = 0;
            }

            SETTINGS_SaveChannelName(gSubMenuSelection, edit);
            return;

        case MENU_SAVE:
            gEeprom.BATTERY_SAVE = gSubMenuSelection;
            break;

#ifdef ENABLE_VOX
            //			case MENU_VOX:
            //				gEeprom.VOX_SWITCH = gSubMenuSelection != 0;
            //				if (gEeprom.VOX_SWITCH)
            //					gEeprom.VOX_LEVEL = gSubMenuSelection - 1;
            //				SETTINGS_LoadCalibration();
            //				gFlagReconfigureVfos = true;
            //				gUpdateStatus        = true;
            //				break;
#endif

        case MENU_ABR:
            gEeprom.BACKLIGHT_TIME = gSubMenuSelection;
            break;

//		case MENU_ABR_MIN:
//			gEeprom.BACKLIGHT_MIN = gSubMenuSelection;
//			gEeprom.BACKLIGHT_MAX = MAX(gSubMenuSelection + 1 , gEeprom.BACKLIGHT_MAX);
//			break;

        case MENU_ABR_MAX:
            gEeprom.BACKLIGHT_MAX = gSubMenuSelection;
            break;

//		case MENU_ABR_ON_TX_RX:
//			gSetting_backlight_on_tx_rx = gSubMenuSelection;
//			break;

        case MENU_TDR:
            gEeprom.DUAL_WATCH = (gEeprom.TX_VFO + 1) * (gSubMenuSelection & 1);
            gEeprom.CROSS_BAND_RX_TX = (gEeprom.TX_VFO + 1) * ((gSubMenuSelection & 2) > 0);

            gFlagReconfigureVfos = true;
            gUpdateStatus = true;
            break;

//		case MENU_BEEP:
//			gEeprom.BEEP_CONTROL = gSubMenuSelection;
//			break;

        case MENU_TOT:
            gEeprom.TX_TIMEOUT_TIMER = gSubMenuSelection;
            break;

#ifdef ENABLE_VOICE
            case MENU_VOICE:
                gEeprom.VOICE_PROMPT = gSubMenuSelection;
                gUpdateStatus        = true;
                break;
#endif

        case MENU_SC_REV:
            gEeprom.SCAN_RESUME_MODE = gSubMenuSelection;
            break;

        case MENU_MDF:
            gEeprom.CHANNEL_DISPLAY_MODE = gSubMenuSelection;
            break;

//		case MENU_AUTOLK:
//			gEeprom.AUTO_KEYPAD_LOCK = gSubMenuSelection;
//			gKeyLockCountdown        = 30;
//			break;

//		case MENU_S_ADD1:
//			gTxVfo->SCANLIST1_PARTICIPATION = gSubMenuSelection;
//			SETTINGS_UpdateChannel(gTxVfo->CHANNEL_SAVE, gTxVfo, true);
//			gVfoConfigureMode = VFO_CONFIGURE;
//			gFlagResetVfos    = true;
//			return;

//		case MENU_S_ADD2:
//			gTxVfo->SCANLIST2_PARTICIPATION = gSubMenuSelection;
//			SETTINGS_UpdateChannel(gTxVfo->CHANNEL_SAVE, gTxVfo, true);
//			gVfoConfigureMode = VFO_CONFIGURE;
//			gFlagResetVfos    = true;
//			return;

        case MENU_STE:
            gEeprom.TAIL_TONE_ELIMINATION = gSubMenuSelection;
            break;

case MENU_RP_STE:
#ifdef ENABLE_FLEETSYNC
            if (gEeprom.PTT_ID == PTT_ID_MODE_FLEETSYNC_PRE ||
                gEeprom.PTT_ID == PTT_ID_MODE_FLEETSYNC_POST ||
                gEeprom.PTT_ID == PTT_ID_MODE_FLEETSYNC_BOTH) {
                gEeprom.FLEETSYNC_PTT_ID_DELAY = gSubMenuSelection;
            } else {
                gEeprom.REPEATER_TAIL_TONE_ELIMINATION = gSubMenuSelection;
            }
#else
            gEeprom.REPEATER_TAIL_TONE_ELIMINATION = gSubMenuSelection;
#endif
            break;

        case MENU_MIC:
            gEeprom.MIC_SENSITIVITY = gSubMenuSelection;
            SETTINGS_LoadCalibration();
            gFlagReconfigureVfos = true;
            break;

//#ifdef ENABLE_AUDIO_BAR
            //			case MENU_MIC_BAR:
            //				gSetting_mic_bar = gSubMenuSelection;
            //				break;
//#endif

        case MENU_COMPAND:
            gTxVfo->Compander = gSubMenuSelection;
            SETTINGS_UpdateChannel(gTxVfo->CHANNEL_SAVE, gTxVfo, true);
            gVfoConfigureMode = VFO_CONFIGURE;
            gFlagResetVfos = true;
//			gRequestSaveChannel = 1;
            return;

        case MENU_1_CALL:
            gEeprom.CHAN_1_CALL = gSubMenuSelection;
            break;

        case MENU_S_LIST:
            gEeprom.SCAN_LIST_DEFAULT = gSubMenuSelection;
            break;

#ifdef ENABLE_ALARM
            case MENU_AL_MOD:
                gEeprom.ALARM_MODE = gSubMenuSelection;
                break;
#endif

        case MENU_D_ST:
            gEeprom.DTMF_SIDE_TONE = gSubMenuSelection;
            break;
#ifdef ENABLE_DTMF_CALLING
            // case MENU_D_RSP: // DTMF Decode Response disabled
            //     gEeprom.DTMF_DECODE_RESPONSE = gSubMenuSelection;
            //     break;

            // case MENU_D_HOLD: // DTMF Auto Reset disabled
            //     gEeprom.DTMF_auto_reset_time = gSubMenuSelection;
            //     break;
#endif

        case MENU_PTT_ID:
            gEeprom.PTT_ID = (gSubMenuSelection < ARRAY_SIZE(gSubMenu_PTT_ID))
                           ? (PTT_ID_Mode_t)gSubMenuSelection
                           : PTT_ID_MODE_OFF;
            gRequestSaveSettings = true;
            break;

//		case MENU_BAT_TXT:
//			gSetting_battery_text = gSubMenuSelection;
//			break;
#ifdef ENABLE_DTMF_CALLING
#ifdef ENABLE_CUSTOM_SIDEFUNCTIONS
            // case MENU_D_DCD: // DTMF DCD disabled
            //     gTxVfo->DTMF_DECODING_ENABLE = gSubMenuSelection;
            //     DTMF_clear_RX();
            //     gRequestSaveChannel = 1;
            //     return;
#endif
#endif
        // case MENU_D_LIVE_DEC: // Live DTMF decoder disabled
        //     gSetting_live_DTMF_decoder = gSubMenuSelection;
        //     gDTMF_RX_live_timeout = 0;
        //     memset(gDTMF_RX_live, 0, sizeof(gDTMF_RX_live));
        //     if (!gSetting_live_DTMF_decoder)
        //         BK4819_DisableDTMF();
        //     gFlagReconfigureVfos = true;
        //     gUpdateStatus = true;
        //     break;
#ifdef ENABLE_DTMF_CALLING
            // case MENU_D_LIST: // DTMF Contact List disabled
            //     gDTMF_chosen_contact = gSubMenuSelection - 1;
            //     if (gIsDtmfContactValid)
            //     {
            //         GUI_SelectNextDisplay(DISPLAY_MAIN);
            //         gDTMF_InputMode       = true;
            //         gDTMF_InputBox_Index  = 3;
            //         memcpy(gDTMF_InputBox, gDTMF_ID, 4);
            //         gRequestDisplayScreen = DISPLAY_INVALID;
            //     }
            //     return;
#endif
#ifdef ENABLE_MDC1200
        case MENU_MDC_PREAMBLE_DURATION:
            // Menu index to preamble value mapping (skip 1 and 9)
            // Index 0->0, 1->2, 2->3, 3->4, 4->5, 5->6, 6->7, 7->8
            uint8_t map_index[] = {0, 2, 3, 4, 5, 6, 7, 8};
            gEeprom.MDC1200_PREAMBLE_DURATION = (gSubMenuSelection < 8) ? map_index[gSubMenuSelection] : 0;
            break;

        case MENU_MDC_PREAMBLE_WHEN:
            gEeprom.MDC1200_PREAMBLE_WHEN = gSubMenuSelection;
            break;
#endif

//		case MENU_AM:
//			gTxVfo->Modulation     = gSubMenuSelection;
//			gRequestSaveChannel = 1;
//			return;


#ifdef ENABLE_AM_FIX_TEST1
            case MENU_AM_FIX_TEST1:
                gSetting_AM_fix_test1 = gSubMenuSelection;
                gVfoConfigureMode = VFO_CONFIGURE_RELOAD;
                gFlagResetVfos    = true;
                break;
#endif

#ifdef ENABLE_NOAA
            case MENU_NOAA_S:
                gEeprom.NOAA_AUTO_SCAN = gSubMenuSelection;
                gFlagReconfigureVfos   = true;
                break;
#endif

        case MENU_DEL_CH:
            SETTINGS_UpdateChannel(gSubMenuSelection, NULL, false);
            gVfoConfigureMode = VFO_CONFIGURE_RELOAD;
            gFlagResetVfos = true;
            return;

        case MENU_RESET:
            SETTINGS_FactoryReset(gSubMenuSelection);
            return;

//		case MENU_350TX:
//			gSetting_350TX = gSubMenuSelection;
//			break;

        case MENU_F_LOCK: {
//			if(gSubMenuSelection == F_LOCK_NONE) { // select 10 times to enable
//				gUnlockAllTxConfCnt++;
//				if(gUnlockAllTxConfCnt < 10)
//					return;
//			}
//			else
//				gUnlockAllTxConfCnt = 0;

            gSetting_F_LOCK = gSubMenuSelection;
            break;
        }
//		case MENU_200TX:
//			gSetting_200TX = gSubMenuSelection;
//			break;
//
//		case MENU_500TX:
//			gSetting_500TX = gSubMenuSelection;
//			break;

//		case MENU_350EN:
//			gSetting_350EN       = gSubMenuSelection;
//			gVfoConfigureMode    = VFO_CONFIGURE_RELOAD;
//			gFlagResetVfos       = true;
//			break;

#ifdef ENABLE_F_CAL_MENU
            case MENU_F_CALI:
                writeXtalFreqCal(gSubMenuSelection, true);
                return;
#endif

        case MENU_BATCAL: {                                                                 // voltages are averages between discharge curves of 1600 and 2200 mAh
            // gBatteryCalibration[0] = (520ul * gSubMenuSelection) / 760;  // 5.20V empty, blinking above this value, reduced functionality below
            // gBatteryCalibration[1] = (689ul * gSubMenuSelection) / 760;  // 6.89V,  ~5%, 1 bars above this value
            // gBatteryCalibration[2] = (724ul * gSubMenuSelection) / 760;  // 7.24V, ~17%, 2 bars above this value
            gBatteryCalibration[3] = gSubMenuSelection;         // 7.6V,  ~29%, 3 bars above this value
            // gBatteryCalibration[4] = (771ul * gSubMenuSelection) / 760;  // 7.71V, ~65%, 4 bars above this value
            // gBatteryCalibration[5] = 2300;
            SETTINGS_SaveBatteryCalibration(gBatteryCalibration);
            return;
        }

        case MENU_TX_CAL: {
            const uint8_t value = (uint8_t)gSubMenuSelection;
            EEPROM_WriteBuffer(MENU_GetTxPowerCalibrationAddress(), &value, 1);
            RADIO_ConfigureSquelchAndOutputPower(gTxVfo);
            return;
        }

        case MENU_BATTYP:
            gEeprom.BATTERY_TYPE = gSubMenuSelection;
            break;

        case MENU_RX_LM:
            gEeprom.RX_LIGHT_MODE = gSubMenuSelection;
            break;

        case MENU_TALK_PERMIT_TONE:
            gEeprom.field38_0x33 = gSubMenuSelection;
            break;

#ifdef ENABLE_CUSTOM_SIDEFUNCTIONS
            case MENU_F1SHRT:
            case MENU_F1LONG:
            case MENU_F2SHRT:
            case MENU_F2LONG:
            case MENU_MLONG:
                {
                    uint8_t * fun[]= {
                        &gEeprom.KEY_1_SHORT_PRESS_ACTION,
                        &gEeprom.KEY_1_LONG_PRESS_ACTION,
                        &gEeprom.KEY_2_SHORT_PRESS_ACTION,
                        &gEeprom.KEY_2_LONG_PRESS_ACTION,
                        &gEeprom.KEY_M_LONG_PRESS_ACTION};
                    *fun[UI_MENU_GetCurrentMenuId()-MENU_F1SHRT] = gSubMenu_SIDEFUNCTIONS[gSubMenuSelection].id;
                }
                break;
#endif

    }

    gRequestSaveSettings = true;
}

static void MENU_ClampSelection(int8_t Direction) {
    int32_t Min;
    int32_t Max;

    if (!MENU_GetLimits(UI_MENU_GetCurrentMenuId(), &Min, &Max)) {
        int32_t Selection = gSubMenuSelection;
        if (Selection < Min) Selection = Min;
        else if (Selection > Max) Selection = Max;
        gSubMenuSelection = NUMBER_AddWithWraparound(Selection, Direction, Min, Max);
    }
}

void MENU_ShowCurrentSetting(void) {
    switch (UI_MENU_GetCurrentMenuId()) {
        case MENU_SQL:
            gSubMenuSelection = gEeprom.SQUELCH_LEVEL;
            break;

case MENU_STEP:
            gSubMenuSelection = FREQUENCY_GetSortedIdxFromStepIdx(gTxVfo->STEP_SETTING);
            break;

//		case MENU_TXP:
//			gSubMenuSelection = gTxVfo->OUTPUT_POWER;
//			break;

        case MENU_RESET:
            gSubMenuSelection = 0;
            break;

        case MENU_R_DCS:
        case MENU_R_CTCS: {
            DCS_CodeType_t type = gTxVfo->freq_config_RX.CodeType;
            uint8_t code = gTxVfo->freq_config_RX.Code;
            int menuid = UI_MENU_GetCurrentMenuId();

            if (gScanUseCssResult) {
                gScanUseCssResult = false;
                type = gScanCssResultType;
                code = gScanCssResultCode;
            }
            if ((menuid == MENU_R_CTCS) ^ (type == CODE_TYPE_CONTINUOUS_TONE)) { //not the same type
                gSubMenuSelection = 0;
                break;
            }

            switch (type) {
                case CODE_TYPE_CONTINUOUS_TONE:
                case CODE_TYPE_DIGITAL:
                    gSubMenuSelection = code + 1;
                    break;
                case CODE_TYPE_REVERSE_DIGITAL:
                    gSubMenuSelection = code + 105;
                    break;
                default:
                    gSubMenuSelection = 0;
                    break;
            }
            break;
        }

        case MENU_T_DCS:
            switch (gTxVfo->freq_config_TX.CodeType) {
                case CODE_TYPE_DIGITAL:
                    gSubMenuSelection = gTxVfo->freq_config_TX.Code + 1;
                    break;
                case CODE_TYPE_REVERSE_DIGITAL:
                    gSubMenuSelection = gTxVfo->freq_config_TX.Code + 105;
                    break;
                default:
                    gSubMenuSelection = 0;
                    break;
            }
            break;

        case MENU_T_CTCS:
            gSubMenuSelection = (gTxVfo->freq_config_TX.CodeType == CODE_TYPE_CONTINUOUS_TONE) ?
                                gTxVfo->freq_config_TX.Code + 1 : 0;
            break;

        case MENU_SFT_D:
            gSubMenuSelection = gTxVfo->TX_OFFSET_FREQUENCY_DIRECTION;
            break;

        case MENU_OFFSET:
            gSubMenuSelection = gTxVfo->TX_OFFSET_FREQUENCY;
            break;
#ifdef ENABLE_CUSTOM_SIDEFUNCTIONS

        case MENU_W_N:
            gSubMenuSelection = gTxVfo->CHANNEL_BANDWIDTH;
            break;
#endif
        case MENU_BCL:
            gSubMenuSelection = gTxVfo->BUSY_CHANNEL_LOCK;
            break;

        case MENU_MEM_CH:
#if 0
            gSubMenuSelection = gEeprom.MrChannel[0];
#else
            gSubMenuSelection = gEeprom.MrChannel[gEeprom.TX_VFO];
#endif
            break;

        case MENU_MEM_NAME:
            gSubMenuSelection = gEeprom.MrChannel[gEeprom.TX_VFO];
            break;

        case MENU_SAVE:
            gSubMenuSelection = gEeprom.BATTERY_SAVE;
            break;

#ifdef ENABLE_VOX
            //		case MENU_VOX:
            //			gSubMenuSelection = gEeprom.VOX_SWITCH ? gEeprom.VOX_LEVEL + 1 : 0;
            //			break;
#endif

        case MENU_ABR:
            gSubMenuSelection = gEeprom.BACKLIGHT_TIME;
            break;

//		case MENU_ABR_MIN:
//			gSubMenuSelection = gEeprom.BACKLIGHT_MIN;
//			break;

        case MENU_ABR_MAX:
            gSubMenuSelection = gEeprom.BACKLIGHT_MAX;
            break;

//		case MENU_ABR_ON_TX_RX:
//			gSubMenuSelection = gSetting_backlight_on_tx_rx;
//			break;

        case MENU_TDR:
            gSubMenuSelection =
                    (gEeprom.DUAL_WATCH != DUAL_WATCH_OFF) + (gEeprom.CROSS_BAND_RX_TX != CROSS_BAND_OFF) * 2;
            break;

//		case MENU_BEEP:
//			gSubMenuSelection = gEeprom.BEEP_CONTROL;
//			break;

        case MENU_TOT:
            gSubMenuSelection = gEeprom.TX_TIMEOUT_TIMER;
            break;

#ifdef ENABLE_VOICE
            case MENU_VOICE:
                gSubMenuSelection = gEeprom.VOICE_PROMPT;
                break;
#endif

        case MENU_SC_REV:
            gSubMenuSelection = gEeprom.SCAN_RESUME_MODE;
            break;

        case MENU_MDF:
            gSubMenuSelection = gEeprom.CHANNEL_DISPLAY_MODE;
            break;

//		case MENU_AUTOLK:
//			gSubMenuSelection = gEeprom.AUTO_KEYPAD_LOCK;
//			break;

//		case MENU_S_ADD1:
//			gSubMenuSelection = gTxVfo->SCANLIST1_PARTICIPATION;
//			break;
//
//		case MENU_S_ADD2:
//			gSubMenuSelection = gTxVfo->SCANLIST2_PARTICIPATION;
//			break;

        case MENU_STE:
            gSubMenuSelection = gEeprom.TAIL_TONE_ELIMINATION;
            break;

        case MENU_RP_STE:
#ifdef ENABLE_FLEETSYNC
            if (gEeprom.PTT_ID == PTT_ID_MODE_FLEETSYNC_PRE ||
                gEeprom.PTT_ID == PTT_ID_MODE_FLEETSYNC_POST ||
                gEeprom.PTT_ID == PTT_ID_MODE_FLEETSYNC_BOTH) {
                gSubMenuSelection = gEeprom.FLEETSYNC_PTT_ID_DELAY;
            } else {
                gSubMenuSelection = gEeprom.REPEATER_TAIL_TONE_ELIMINATION;
            }
#else
            gSubMenuSelection = gEeprom.REPEATER_TAIL_TONE_ELIMINATION;
#endif
            break;

        case MENU_MIC:
            gSubMenuSelection = gEeprom.MIC_SENSITIVITY;
            break;

//#ifdef ENABLE_AUDIO_BAR
            //		case MENU_MIC_BAR:
            //			gSubMenuSelection = gSetting_mic_bar;
            //			break;
//#endif

        case MENU_COMPAND:
            gSubMenuSelection = gTxVfo->Compander;
            return;

        case MENU_1_CALL:
            gSubMenuSelection = gEeprom.CHAN_1_CALL;
            break;

        case MENU_S_LIST:
            gSubMenuSelection = gEeprom.SCAN_LIST_DEFAULT;
            break;

        case MENU_SLIST1:
            gSubMenuSelection = RADIO_FindNextChannel(0, 1, true, 0);
            break;

        case MENU_SLIST2:
            gSubMenuSelection = RADIO_FindNextChannel(0, 1, true, 1);
            break;

#ifdef ENABLE_ALARM
            case MENU_AL_MOD:
                gSubMenuSelection = gEeprom.ALARM_MODE;
                break;
#endif

        case MENU_D_ST:
            gSubMenuSelection = gEeprom.DTMF_SIDE_TONE;
            break;
#ifdef ENABLE_DTMF_CALLING
        // case MENU_D_RSP: // DTMF Decode Response disabled
        //     gSubMenuSelection = gEeprom.DTMF_DECODE_RESPONSE;
        //     break;

        // case MENU_D_HOLD: // DTMF Auto Reset disabled
        //     gSubMenuSelection = gEeprom.DTMF_auto_reset_time;
        //     break;
#endif

        case MENU_PTT_ID:
            gSubMenuSelection = (gEeprom.PTT_ID < ARRAY_SIZE(gSubMenu_PTT_ID))
                              ? gEeprom.PTT_ID
                              : PTT_ID_MODE_OFF;
            break;

//		case MENU_BAT_TXT:
//			gSubMenuSelection = gSetting_battery_text;
//			return;
#ifdef ENABLE_DTMF_CALLING
#ifdef ENABLE_CUSTOM_SIDEFUNCTIONS
        // case MENU_D_DCD: // DTMF DCD disabled
        //     gSubMenuSelection = gTxVfo->DTMF_DECODING_ENABLE;
        //     break;
#endif
        // case MENU_D_LIST: // DTMF Contact List disabled
        //     gSubMenuSelection = gDTMF_chosen_contact + 1;
        //     break;
#endif
        // case MENU_D_LIVE_DEC: // Live DTMF decoder disabled
        //     gSubMenuSelection = gSetting_live_DTMF_decoder;
        //     break;
#ifdef ENABLE_MDC1200
        case MENU_MDC_PREAMBLE_DURATION: {
            // Preamble value to menu index mapping (skip 1 and 9)
            // 0->0, 2->1, 3->2, 4->3, 5->4, 6->5, 7->6, 8->7
            const uint8_t map_value[] = {0, 2, 3, 4, 5, 6, 7, 8};
            gSubMenuSelection = 0;  // Default to OFF
            for (int i = 0; i < 8; i++) {
                if (map_value[i] == gEeprom.MDC1200_PREAMBLE_DURATION) {
                    gSubMenuSelection = i;
                    break;
                }
            }
            break;
        }

        case MENU_MDC_PREAMBLE_WHEN:
            gSubMenuSelection = gEeprom.MDC1200_PREAMBLE_WHEN;
            break;
#endif

//		case MENU_AM:
//			gSubMenuSelection = gTxVfo->Modulation;
//			break;

#ifdef ENABLE_AM_FIX_TEST1
            case MENU_AM_FIX_TEST1:
                gSubMenuSelection = gSetting_AM_fix_test1;
                break;
#endif

#ifdef ENABLE_NOAA
            case MENU_NOAA_S:
                gSubMenuSelection = gEeprom.NOAA_AUTO_SCAN;
                break;
#endif

        case MENU_DEL_CH:
#if 0
            gSubMenuSelection = RADIO_FindNextChannel(gEeprom.MrChannel[0], 1, false, 1);
#else
            gSubMenuSelection = RADIO_FindNextChannel(gEeprom.MrChannel[gEeprom.TX_VFO], 1, false, 1);
#endif
            break;

//		case MENU_350TX:
//			gSubMenuSelection = gSetting_350TX;
//			break;

        case MENU_F_LOCK:
            gSubMenuSelection = gSetting_F_LOCK;
            break;

//		case MENU_200TX:
//			gSubMenuSelection = gSetting_200TX;
//			break;
//
//		case MENU_500TX:
//			gSubMenuSelection = gSetting_500TX;
//			break;
//
//		case MENU_350EN:
//			gSubMenuSelection = gSetting_350EN;
//			break;

#ifdef ENABLE_F_CAL_MENU
            case MENU_F_CALI:
                gSubMenuSelection = gEeprom.BK4819_XTAL_FREQ_LOW;
                break;
#endif

        case MENU_BATCAL:
            gSubMenuSelection = gBatteryCalibration[3];
            break;

        case MENU_TX_CAL: {
            uint8_t value;
            EEPROM_ReadBuffer(MENU_GetTxPowerCalibrationAddress(), &value, 1);
            gSubMenuSelection = value;
            break;
        }

        case MENU_BATTYP:
            gSubMenuSelection = gEeprom.BATTERY_TYPE;
            break;

        case MENU_RX_LM:
            gSubMenuSelection = gEeprom.RX_LIGHT_MODE;
            break;

        case MENU_TALK_PERMIT_TONE:
            gSubMenuSelection = gEeprom.field38_0x33 & 7;
            break;

#ifdef ENABLE_CUSTOM_SIDEFUNCTIONS
        case MENU_F1SHRT:
        case MENU_F1LONG:
        case MENU_F2SHRT:
        case MENU_F2LONG:
        case MENU_MLONG: {
            uint8_t *fun[] = {
                &gEeprom.KEY_1_SHORT_PRESS_ACTION,
                &gEeprom.KEY_1_LONG_PRESS_ACTION,
                &gEeprom.KEY_2_SHORT_PRESS_ACTION,
                &gEeprom.KEY_2_LONG_PRESS_ACTION,
                &gEeprom.KEY_M_LONG_PRESS_ACTION
            };
            uint8_t id = *fun[UI_MENU_GetCurrentMenuId() - MENU_F1SHRT];

            for (int i = 0; i < gSubMenu_SIDEFUNCTIONS_size; i++) {
                if (gSubMenu_SIDEFUNCTIONS[i].id == id) {
                    gSubMenuSelection = i;
                    break;
                }
            }
            break;
        }
#endif

        default:
            return;
    }
}

static void MENU_Key_0_to_9(KEY_Code_t Key, bool bKeyPressed, bool bKeyHeld) {
    uint8_t Offset;
    int32_t Min;
    int32_t Max;
    uint16_t Value = 0;

    if (bKeyHeld || !bKeyPressed)
        return;

    gBeepToPlay = BEEP_1KHZ_60MS_OPTIONAL;
    uint8_t now_menu = UI_MENU_GetCurrentMenuId();
#ifdef ENABLE_MDC1200_EDIT //���뷨����
    uint8_t end_index = now_menu == MENU_MEM_NAME ? MAX_EDIT_INDEX :
                        now_menu == MENU_AX25_CALLSIGN ? 8 :
                        now_menu == MENU_FLEETSYNC_UNIT ? 8 : 4;
#else
    uint8_t end_index = (now_menu == MENU_AX25_CALLSIGN) ? 8 : MAX_EDIT_INDEX;
#endif
    if ((now_menu == MENU_MEM_NAME //���뷨
#ifdef ENABLE_MDC1200
#ifdef ENABLE_MDC1200_EDIT
                || now_menu == MENU_MDC_ID
#endif
#endif
#ifdef ENABLE_FLEETSYNC
        || now_menu == MENU_FLEETSYNC_UNIT
#endif
        || now_menu == MENU_AX25_CALLSIGN
        ) &&
        edit_index >= 0) {    // currently editing the channel name

        if (edit_index < end_index) {
            if (Key <= KEY_9) {
                {
                    edit[edit_index] = '0' + Key ;
                    if (++edit_index >= end_index) {    // exit edit
                        //gFlagAcceptSetting = false;
                        gAskForConfirmation = 1;
                    }
                }

                gRequestDisplayScreen = DISPLAY_MENU;
            }
        }

        return;
    }

    INPUTBOX_Append(Key);

    gRequestDisplayScreen = DISPLAY_MENU;

    if (!gIsInSubMenu) {
        switch (gInputBoxIndex) {
            case 2:
                gInputBoxIndex = 0;

                Value = (gInputBox[0] * 10) + gInputBox[1];

                if (Value > 0 && Value <= gMenuListCount) {
                    gMenuCursor = Value - 1;
#ifndef ENABLE_MDC1200
                    if (gMenuCursor + 1 >= 26)gMenuCursor++;

#endif
                    gFlagRefreshSetting = true;
                    return;
                }

                if (Value <= gMenuListCount)
                    break;

                gInputBox[0] = gInputBox[1];
                gInputBoxIndex = 1;
                [[fallthrough]];
            case 1:
                Value = gInputBox[0];
                if (Value > 0 && Value <= gMenuListCount) {
                    gMenuCursor = Value - 1;
#ifndef ENABLE_MDC1200
                    if (gMenuCursor + 1 >= 26)gMenuCursor++;

#endif
                    gFlagRefreshSetting = true;
                    return;
                }
                break;
        }

        gInputBoxIndex = 0;

        gBeepToPlay = BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL;
        return;
    }

    if (UI_MENU_GetCurrentMenuId() == MENU_OFFSET) {
        uint32_t Frequency;

        if (gInputBoxIndex < 6) {    // invalid frequency
#ifdef ENABLE_VOICE
            gAnotherVoiceID = (VOICE_ID_t)Key;
#endif
            return;
        }

#ifdef ENABLE_VOICE
        gAnotherVoiceID = (VOICE_ID_t)Key;
#endif

        Frequency = StrToUL(INPUTBOX_GetAscii()) * 100;
        gSubMenuSelection = FREQUENCY_RoundToStep(Frequency, gTxVfo->StepFrequency);

        gInputBoxIndex = 0;
        return;
    }

    if (UI_MENU_GetCurrentMenuId() == MENU_MEM_CH ||
        UI_MENU_GetCurrentMenuId() == MENU_DEL_CH ||
        UI_MENU_GetCurrentMenuId() == MENU_1_CALL ||
        UI_MENU_GetCurrentMenuId() == MENU_MEM_NAME) {    // enter 1-3 digit channel number

        if (gInputBoxIndex < 3) {
#ifdef ENABLE_VOICE
            gAnotherVoiceID   = (VOICE_ID_t)Key;
#endif
            gRequestDisplayScreen = DISPLAY_MENU;
            return;
        }

        gInputBoxIndex = 0;

        Value = 0;
        if (gInputBox[0] >= 1 && gInputBox[0] <= 9) {
            if (gInputBox[1] == 10 && gInputBox[2] == 10) {
                // 1 digit input
                Value = gInputBox[0] - 1;
            } else if (gInputBox[1] >= 0 && gInputBox[1] <= 9 && gInputBox[2] == 10) {
                // 2 digit input
                Value = ((gInputBox[0] * 10) + gInputBox[1]) - 1;
            } else if (gInputBox[1] >= 0 && gInputBox[1] <= 9 && gInputBox[2] >= 0 && gInputBox[2] <= 9) {
                // 3 digit input
                Value = ((gInputBox[0] * 100) + (gInputBox[1] * 10) + gInputBox[2]) - 1;
            }
        }

        if (IS_MR_CHANNEL(Value)) {
#ifdef ENABLE_VOICE
            gAnotherVoiceID = (VOICE_ID_t)Key;
#endif
            gSubMenuSelection = Value;
            return;
        }

        gBeepToPlay = BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL;
        return;
    }

    if (MENU_GetLimits(UI_MENU_GetCurrentMenuId(), &Min, &Max)) {
        gInputBoxIndex = 0;
        gBeepToPlay = BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL;
        return;
    }

    Offset = (Max >= 100) ? 3 : (Max >= 10) ? 2 : 1;

    switch (gInputBoxIndex) {
        case 1:
            Value = gInputBox[0];
            break;
        case 2:
            Value = (gInputBox[0] * 10) + gInputBox[1];
            break;
        case 3:
            Value = (gInputBox[0] * 100) + (gInputBox[1] * 10) + gInputBox[2];
            break;
    }

    if (Offset == gInputBoxIndex)
        gInputBoxIndex = 0;

    if (Value <= Max) {
        gSubMenuSelection = Value;
        return;
    }

    gBeepToPlay = BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL;
}

static void MENU_Key_EXIT(bool bKeyPressed, bool bKeyHeld) {
    if (bKeyHeld || !bKeyPressed)
        return;
#ifdef ENABLE_FLEETSYNC
    if (UI_MENU_GetCurrentMenuId() == MENU_FLEETSYNC_UNIT && gIsInSubMenu && edit_index > 0 && gAskForConfirmation == 0) {
        if (edit_index == 4)
            edit_index = 2;
        else
            edit_index--;
        gRequestDisplayScreen = DISPLAY_MENU;
        return;
    }
#endif
    if ((UI_MENU_GetCurrentMenuId() == MENU_MEM_NAME ||
         UI_MENU_GetCurrentMenuId() == MENU_AX25_CALLSIGN) &&
        gIsInSubMenu == true && edit_index >= 0 && gAskForConfirmation == 0) {
        edit_index = -1;
        return;
    }
    gBeepToPlay = BEEP_1KHZ_60MS_OPTIONAL;

    if (!gCssBackgroundScan) {
        /* Backlight related menus set full brightness. Set it back to the configured value,
		   just in case we are exiting from one of them. */
        BACKLIGHT_TurnOn();
        if (gIsInSubMenu) {
            if (gInputBoxIndex == 0 || UI_MENU_GetCurrentMenuId() != MENU_OFFSET) {
                gAskForConfirmation = 0;
                gIsInSubMenu = false;
                gInputBoxIndex = 0;
                gFlagRefreshSetting = true;

#ifdef ENABLE_VOICE
                gAnotherVoiceID = VOICE_ID_CANCEL;
#endif
            } else
                gInputBox[--gInputBoxIndex] = 10;

            // ***********************

            gRequestDisplayScreen = DISPLAY_MENU;
            return;
        }

#ifdef ENABLE_VOICE
        gAnotherVoiceID = VOICE_ID_CANCEL;
#endif

        gRequestDisplayScreen = DISPLAY_MAIN;

        if (gEeprom.BACKLIGHT_TIME == 0) // backlight set to always off
        {
            BACKLIGHT_TurnOff();    // turn the backlight OFF
        }
    } else {
        MENU_StopCssScan();

#ifdef ENABLE_VOICE
        gAnotherVoiceID   = VOICE_ID_SCANNING_STOP;
#endif

        gRequestDisplayScreen = DISPLAY_MENU;
    }

    gPttWasReleased = true;
}
static void MENU_Key_MENU(const bool bKeyPressed, const bool bKeyHeld) {
    if (bKeyHeld || !bKeyPressed)
        return;
    gBeepToPlay = BEEP_1KHZ_60MS_OPTIONAL;
    gRequestDisplayScreen = DISPLAY_MENU;
    if (!gIsInSubMenu) {
#ifdef ENABLE_VOICE
        gAnotherVoiceID = MenuList[gMenuCursor].voice_id;
#endif
        if (UI_MENU_GetCurrentMenuId() == MENU_DEL_CH || UI_MENU_GetCurrentMenuId() == MENU_MEM_NAME)
//            if (!RADIO_CheckValidChannel(gSubMenuSelection, false, 0))
//                return;  // invalid channel
        {
            uint8_t before=gSubMenuSelection;
            while(!RADIO_CheckValidChannel(gSubMenuSelection, false, 0))
            {
                gSubMenuSelection++;
                if (gSubMenuSelection==before)
                    return;  // invalid channel
                    else if(gSubMenuSelection==MR_CHANNEL_LAST)gSubMenuSelection=0;
            }
        }

        if (
            #ifdef ENABLE_DTMF_CALLING
            UI_MENU_GetCurrentMenuId() == MENU_ANI_ID
            #else
            false
            #endif
            #ifndef ENABLE_MDC1200_EDIT
            || UI_MENU_GetCurrentMenuId() == MENU_MDC_ID
#endif
                )
            return;  // invalid
        gAskForConfirmation = 0;
        gIsInSubMenu = true;

//		if (UI_MENU_GetCurrentMenuId() != MENU_D_LIST)
        {
            gInputBoxIndex = 0;
            edit_index = -1;
            if (UI_MENU_GetCurrentMenuId() == MENU_AX25_CALLSIGN) {
                memcpy(edit, gEeprom.AX25_CALLSIGN, 6);
                edit[6] = '-';
                edit[7] = (char)(gEeprom.AX25_SSID < 10u ?
                                '0' + gEeprom.AX25_SSID :
                                'A' + gEeprom.AX25_SSID - 10u);
                edit[8] = '\0';
                edit_index = 0;
                memcpy(edit_original, edit, sizeof(edit_original));
            }

        }
#ifdef ENABLE_MDC1200
#ifdef ENABLE_MDC1200_EDIT
        if (UI_MENU_GetCurrentMenuId() == MENU_MDC_ID) {
            edit_index = 0;
            memmove(edit_original, edit, sizeof(edit_original));
        }
#endif
#endif
#ifdef ENABLE_FLEETSYNC
    if (UI_MENU_GetCurrentMenuId() == MENU_FLEETSYNC_UNIT) {
        edit_index = 0;
    }
#endif
        return;
    }
#ifdef ENABLE_MDC1200
#ifdef ENABLE_MDC1200_EDIT

    if (UI_MENU_GetCurrentMenuId() == MENU_MDC_ID && edit_index < 4) {    // editing the channel name characters

        if (++edit_index < 4)
            return;

        // exit
        if (memcmp(edit_original, edit, sizeof(edit_original)) == 0) {    // no change - drop it
            gIsInSubMenu = false;
        }
        //gFlagAcceptSetting = false;
        gAskForConfirmation = 0;

    }
#endif

#endif
#ifdef ENABLE_FLEETSYNC
    if (UI_MENU_GetCurrentMenuId() == MENU_FLEETSYNC_UNIT && edit_index < 8) {
        if (++edit_index == 3)
            ++edit_index;
        if (edit_index < 8)
            return;

        gAskForConfirmation = 0;
    }
#endif
    if (UI_MENU_GetCurrentMenuId() == MENU_AX25_CALLSIGN) {
        if (edit_index >= 0 && edit_index < 8) {
            if (++edit_index == 6)
                ++edit_index; /* position 6 is the fixed SSID separator */
            if (edit_index < 8)
                return;
            gAskForConfirmation = 0;
        }
    }
    if (UI_MENU_GetCurrentMenuId() == MENU_MEM_NAME) { //���뷨MENU
        if (edit_index < 0) {    // enter channel name edit mode
            if (!RADIO_CheckValidChannel(gSubMenuSelection, false, 0))
                return;

            SETTINGS_FetchChannelName(edit, gSubMenuSelection);
            // pad the channel name out with '_'
            edit_index = strlen(edit);
            while (edit_index < MAX_EDIT_INDEX)edit[edit_index++] = '_';
            edit[edit_index] = 0;
            edit_index = 0;  // 'edit_index' is going to be used as the cursor position

            memcpy(edit_original, edit, sizeof(edit_original));
            return;
        } else if (edit_index >= 0 && edit_index < MAX_EDIT_INDEX) {    // editing the channel name characters

            edit_index++;
            if (edit_index < MAX_EDIT_INDEX) {

                return;
            }
            // exit
            if (memcmp(edit_original, edit, sizeof(edit_original)) == 0) {    // no change - drop it
                gIsInSubMenu = false;
            }
            //gFlagAcceptSetting = false;
            gAskForConfirmation = 0;
        }
    }
    // exiting the sub menu

    if (gIsInSubMenu) {
        if (UI_MENU_GetCurrentMenuId() == MENU_RESET ||
            UI_MENU_GetCurrentMenuId() == MENU_MEM_CH ||
            UI_MENU_GetCurrentMenuId() == MENU_DEL_CH ||
            UI_MENU_GetCurrentMenuId() == MENU_MEM_NAME ||
            UI_MENU_GetCurrentMenuId() == MENU_AX25_CALLSIGN
#ifdef ENABLE_MDC1200
#ifdef ENABLE_MDC1200_EDIT

            ||
UI_MENU_GetCurrentMenuId() == MENU_MDC_ID
#endif
#endif

                ) {
            switch (gAskForConfirmation) {
                case 0:
                    gAskForConfirmation = 1;
                    break;

                case 1:
                    gAskForConfirmation = 2;

                    UI_DisplayMenu();

                    if (UI_MENU_GetCurrentMenuId() == MENU_RESET) {
#ifdef ENABLE_VOICE
                        AUDIO_SetVoiceID(0, VOICE_ID_CONFIRM);
                        AUDIO_PlaySingleVoice(true);
#endif

                        MENU_AcceptSetting();

#if defined(ENABLE_OVERLAY)
                        overlay_FLASH_RebootToBootloader();
#else
                        NVIC_SystemReset();
#endif
                    }

                    gFlagAcceptSetting = true;
                    gIsInSubMenu = false;
                    gAskForConfirmation = 0;
            }
        } else {
            gFlagAcceptSetting = true;
            gIsInSubMenu = false;
        }
    }
    SCANNER_Stop();

#ifdef ENABLE_VOICE
    gAnotherVoiceID = VOICE_ID_CONFIRM;
#endif

    gInputBoxIndex = 0;
}

static void MENU_Key_STAR(const bool bKeyPressed, const bool bKeyHeld) {
    if (bKeyHeld || !bKeyPressed)
        return;

    gBeepToPlay = BEEP_1KHZ_60MS_OPTIONAL;
//���뷨��ģʽ�л�
    if (UI_MENU_GetCurrentMenuId() == MENU_MEM_NAME && edit_index >= 0) {    // currently editing the channel name

        if (edit_index < MAX_EDIT_INDEX) {
            edit[edit_index] = '-';
            if (++edit_index >= MAX_EDIT_INDEX) {    // exit edit
                //gFlagAcceptSetting = false;
                gAskForConfirmation = 1;
            }


            gRequestDisplayScreen = DISPLAY_MENU;
        }

        return;
    }

    RADIO_SelectVfos();

#ifdef ENABLE_NOAA
    if (!IS_NOAA_CHANNEL(gRxVfo->CHANNEL_SAVE) && gRxVfo->Modulation == MODULATION_FM)
#else
    if (gRxVfo->Modulation == MODULATION_FM)
#endif
    {
        gPttWasReleased = true;
        return;
    }

    gBeepToPlay = BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL;
}

static void MENU_Key_UP_DOWN(bool bKeyPressed, bool bKeyHeld, int8_t Direction) {
    uint8_t VFO;
    uint8_t Channel;
    bool bCheckScanList;
    if (gIsInSubMenu && edit_index >= 0) { //���뷨UP DOWN
        if (UI_MENU_GetCurrentMenuId() == MENU_AX25_CALLSIGN) {
            if (bKeyPressed && edit_index < 6) {
                const char alphabet[] = " 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
                int pos = 0;
                while (pos < (int)sizeof(alphabet) - 1 && alphabet[pos] != edit[edit_index]) ++pos;
                pos = (pos + Direction + (int)sizeof(alphabet) - 1) % ((int)sizeof(alphabet) - 1);
                edit[edit_index] = alphabet[pos];
                gRequestDisplayScreen = DISPLAY_MENU;
            } else if (bKeyPressed && edit_index == 7) {
                const char ssid_hex[] = "0123456789ABCDEF";
                int pos = 0;
                while (pos < 16 && ssid_hex[pos] != edit[7]) ++pos;
                pos = (pos + Direction + 16) % 16;
                edit[7] = ssid_hex[pos];
                gRequestDisplayScreen = DISPLAY_MENU;
            }
            return;
        }
        if (UI_MENU_GetCurrentMenuId() == MENU_MEM_NAME) {    // change the character

            if (bKeyPressed && edit_index < MAX_EDIT_INDEX) {
                const char unwanted[] = "$%&!\"':;?^`|{}";
                char c = edit[edit_index] + Direction;
                unsigned int i = 0;
                while (i < sizeof(unwanted) && c >= 32 && c <= 126) {
                    if (c == unwanted[i++]) {    // choose next character
                        c += Direction;
                        i = 0;
                    }
                }
                edit[edit_index] = ((uint8_t) c < 32) ? 126 : ((uint8_t) c > 126) ? 32 : c;
                gRequestDisplayScreen = DISPLAY_MENU;
            }
            return;
        }
#ifdef ENABLE_MDC1200
#ifdef ENABLE_MDC1200_EDIT
        if (UI_MENU_GetCurrentMenuId() == MENU_MDC_ID) {
            if (bKeyPressed && edit_index < 4) {
                char c = edit[edit_index] + Direction;
                if (c < '0')c = 'F';
                else if (c > 'F')c = '0';
                else if (c > '9' && c < 'A') {
                    if (Direction == 1)c = 'A';
                    else c = '9';
                }

                edit[edit_index] = c;

                gRequestDisplayScreen = DISPLAY_MENU;
            }
            return;
        }
#endif
#endif
#ifdef ENABLE_FLEETSYNC
        if (UI_MENU_GetCurrentMenuId() == MENU_FLEETSYNC_UNIT) {
            return;
        }
#endif

    }
    if (!bKeyHeld) {
        if (!bKeyPressed)
            return;

        gBeepToPlay = BEEP_1KHZ_60MS_OPTIONAL;

        gInputBoxIndex = 0;
    } else if (!bKeyPressed)
        return;

    if (SCANNER_IsScanning()) {
        return;
    }

    if (!gIsInSubMenu) {
#ifndef ENABLE_MDC1200
        uint8_t last_num = gMenuCursor;
#endif
        gMenuCursor = NUMBER_AddWithWraparound(gMenuCursor, key_dir * Direction, 0, gMenuListCount - 1);
#ifndef ENABLE_MDC1200
        if (last_num + 1 < 26 && gMenuCursor + 1 == 26)gMenuCursor++;
        else if (last_num + 1 == 27 && gMenuCursor + 1 == 26)gMenuCursor--;
#endif
        gFlagRefreshSetting = true;

        gRequestDisplayScreen = DISPLAY_MENU;

        if (UI_MENU_GetCurrentMenuId() != MENU_ABR
            && UI_MENU_GetCurrentMenuId() != MENU_ABR_MAX
            && gEeprom.BACKLIGHT_TIME == 0) // backlight always off and not in the backlight menu
        {
            BACKLIGHT_TurnOff();
        }

        return;
    }

    if (UI_MENU_GetCurrentMenuId() == MENU_OFFSET) {
        int32_t Offset = (Direction * gTxVfo->StepFrequency) + gSubMenuSelection;
        if (Offset < 99999990) {
            if (Offset < 0)
                Offset = 99999990;
        } else
            Offset = 0;

        gSubMenuSelection = FREQUENCY_RoundToStep(Offset, gTxVfo->StepFrequency);
        gRequestDisplayScreen = DISPLAY_MENU;
        return;
    }

    VFO = 0;

    switch (UI_MENU_GetCurrentMenuId()) {
        case MENU_DEL_CH:
        case MENU_1_CALL:
        case MENU_MEM_NAME:
            bCheckScanList = false;
            break;

        case MENU_SLIST2:
            VFO = 1;
            [[fallthrough]];
        case MENU_SLIST1:
            bCheckScanList = true;
            break;

        default:
            MENU_ClampSelection(Direction);
            gRequestDisplayScreen = DISPLAY_MENU;
            return;
    }

    Channel = RADIO_FindNextChannel(gSubMenuSelection + Direction, Direction, bCheckScanList, VFO);
    if (Channel != 0xFF)
        gSubMenuSelection = Channel;

    gRequestDisplayScreen = DISPLAY_MENU;
}

void MENU_ProcessKeys(KEY_Code_t Key, bool bKeyPressed, bool bKeyHeld) {
    switch (Key) {
        case KEY_0:
        case KEY_1:
        case KEY_2:
        case KEY_3:
        case KEY_4:
        case KEY_5:
        case KEY_6:
        case KEY_7:
        case KEY_8:
        case KEY_9:
            MENU_Key_0_to_9(Key, bKeyPressed, bKeyHeld);
            break;
        case KEY_MENU:
            MENU_Key_MENU(bKeyPressed, bKeyHeld);
            break;
        case KEY_UP:
            MENU_Key_UP_DOWN(bKeyPressed, bKeyHeld, 1);
            break;
        case KEY_DOWN:
            MENU_Key_UP_DOWN(bKeyPressed, bKeyHeld, -1);
            break;
        case KEY_EXIT:
            MENU_Key_EXIT(bKeyPressed, bKeyHeld);
            break;
        case KEY_STAR:
            MENU_Key_STAR(bKeyPressed, bKeyHeld);
            break;
        case KEY_F:
            if (UI_MENU_GetCurrentMenuId() == MENU_MEM_NAME && //���뷨
                edit_index >= 0) {    // currently editing the channel name
                if (!bKeyHeld && bKeyPressed) {
                    gBeepToPlay = BEEP_1KHZ_60MS_OPTIONAL;


                    if (edit_index < MAX_EDIT_INDEX


                            ) {


                        edit[edit_index] = ' ';


                        if (++edit_index >= MAX_EDIT_INDEX) {    // exit edit
                            //gFlagAcceptSetting = false;
                            gAskForConfirmation = 1;
                        }


                        gRequestDisplayScreen = DISPLAY_MENU;
                    }
                }
                break;
            }

            GENERIC_Key_F(bKeyPressed, bKeyHeld);
            break;
        case KEY_PTT:
            GENERIC_Key_PTT(bKeyPressed);
            break;
        default:
            if (!bKeyHeld && bKeyPressed)
                gBeepToPlay = BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL;
            break;
    }

    if (gScreenToDisplay == DISPLAY_MENU) {
        if (/*UI_MENU_GetCurrentMenuId() == MENU_VOL||*/
#ifdef ENABLE_F_CAL_MENU
UI_MENU_GetCurrentMenuId() == MENU_F_CALI||
#endif
UI_MENU_GetCurrentMenuId() == MENU_BATCAL
                ) {
            gMenuCountdown = menu_timeout_long_500ms;
        } else {
            gMenuCountdown = menu_timeout_500ms;
        }
    }

}
