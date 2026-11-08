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
#include "driver/eeprom.h"
#include <string.h>
#include <stdlib.h>  // abs()
#include "bitmaps.h"
#include "driver/uart.h"
#include "app/dtmf.h"
#include "app/menu.h"
#include "board.h"
#include "dcs.h"
#include "driver/backlight.h"
#include "driver/bk4819.h"
#include "driver/st7565.h"
#include "external/printf/printf.h"
#include "frequencies.h"
#include "helper/battery.h"
#include "misc.h"
#include "settings.h"
#include "ui/helper.h"
#include "ui/inputbox.h"
#include "ui/menu.h"
#include "ui/ui.h"
#include "ui_strings.h"

void insertNewline(char a[], int index, int len) {

    if (index < 0 || index >= len || len >= 63) {
        return;
    }
    for (int i = len; i >= index; i--) {
        a[i + 1] = a[i];
    }
    a[index] = '\n';
    a[len + 1] = '\0'; // Null-terminate the string
}

const t_menu_item MenuList[] =
        {
//   text,     voice ID,                               menu ID
                {/*"Step",*/   VOICE_ID_FREQUENCY_STEP, MENU_STEP, STR_STEP},
                {/*"RxDCS",*/  VOICE_ID_DCS, MENU_R_DCS, STR_RXDCS}, // was "R_DCS"
                {/*"RxCTCS",*/ VOICE_ID_CTCSS, MENU_R_CTCS, STR_RXCTCS}, // was "R_CTCS"
                {/*"TxDCS",*/  VOICE_ID_DCS, MENU_T_DCS, STR_TXDCS}, // was "T_DCS"
                {/*"TxCTCS",*/ VOICE_ID_CTCSS, MENU_T_CTCS, STR_TXCTCS}, // was "T_CTCS"
                {/*"TxODir",*/ VOICE_ID_TX_OFFSET_FREQUENCY_DIRECTION, MENU_SFT_D, STR_TXODIR}, // was "SFT_D"
                {/*"TxOffs",*/ VOICE_ID_TX_OFFSET_FREQUENCY, MENU_OFFSET, STR_TXOFFS}, // was "OFFSET"
#ifdef ENABLE_CUSTOM_SIDEFUNCTIONS

                {/*"W/N",*/    VOICE_ID_CHANNEL_BANDWIDTH,             MENU_W_N           ,STR_W_N},
#endif

                {/*"BusyCL",*/ VOICE_ID_BUSY_LOCKOUT, MENU_BCL, STR_BUSYCL}, // was "BCL"
                {/*"Compnd",*/ VOICE_ID_INVALID, MENU_COMPAND, STR_COMPND},
                {/*"ChSave",*/ VOICE_ID_MEMORY_CHANNEL, MENU_MEM_CH, STR_CHSAVE}, // was "MEM-CH"
                {/*"ChDele",*/ VOICE_ID_DELETE_CHANNEL, MENU_DEL_CH, STR_CHDELE}, // was "DEL-CH"
                {/*"ChName",*/ VOICE_ID_INVALID, MENU_MEM_NAME, STR_CHNAME},
                {/*"SList",*/  VOICE_ID_INVALID, MENU_S_LIST, STR_SLIST},
                {/*"SList1",*/ VOICE_ID_INVALID, MENU_SLIST1, STR_SLIST1},
                {/*"SList2",*/ VOICE_ID_INVALID, MENU_SLIST2, STR_SLIST2},
                {/*"ScnRev",*/ VOICE_ID_INVALID, MENU_SC_REV, STR_SCNREV},
                {/*"TxTOut",*/ VOICE_ID_TRANSMIT_OVER_TIME, MENU_TOT, STR_TXTOUT}, // was "TOT"
                {/*"BatSav",*/ VOICE_ID_SAVE_MODE, MENU_SAVE, STR_BATSAV}, // was "SAVE"
                {/*"Mic",*/    VOICE_ID_INVALID, MENU_MIC, STR_MIC},
                {/*"ChDisp",*/ VOICE_ID_INVALID, MENU_MDF, STR_CHDISP}, // was "MDF"
                {/*"BackLt",*/ VOICE_ID_INVALID, MENU_ABR, STR_BACKLT}, // was "ABR"
                {/*"BLMax",*/  VOICE_ID_INVALID, MENU_ABR_MAX, STR_LIGHT},
                {/*"AXCall",*/ VOICE_ID_INVALID, MENU_AX25_CALLSIGN, "AXCall"},
#ifdef ENABLE_MDC1200
                {/*"MDCID",*/  VOICE_ID_INVALID, MENU_MDC_ID, MDC_ID},
 #ifdef ENABLE_FLEETSYNC
                {/*"FScID",*/ VOICE_ID_INVALID, MENU_FLEETSYNC_UNIT, "FScID"},
#endif
                {/*"IDDly",*/ VOICE_ID_INVALID, MENU_RP_STE, "ID Dly"},
                {/*"MDCPre",*/ VOICE_ID_INVALID, MENU_MDC_PREAMBLE_DURATION, "MDCPre"},
                {/*"PreWhn",*/ VOICE_ID_INVALID, MENU_MDC_PREAMBLE_WHEN, "PreWhn"},
#endif
                {/*"PTT ID",*/ VOICE_ID_INVALID, MENU_PTT_ID, STR_PTT_ID},


                {/*"STE",*/    VOICE_ID_INVALID, MENU_STE, STR_STE},
                {/*"C.End",*/  VOICE_ID_INVALID, MENU_END_CALL_TONE, "C.End"},
                {/*"1 Call",*/ VOICE_ID_INVALID, MENU_1_CALL, STR_V_1_CALL},

#ifdef ENABLE_CUSTOM_SIDEFUNCTIONS
                {/*"F1Shrt",*/ VOICE_ID_INVALID,                       MENU_F1SHRT        ,STR_F1SHRT},
                {/*"F1Long",*/ VOICE_ID_INVALID,                       MENU_F1LONG        ,STR_F1LONG},
                {/*"F2Shrt",*/ VOICE_ID_INVALID,                       MENU_F2SHRT        ,STR_F2SHRT},
                {/*"F2Long",*/ VOICE_ID_INVALID,                       MENU_F2LONG        ,STR_F2LONG},
                {/*"M Long",*/ VOICE_ID_INVALID,                       MENU_MLONG         ,STR_M_LONG},
#endif

#ifdef ENABLE_DTMF_CALLING

                {/*"ANI ID",*/ VOICE_ID_ANI_CODE,                      MENU_ANI_ID        ,DTMF_ID},
#endif
                {/*"UPCode",*/ VOICE_ID_INVALID, MENU_UPCODE, STR_UPCODE},
                {/*"DWCode",*/ VOICE_ID_INVALID, MENU_DWCODE, STR_DWCODE},
                {/*"D ST",*/   VOICE_ID_INVALID, MENU_D_ST, STR_D_ST},
#ifdef ENABLE_DTMF_CALLING
                // DTMF response/hold menus disabled
#endif
#ifdef ENABLE_DTMF_CALLING
#ifdef ENABLE_CUSTOM_SIDEFUNCTIONS
#endif
                // {/*"D List",*/ VOICE_ID_INVALID,                       MENU_D_LIST        ,STR_D_LIST}, // disabled
#endif
                // {/*"D Live",*/ VOICE_ID_INVALID, MENU_D_LIVE_DEC, STR_D_LIVE}, // live DTMF decoder disabled
#ifdef ENABLE_AM_FIX_TEST1//0
                {/*"AM FT1",*/ VOICE_ID_INVALID,                       MENU_AM_FIX_TEST1  ,""},
#endif

                {/*"RxMode",*/ VOICE_ID_DUAL_STANDBY, MENU_TDR, STR_RXMODE},
                {/*"Sql",*/    VOICE_ID_SQUELCH, MENU_SQL, STR_SQL},

                // hidden menu items from here on
                // enabled if pressing both the PTT and upper side button at power-on
//                {/*"Tx 200",*/ VOICE_ID_INVALID,                       MENU_200TX         ,STR_TX_200}, // was "200TX"
//                {/*"Tx 350",*/ VOICE_ID_INVALID,                       MENU_350TX         ,STR_TX_350}, // was "350TX"
//                {/*"Tx 500",*/ VOICE_ID_INVALID,                       MENU_500TX         ,STR_TX_500}, // was "500TX"
//                {/*"350 En",*/ VOICE_ID_INVALID,                       MENU_350EN         ,STR_V_350_EN}, // was "350EN"
                {/*"TPT",*/    VOICE_ID_INVALID, MENU_TALK_PERMIT_TONE, "TPT"},
                {/*"Rx.Led",*/ VOICE_ID_INVALID, MENU_RX_LM, "Rx.Led"},
                {/*"F Lock",*/ VOICE_ID_INVALID, MENU_F_LOCK, STR_F_LOCK},
#ifdef ENABLE_F_CAL_MENU//0
                {/*"FrCali",*/ VOICE_ID_INVALID,                       MENU_F_CALI        ,""}, // reference xtal calibration
#endif
                {/*"BatCal",*/ VOICE_ID_INVALID, MENU_BATCAL, STR_BATCAL}, // battery voltage calibration
                {/*"PwrCal",*/ VOICE_ID_INVALID, MENU_TX_CAL, "PwrCal"},
                {/*"BatVol",*/ VOICE_ID_INVALID, MENU_BATTYP, "BatVol"},
                {/*"Reset",*/  VOICE_ID_INITIALISATION, MENU_RESET,
                               STR_RESET}, // might be better to move this to the hidden menu items ?

                {/*"",*/       VOICE_ID_INVALID, 0xff, "\x00"}  // end of list - DO NOT delete or move this this
        };

#ifdef ENABLE_CUSTOM_SIDEFUNCTIONS
const char gSubMenu_W_N[][7] =//7

        {
//                "WIDE",
//                "NARROW"
                STR_WIDE,
               STR_NARROW
        };
#endif
const char gSubMenu_SFT_D[][4] =

        {
//                "OFF",
//                "+",
//                "-"
                STR_OFF,
                STR_TX_EQUALS_RX_PLUS,
                STR_TX_EQUALS_RX_MINUS

        };


const char gSubMenu_STE[][8] =  // 0=OFF, 1=55Hz, 2=180°
{
    "OFF",
    "55Hz",
    "180"
};

const char gSubMenu_OFF_ON[][4] =


        {
//                "OFF",
//                "ON"
                STR_OFF,
                STR_ON
        };

const char gSubMenu_SAVE[][4] =//4
        {
//                "OFF",
//                "1:1",
//                "1:2",
//                "1:3",
//                "1:4",
//                "1:5",
//                "1:6"

                STR_OFF,
                STR_V_1_1,
                STR_V_1_2,
                STR_V_1_3,
                STR_V_1_4,
                STR_V_1_5,
                STR_V_1_6,

        };
const char gSubMenu_TOT[][7] = //7
        {
//                "30 sec",
//                "1 min",
//                "2 min",
//                "3 min",
//                "4 min",
//                "5 min",
//                "6 min",
//                "7 min",
//                "8 min",
//                "9 min",
//                "15 min"

                STR_V_30_SEC,
                STR_V_1_MIN,
                STR_V_2_MIN,
                STR_V_3_MIN,
                STR_V_4_MIN,
                STR_V_5_MIN,
                STR_V_6_MIN,
                STR_V_7_MIN,
                STR_V_8_MIN,
                STR_V_9_MIN,
                STR_V_15_MIN

        };

const char *const gSubMenu_RXMode[] =
        {

//                "MAIN\nONLY",        // TX and RX on main only
//                "DUAL RX\nRESPOND", // Watch both and respond
//                "CROSS\nBAND",        // TX on main, RX on secondary
//                "MAIN TX\nDUAL RX"    // always TX on main, but RX on both
                STR_MAIN_ONLY,        // TX and RX on main only
                STR_DUAL_RX_RESPOND, // Watch both and respond
                STR_CROSS_BAND,        // TX on main, RX on secondary
                STR_MAIN_TX_DUAL_RX    // always TX on main, but RX on both

        };

#ifdef ENABLE_VOICE
const char gSubMenu_VOICE[][4] =
{
    "OFF",
    "CHI",
    "ENG"
};
#endif
const char gSubMenu_SC_REV[][8] =//8


        {
//                "TIMEOUT",
//                "CARRIER",
//                "STOP"
                STR_TIME,
                STR_CARRIER,
                STR_STOP

        };

const char *const gSubMenu_MDF[] =
        {
//                "FREQ",
//                "CHANNEL\nNUMBER",
//                "NAME",
//                "NAME\n+\nFREQ"
                STR_FREQ,
                STR_CHANNEL_NUMBER,
                STR_NAME,
                STR_NAME_FREQ
        };

#ifdef ENABLE_ALARM
const char gSubMenu_AL_MOD[][5] =
{
    "SITE",
    "TONE"
};
#endif
#ifdef ENABLE_DTMF_CALLING

//const char gSubMenu_D_RSP[][11] =//11 // DTMF Response disabled - commented out

        /*{
//                "DO\nNOTHING",
//                "RING",
//                "REPLY",
//                "BOTH"
                STR_DO_NOTHING,
                STR_RING,
                STR_REPLY,
               STR_BOTH
        }*/;
#endif

const char *const gSubMenu_PTT_ID[] =
{
    STR_OFF,
    "Pre MDC",
    "Post MDC",
    "Both MDC",
    "Pre FSync",
    "Post FSync",
    "Both FSync",
    "Pre AX.25",
    "Post AX.25",
    "Both AX.25"
};

#ifdef ENABLE_MDC1200
const char gSubMenu_MDC_PREAMBLE_DURATION[][4] =
{
                "OFF", "+1", "+2", "+3", "+4",
                "+5", "+6", "+7"
};

const char gSubMenu_MDC_PREAMBLE_WHEN[][5] =
{
                "Pre",
                "Post",
                "Both"
};
#endif

const char gSubMenu_RESET[][4] =//4


        {
//                "VFO",
//                "ALL"
                STR_VFO,
                STR_ALL
        };

const char *const gSubMenu_F_LOCK[] =
        {
                "DEFAULT+\n137-174\n400-470",
                "RTX Spec+\n138-150\n400-500",
                "CE HAM\n144-146\n430-440",
                "GB HAM\n144-148\n430-440",

//                "DISABLE\nALL",
//                "UNLOCK\nALL",
                STR_DISABLE_ALL,
                STR_UNLOCK_ALL,
        };

const char gSubMenu_BACKLIGHT[][7] =//7

        {
//                "OFF",
//                "5 sec",
//                "10 sec",
//                "20 sec",
//                "1 min",
//                "2 min",
//                "4 min",
//                "ON"
                STR_OFF,
                STR_V_5_SEC,
                STR_V_10_SEC,
                STR_V_20_SEC,
                STR_V_1_MIN,
                STR_V_2_MIN,
                STR_V_4_MIN,
                STR_ON

        };


const char gSubMenu_RX_TX[][6] =//6

        {
//                "OFF",
//                "TX",
//                "RX",
//                "TX/RX"
                STR_OFF,
                STR_TX,
                STR_RX,
                STR_TX_RX
        };

#ifdef ENABLE_AM_FIX_TEST1
const char gSubMenu_AM_fix_test1[][8] =
{
    "LNA-S 0",
    "LNA-S 1",
    "LNA-S 2",
    "LNA-S 3"
};
#endif


const char gSubMenu_RX_LM[][6] =
        {
                "OFF",
                "SOLID",
                "BLINK"
        };

const char gSubMenu_TALK_PERMIT_TONE[][6] =
    {
        "OFF",
        "XTS",
        "TRBO",
        "Kenw",
        "Auto"
    };

const char gSubMenu_BATTYP[][8] =
        {
                "1600mAh",
                "2200mAh"
        };


#ifdef ENABLE_CUSTOM_SIDEFUNCTIONS
const t_sidefunction SIDEFUNCTIONS[] =
        {
               {STR_OFF, ACTION_OPT_NONE},
#ifdef ENABLE_FLASHLIGHT
               {STR_LIGHT_2, ACTION_OPT_FLASHLIGHT},
#endif
               {STR_POWER, ACTION_OPT_POWER},
               {STR_MONITOR, ACTION_OPT_MONITOR},
               {STR_SCAN, ACTION_OPT_SCAN},
#ifdef ENABLE_VOX
               {STR_VOX,				ACTION_OPT_VOX},
#endif
#ifdef ENABLE_ALARM
                {"ALARM",			ACTION_OPT_ALARM},
#endif
#ifdef ENABLE_FMRADIO
               {STR_FM_RADIO,		ACTION_OPT_FM},
#endif
#ifdef ENABLE_TX1750
                {"1750HZ",			ACTION_OPT_1750},
#endif
               {STR_LOCK_KEY, ACTION_OPT_KEYLOCK},
               {STR_SWITCH_VFO, ACTION_OPT_A_B},
               {STR_VFO_MR, ACTION_OPT_VFO_MR},
               {STR_DEMODU, ACTION_OPT_SWITCH_DEMODUL},
               {STR_W_N, ACTION_OPT_WIDTH},
#ifdef ENABLE_SIDEFUNCTIONS_SEND
               {STR_MAIN_SEND, ACTION_OPT_SEND_CURRENT},
               {STR_DUAL_SEND, ACTION_OPT_SEND_OTHER},
#endif
#ifdef ENABLE_BLMIN_TMP_OFF
                {"BLMIN\nTMP OFF",  ACTION_OPT_BLMIN_TMP_OFF}, 		//BackLight Minimum Temporay OFF
#endif
        };
const t_sidefunction *gSubMenu_SIDEFUNCTIONS = SIDEFUNCTIONS;
const uint8_t gSubMenu_SIDEFUNCTIONS_size = ARRAY_SIZE(SIDEFUNCTIONS);
#endif

bool gIsInSubMenu;
uint8_t gMenuCursor;

int UI_MENU_GetCurrentMenuId() {

    if (gMenuCursor < ARRAY_SIZE(MenuList))
        return MenuList[gMenuCursor].menu_id;
    else
        return MenuList[ARRAY_SIZE(MenuList) - 1].menu_id;
}

uint8_t UI_MENU_GetMenuIdx(uint8_t id) {
    for (uint8_t i = 0; i < ARRAY_SIZE(MenuList); i++)
        if (MenuList[i].menu_id == id)
            return i;
    return 0;
}

int32_t gSubMenuSelection;

// edit box
char edit_original[17]; // a copy of the text before editing so that we can easily test for changes/difference
char edit[17];
int edit_index;


void UI_DisplayMenu(void) {
    const unsigned int menu_list_width = 6; // max no. of characters on the menu list (left side)
    const unsigned int menu_item_x1 = (8 * menu_list_width);//+ 2;
    const unsigned int menu_item_x2 = LCD_WIDTH - 1;
    unsigned int i;
    char String[64];  // bigger cuz we can now do multi-line in one string (use '\n' char)
#ifdef ENABLE_DTMF_CALLING
    char               Contact[16];
#endif

    // clear the screen buffer
    UI_DisplayClear();

#if 1
    // original menu layout



    // invert the current menu list item pixels反转当前菜单项的像素值 ：


    // draw vertical separating dotted line绘制垂直分隔的点线 ：
//    for (i = 0; i < 7; i++)
//        gFrameBuffer[i][(8 * menu_list_width) + 1] = 0xAA;


    // draw the little sub-menu triangle marker绘制子菜单三角标志：
    //const void *BITMAP_CurrentIndicator = BITMAP_MARKER;

    if (gIsInSubMenu)
        memmove(gFrameBuffer[2] + 41, BITMAP_VFO_Default, sizeof(BITMAP_VFO_Default));
#ifndef ENABLE_MDC1200
    uint8_t add = 1;

    if (gMenuCursor + 1 >= gMenuListCount - 1)  // -1 for the terminator entry
        add = 0;

    sprintf(String, "%2u/%u", add + gMenuCursor, gMenuListCount - 1);

#else
    sprintf(String, "%2u/%u", 1 + gMenuCursor, gMenuListCount);
#endif

    UI_PrintStringSmall(String, 2, 0, 6);


    {
    const char *menu_name = MenuList[gMenuCursor].name;
#ifdef ENABLE_FLEETSYNC
    if (MenuList[gMenuCursor].menu_id == MENU_RP_STE) {
        if (gEeprom.PTT_ID == PTT_ID_MODE_FLEETSYNC_PRE ||
                 gEeprom.PTT_ID == PTT_ID_MODE_FLEETSYNC_POST ||
                 gEeprom.PTT_ID == PTT_ID_MODE_FLEETSYNC_BOTH)
            menu_name = "FSc Dly";
        else
            menu_name = "MDC Dly";
    }
#endif
    uint8_t size_menu = strlen(menu_name)*7;
    UI_PrintStringSmall(menu_name, size_menu < 48 ? (48 - size_menu) / 2 : 0, 0, 0);
    }

#else
    {	// new menu layout .. experimental & unfinished

        const int menu_index = gMenuCursor;  // current selected menu item
        i = 1;

        if (!gIsInSubMenu)
        {
            while (i < 2)
            {	// leading menu items - small text
                const int k = menu_index + i - 2;
                if (k < 0)
                    UI_PrintStringSmall(MenuList[gMenuListCount + k].name, 0, 0, i);  // wrap-a-round
                else
                if (k >= 0 && k < (int)gMenuListCount)
                    UI_PrintStringSmall(MenuList[k].name, 0, 0, i);
                i++;
            }

            // current menu item - keep big n fat
            if (menu_index >= 0 && menu_index < (int)gMenuListCount)
                UI_PrintStringSmall(MenuList[menu_index].name, 0, 0, 2);
            i++;

            while (i < 4)
            {	// trailing menu item - small text
                const int k = menu_index + i - 2;
                if (k >= 0 && k < (int)gMenuListCount)
                    UI_PrintStringSmall(MenuList[k].name, 0, 0, 1 + i);
                else
                if (k >= (int)gMenuListCount)
                    UI_PrintStringSmall(MenuList[gMenuListCount - k].name, 0, 0, 1 + i);  // wrap-a-round
                i++;
            }

            // draw the menu index number/count
            sprintf(String, "%2u.%u", 1 + gMenuCursor, gMenuListCount);
            UI_PrintStringSmall(String, 2, 0, 6);
        }
        else
        if (menu_index >= 0 && menu_index < (int)gMenuListCount)
        {	// current menu item

            UI_PrintStringSmall(MenuList[menu_index].name, 0, 0, 0);
//			UI_PrintStringSmall(String, 0, 0, 0);
        }
    }
#endif

    // **************

    memset(String, 0, sizeof(String));

    bool already_printed = false;
/* Brightness is set to max in some entries of this menu. Return it to the configured brightness
	   level the "next" time we enter here.I.e., when we move from one menu to another.
	   It also has to be set back to max when pressing the Exit key. */

    BACKLIGHT_TurnOn();
    switch (UI_MENU_GetCurrentMenuId()) {
        case MENU_SQL:
            sprintf(String, "%d", gSubMenuSelection);
            break;

        case MENU_MIC: {    // display the mic gain in actual dB rather than just an index number
            const uint8_t mic = gMicGain_dB2[gSubMenuSelection];
            sprintf(String, "+%u.%01udB", mic / 2, mic % 2);
        }
            break;

//#ifdef ENABLE_AUDIO_BAR
//            case MENU_MIC_BAR:
//                strcpy(String, gSubMenu_OFF_ON[gSubMenuSelection]);
//                break;
//#endif

        case MENU_STEP: {
            uint16_t step = gStepFrequencyTable[FREQUENCY_GetStepIdxFromSortedIdx(gSubMenuSelection)];
            sprintf(String, "%d.%02ukHz", step / 100, step % 100);
            break;
        }

//        case MENU_TXP:
//            strncpy(String, gSubMenu_TXP[gSubMenuSelection], sizeof(gSubMenu_TXP[gSubMenuSelection]));
//            String[sizeof(gSubMenu_TXP[gSubMenuSelection])] = '\0';
//
//
//            break;

        case MENU_R_DCS:
        case MENU_T_DCS:
            if (gSubMenuSelection == 0)
                //translate
#ifdef test
                strcpy(String, "OFF");

#else
                strcpy(String, STR_OFF);

#endif


            else if (gSubMenuSelection < 105)
                sprintf(String, "D%03oN", DCS_Options[gSubMenuSelection - 1]);
            else
                sprintf(String, "D%03oI", DCS_Options[gSubMenuSelection - 105]);

            break;

        case MENU_R_CTCS:
        case MENU_T_CTCS: {
            if (gSubMenuSelection == 0)
                // translate
#ifdef test
                strcpy(String, "OFF");

#else
                //STR_OFF
                strcpy(String, STR_OFF);

#endif

            else {
                sprintf(String, "%u.%uHz", CTCSS_Options[gSubMenuSelection - 1] / 10,
                        CTCSS_Options[gSubMenuSelection - 1] % 10);
            }

            break;
        }

        case MENU_SFT_D:
            strncpy(String, gSubMenu_SFT_D[gSubMenuSelection], sizeof(gSubMenu_SFT_D[gSubMenuSelection]));
            String[sizeof(gSubMenu_SFT_D[gSubMenuSelection])] = '\0';
            break;

        case MENU_OFFSET:
            if (!gIsInSubMenu || gInputBoxIndex == 0) {
                sprintf(String, "%3d.%05u", gSubMenuSelection / 100000, abs(gSubMenuSelection) % 100000);
                UI_PrintStringSmall(String, menu_item_x1, menu_item_x2, 2);
            } else {
                const char *ascii = INPUTBOX_GetAscii();
                sprintf(String, "%.3s.%.3s  ", ascii, ascii + 3);
                UI_PrintStringSmall(String, menu_item_x1, menu_item_x2, 2);
            }

            UI_PrintStringSmall("MHz", menu_item_x1, menu_item_x2, 4);

            already_printed = true;
            break;
#ifdef ENABLE_CUSTOM_SIDEFUNCTIONS

            case MENU_W_N:

                strcpy(String, gSubMenu_W_N[gSubMenuSelection]);
                break;
#endif

        case MENU_ABR:
            strcpy(String, gSubMenu_BACKLIGHT[gSubMenuSelection]);


//            BACKLIGHT_SetBrightness(-1);
            break;

            // case MENU_ABR_MIN:
        case MENU_ABR_MAX:
            sprintf(String, "%d", gSubMenuSelection);
            if (gIsInSubMenu)
                BACKLIGHT_SetBrightness(gSubMenuSelection);
//            else
//                BACKLIGHT_SetBrightness(-1);
            break;

//        case MENU_AM:
//            strcpy(String, gModulationStr[gSubMenuSelection]);
//
//            break;

#ifdef ENABLE_AM_FIX_TEST1
            case MENU_AM_FIX_TEST1:
                strcpy(String, gSubMenu_AM_fix_test1[gSubMenuSelection]);
break;
#endif


        case MENU_COMPAND:
            strcpy(String, gSubMenu_RX_TX[gSubMenuSelection]);


            break;

        case MENU_BCL:
            //     case MENU_BEEP:
//        case MENU_S_ADD1:
//        case MENU_S_ADD2:
        case MENU_STE:
            strcpy(String, gSubMenu_STE[gSubMenuSelection]);
            break;

        case MENU_END_CALL_TONE:
            strcpy(String, gSubMenu_OFF_ON[gSubMenuSelection]);
            break;

        case MENU_D_ST:
#ifdef ENABLE_DTMF_CALLING
#ifdef ENABLE_CUSTOM_SIDEFUNCTIONS
#endif
#endif
        // case MENU_D_LIVE_DEC: // Live DTMF decoder disabled
#ifdef ENABLE_NOAA
            case MENU_NOAA_S:
#endif
//        case MENU_350TX:
//        case MENU_200TX:
//        case MENU_500TX:
//        case MENU_350EN:
            strcpy(String, gSubMenu_OFF_ON[gSubMenuSelection]);

            break;
        case MENU_MEM_CH:
        case MENU_1_CALL:
        case MENU_DEL_CH: {
            const bool valid = RADIO_CheckValidChannel(gSubMenuSelection, false, 1);

            UI_GenerateChannelStringEx(String, valid, gSubMenuSelection);
            UI_PrintStringSmall(String, menu_item_x1 - 12, menu_item_x2, 2);

            if (valid && !gAskForConfirmation) {    // show the frequency so that the user knows the channels frequency
                const uint32_t frequency = SETTINGS_FetchChannelFrequency(gSubMenuSelection);
                sprintf(String, "%u.%05u", frequency / 100000, frequency % 100000);
                UI_PrintStringSmall(String, menu_item_x1 - 12, menu_item_x2, 5);
            }
            SETTINGS_FetchChannelName(String, gSubMenuSelection);
            UI_PrintStringSmall(String[0] ? String : "--", menu_item_x1 - 12, menu_item_x2, 3);
            already_printed = true;
            break;
        }
        case MENU_AX25_CALLSIGN: {
            if (gIsInSubMenu) {
                UI_PrintStringSmall(edit, menu_item_x1, menu_item_x2, 3);
                if (edit_index < 8 && edit_index != 6)
                    UI_PrintStringSmall("^", menu_item_x1 + (((menu_item_x2 - menu_item_x1) - 56) + 1) / 2 + (7 * edit_index), 0, 4);
            } else {
                sprintf(String, "%s-%X", gEeprom.AX25_CALLSIGN, gEeprom.AX25_SSID);
                UI_PrintStringSmall(String, menu_item_x1, menu_item_x2, 3);
                edit_index = -1;
                memcpy(edit, gEeprom.AX25_CALLSIGN, 6);
                edit[6] = '-';
                edit[7] = (char)(gEeprom.AX25_SSID < 10u ? '0' + gEeprom.AX25_SSID : 'A' + gEeprom.AX25_SSID - 10u);
                edit[8] = '\0';
            }
            already_printed = true;
            break;
        }
#ifdef ENABLE_MDC1200
        case MENU_MDC_ID: {
#ifdef ENABLE_MDC1200_EDIT
            if (gIsInSubMenu) {
                // show the channel name being edited
                UI_PrintStringSmall(edit, menu_item_x1, menu_item_x2, 3);
                if (edit_index < 4)
                    UI_PrintStringSmall("^", menu_item_x1 + (((menu_item_x2 - menu_item_x1) - (28)) + 1) / 2 + (7 * edit_index), 0, 4); // show the cursor
            } else {
#endif
                sprintf(String, "%04X", gEeprom.MDC1200_ID); // %04X确保输出是4个字符长度的十六进制数
                UI_PrintStringSmall(String, menu_item_x1, menu_item_x2, 3); //4

#ifdef ENABLE_MDC1200_EDIT

                edit_index = -1;
                edit[0] = String[0];
                edit[1] = String[1];
                edit[2] = String[2];
                edit[3] = String[3];
                edit[4] = '\0';
#endif
#ifdef ENABLE_MDC1200_EDIT
            }
#endif
            already_printed = true;
            break;
        }
#endif
#ifdef ENABLE_FLEETSYNC
        case MENU_FLEETSYNC_UNIT: {
            if (gIsInSubMenu) {
                char separator = edit[3];
                edit[3] = '\0';
                UI_PrintStringSmall(edit, menu_item_x1, 0, 2);
                edit[3] = separator;
                UI_PrintStringSmall(edit + 4, menu_item_x1, 0, 4);
                if (edit_index < 8 && edit_index != 3) {
                    const uint8_t line = edit_index < 3 ? 2 : 4;
                    const uint8_t column = edit_index < 3 ? edit_index : edit_index - 4;
                    UI_PrintStringSmall("^", menu_item_x1 + (7 * column), 0, line + 1);
                }
            } else {
                sprintf(String, "%03u", gEeprom.FLEETSYNC_FLEET);
                UI_PrintStringSmall(String, menu_item_x1, 0, 2);
                sprintf(String, "%04u", gEeprom.FLEETSYNC_UNIT);
                UI_PrintStringSmall(String, menu_item_x1, 0, 4);

                edit_index = -1;
                sprintf(edit, "%03u\n%04u", gEeprom.FLEETSYNC_FLEET, gEeprom.FLEETSYNC_UNIT);
            }
            already_printed = true;
            break;
        }
#endif
        case MENU_MEM_NAME: { //输入法显示
//ok


            const bool valid = RADIO_CheckValidChannel(gSubMenuSelection, false, 1);
            UI_GenerateChannelStringEx(String, valid, gSubMenuSelection);
            UI_PrintStringSmall(String, menu_item_x1 - 12, menu_item_x2, 2);

            if (valid) {
                const uint32_t frequency = SETTINGS_FetchChannelFrequency(gSubMenuSelection);
                //bug way
                char tmp_name[17] = {0};
                SETTINGS_FetchChannelName(tmp_name, gSubMenuSelection);

                if (!gIsInSubMenu)
                    edit_index = -1;
                if (edit_index < 0) {    // show the channel name
                    SETTINGS_FetchChannelName(String, gSubMenuSelection);
                    char *pPrintStr = String[0] ? String : "--";
                    UI_PrintStringSmall(pPrintStr, menu_item_x1 - 12, menu_item_x2, 3);

                }

//
                else {


                    UI_PrintStringSmall(edit, menu_item_x1 - 12, menu_item_x2, 3);

                    if (edit_index < MAX_EDIT_INDEX) {
//#endif


                        gFrameBuffer[4][menu_item_x1 - 12 + 7 * edit_index +
                                        (((menu_item_x2 - menu_item_x1 + 12) - (7 * MAX_EDIT_INDEX)) + 1) / 2 + 3] |=
                                3 << 6;
                        gFrameBuffer[4][menu_item_x1 - 12 + 7 * edit_index +
                                        (((menu_item_x2 - menu_item_x1 + 12) - (7 * MAX_EDIT_INDEX)) + 1) / 2 + 4] |=
                                3 << 6;



                    }
                }

//NOT OK
//                sprintf(String, "%d", edit_index);
//                UI_PrintStringSmall(String, 0, 0, 1);
//                sprintf(String, "%d", gIsInSubMenu);
//                UI_PrintStringSmall(String, 20, 0, 1);
//
//                sprintf(String,"%d",edit[2]);
//                UI_PrintStringSmall(String, 0, 0, 3);


                if (!gAskForConfirmation) {    // show the frequency so that the user knows the channels frequency
                    sprintf(String, "%u.%05u", frequency / 100000, frequency % 100000);
                    {
                        UI_PrintStringSmall(String, menu_item_x1 - 12, menu_item_x2, 5);
                    }
                }
            }

            already_printed = true;

            break;
        }

        case MENU_SAVE:
            strcpy(String, gSubMenu_SAVE[gSubMenuSelection]);


            break;

        case MENU_TDR:
            strcpy(String, gSubMenu_RXMode[gSubMenuSelection]);


            break;

        case MENU_TOT:
            strcpy(String, gSubMenu_TOT[gSubMenuSelection]);


            break;

#ifdef ENABLE_VOICE
            case MENU_VOICE:
                strcpy(String, gSubMenu_VOICE[gSubMenuSelection]);
                break;
#endif

        case MENU_SC_REV:


            strcpy(String, gSubMenu_SC_REV[gSubMenuSelection]);


            break;

        case MENU_MDF:


            strcpy(String, gSubMenu_MDF[gSubMenuSelection]);


            break;

        case MENU_RP_STE:
            sprintf(String, "%d00ms", gSubMenuSelection);
            break;

        case MENU_S_LIST:
            if (gSubMenuSelection < 2)

                //translate

#ifdef test
                sprintf(String, "list %u", 1 + gSubMenuSelection);

#else  //！！STR_LIST
                sprintf(String, STR_LIST" %u", 1 + gSubMenuSelection);

#endif

            else

#ifdef test
                strcpy(String, "ALL");

#else
                //STR_ALL
                strcpy(String, STR_ALL);

#endif
            break;

#ifdef ENABLE_ALARM
            case MENU_AL_MOD:
                sprintf(String, gSubMenu_AL_MOD[gSubMenuSelection]);
                break;
#endif
#ifdef ENABLE_DTMF_CALLING
            case MENU_ANI_ID:

                strcpy(String, gEeprom.ANI_DTMF_ID);
                break;
#endif

        case MENU_UPCODE:

            sprintf(String, "%.8s\n%.8s", gEeprom.DTMF_UP_CODE, gEeprom.DTMF_UP_CODE + 8);
            break;

        case MENU_DWCODE:
            sprintf(String, "%.8s\n%.8s", gEeprom.DTMF_DOWN_CODE, gEeprom.DTMF_DOWN_CODE + 8);
            break;
#ifdef ENABLE_DTMF_CALLING
            // case MENU_D_RSP: // DTMF Response disabled
            //     strcpy(String, gSubMenu_D_RSP[gSubMenuSelection]);
            //     break;

            // case MENU_D_HOLD: // DTMF Hold disabled
            //     sprintf(String, "%ds", gSubMenuSelection);
            //     break;
#endif
        case MENU_PTT_ID:
            strcpy(String, gSubMenu_PTT_ID[gSubMenuSelection]);


            break;

//        case MENU_BAT_TXT:
//            strcpy(String, gSubMenu_BAT_TXT[gSubMenuSelection]);
//
//
//            break;
#ifdef ENABLE_DTMF_CALLING
            // case MENU_D_LIST: // DTMF Contact List disabled
            //     gIsDtmfContactValid = DTMF_GetContact((int) gSubMenuSelection - 1, Contact);
            //     if (!gIsDtmfContactValid)
            //         strcpy(String, "NULL");
            //     else
            //         memcpy(String, Contact, 8);
            //     break;
#endif
#ifdef ENABLE_MDC1200
        case MENU_MDC_PREAMBLE_DURATION:
            strcpy(String, gSubMenu_MDC_PREAMBLE_DURATION[gSubMenuSelection]);
            break;

        case MENU_MDC_PREAMBLE_WHEN: {
            const char *options[] = {"Pre", "Post", "Both"};
            strcpy(String, options[gSubMenuSelection]);
            break;
        }
#endif

//        case MENU_VOL:
//            sprintf(String, "%u.%02uV\n%u%%",
//                    gBatteryVoltageAverage / 100, gBatteryVoltageAverage % 100,
//                    BATTERY_VoltsToPercent(gBatteryVoltageAverage));
//            break;

        case MENU_RESET:
            strcpy(String, gSubMenu_RESET[gSubMenuSelection]);


            break;

        case MENU_F_LOCK:
//            if (!gIsInSubMenu && gUnlockAllTxConfCnt > 0&& gUnlockAllTxConfCnt < 10)
//                strcpy(String, "READ\nMANUAL");
//
//            else
            strcpy(String, gSubMenu_F_LOCK[gSubMenuSelection]);


            break;

#ifdef ENABLE_F_CAL_MENU
            case MENU_F_CALI:
                {
                    const uint32_t value   = 22656 + gSubMenuSelection;
                    const uint32_t xtal_Hz = (0x4f0000u + value) * 5;

                    writeXtalFreqCal(gSubMenuSelection, false);

                    sprintf(String, "%d\n%u.%06u\nMHz",
                        gSubMenuSelection,
                        xtal_Hz / 1000000, xtal_Hz % 1000000);
                }
                break;
#endif

        case MENU_BATCAL: {
            const uint16_t vol = (uint32_t) gBatteryVoltageAverage * gBatteryCalibration[3] / gSubMenuSelection;
            sprintf(String, "%u.%02uV\n%u", vol / 100, vol % 100, gSubMenuSelection);
            break;
        }

        case MENU_TX_CAL: {
            const char *power = gTxVfo->OUTPUT_POWER == OUTPUT_POWER_HIGH ? "H" :
                                gTxVfo->OUTPUT_POWER == OUTPUT_POWER_MID ? "M" : "L";
            sprintf(String, "%s:%u\n%u%%", power, gSubMenuSelection, (gSubMenuSelection * 100u) / 150u);
            break;
        }

        case MENU_BATTYP:
            strcpy(String, gSubMenu_BATTYP[gSubMenuSelection]);

            break;

        case MENU_RX_LM:
            strcpy(String, gSubMenu_RX_LM[gSubMenuSelection]);

            break;

        case MENU_TALK_PERMIT_TONE:
            strcpy(String, gSubMenu_TALK_PERMIT_TONE[gSubMenuSelection]);

            break;

#ifdef ENABLE_CUSTOM_SIDEFUNCTIONS
            case MENU_F1SHRT:
            case MENU_F1LONG:
            case MENU_F2SHRT:
            case MENU_F2LONG:
            case MENU_MLONG:
                strcpy(String, gSubMenu_SIDEFUNCTIONS[gSubMenuSelection].name);
                break;
#endif

    }

    if (!already_printed) {    // we now do multi-line text in a single string

        unsigned int y;
        unsigned int lines = 1;
        unsigned int len = strlen(String);
        bool small = false;

        if (len > 0) {
            // count number of lines
            for (i = 0; i < len; i++) {
                if (String[i] == '\n' && i < (len - 1)) {    // found new line char
                    lines += 1;
                    String[i] = 0;  // null terminate the line
                }
            }

            if (lines > 3) {    // use small text
                small = true;
                if (lines > 7)
                    lines = 7;
            }

            // center vertically'ish
            if (small)
                y = 3 - ((lines + 0) / 2);  // untested
            else
                y = 2 - ((lines + 0) / 2);

            // draw the text lines
            for (i = 0; i < len && lines > 0; lines--) {
                if (small)
                    UI_PrintStringSmall(String + i, menu_item_x1, menu_item_x2, y + 1);
                else
                    UI_PrintStringSmall(String + i, menu_item_x1, menu_item_x2, y + 1);

                // look for start of next line
                while (i < len && String[i] != 0 && String[i] != '\n')
                    i++;

                // hop over the null term char(s)
                while (i < len && (String[i] == 0 || String[i] == '\n'))
                    i++;

                y += small ? 1 : 2;
            }
        }
    }

    if (UI_MENU_GetCurrentMenuId() == MENU_SLIST1 || UI_MENU_GetCurrentMenuId() == MENU_SLIST2) {
        i = (UI_MENU_GetCurrentMenuId() == MENU_SLIST1) ? 0 : 1;

        char *pPrintStr = String;

        if (gSubMenuSelection < 0) {
            pPrintStr = "NULL";
        } else {
            UI_GenerateChannelStringEx(String, true, gSubMenuSelection);
            pPrintStr = String;
        }

        // channel number
        UI_PrintStringSmall(pPrintStr, menu_item_x1 - 12, menu_item_x2, 2);

        SETTINGS_FetchChannelName(String, gSubMenuSelection);
        pPrintStr = String[0] ? String : "--";


// channel name and scan-list
        if (gSubMenuSelection < 0 || !gEeprom.SCAN_LIST_ENABLED[i]) {
            UI_PrintStringSmall(pPrintStr, menu_item_x1 - 12, menu_item_x2, 4);
        } else {
            UI_PrintStringSmall(pPrintStr, menu_item_x1 - 12, menu_item_x2, 4);

//
//            if (IS_MR_CHANNEL(gEeprom.SCANLIST_PRIORITY_CH1[i])) {
//                sprintf(String, "PRI%d:%u", 1, gEeprom.SCANLIST_PRIORITY_CH1[i] + 1);
//                UI_PrintStringSmall(String, menu_item_x1 - 12, menu_item_x2, 3);
//            }
//
//            if (IS_MR_CHANNEL(gEeprom.SCANLIST_PRIORITY_CH2[i])) {
//                sprintf(String, "PRI%d:%u", 2, gEeprom.SCANLIST_PRIORITY_CH2[i] + 1);
//                UI_PrintStringSmall(String, menu_item_x1 - 12, menu_item_x2, 6);
//            }

        }
    }


    if ((UI_MENU_GetCurrentMenuId() == MENU_R_CTCS || UI_MENU_GetCurrentMenuId() == MENU_R_DCS) && gCssBackgroundScan)
        //STR_SCAN
        UI_PrintStringSmall(STR_SCAN, menu_item_x1, menu_item_x2, 5);

//
//    if (UI_MENU_GetCurrentMenuId() == MENU_UPCODE)
//        if (strlen(gEeprom.DTMF_UP_CODE) > 12)
//            UI_PrintStringSmall(gEeprom.DTMF_UP_CODE + 12, menu_item_x1, menu_item_x2, 5);
//
//    if (UI_MENU_GetCurrentMenuId() == MENU_DWCODE)
//        if (strlen(gEeprom.DTMF_DOWN_CODE) > 12)
//            UI_PrintStringSmall(gEeprom.DTMF_DOWN_CODE + 12, menu_item_x1, menu_item_x2, 5);
#ifdef ENABLE_DTMF_CALLING
    // if (UI_MENU_GetCurrentMenuId() == MENU_D_LIST && gIsDtmfContactValid) { // disabled
    //     Contact[11] = 0;
    //     memcpy(&gDTMF_ID, Contact + 8, 4);
    //     sprintf(String, "ID:%4s", gDTMF_ID);
    //     UI_PrintStringSmall(String, menu_item_x1, menu_item_x2, 5);
    // }
#endif
    if (UI_MENU_GetCurrentMenuId() == MENU_R_CTCS ||
        UI_MENU_GetCurrentMenuId() == MENU_T_CTCS ||
        UI_MENU_GetCurrentMenuId() == MENU_R_DCS ||
        UI_MENU_GetCurrentMenuId() == MENU_T_DCS
        // || UI_MENU_GetCurrentMenuId() == MENU_D_LIST // disabled
            ) {

        sprintf(String, "%2d", gSubMenuSelection);
        UI_PrintStringSmall(String, 105, 0, 1);//small
    }

    if ((UI_MENU_GetCurrentMenuId() == MENU_RESET ||
         UI_MENU_GetCurrentMenuId() == MENU_MEM_CH ||
         UI_MENU_GetCurrentMenuId() == MENU_MEM_NAME ||
         #ifdef ENABLE_MDC1200_EDIT

         UI_MENU_GetCurrentMenuId() == MENU_MDC_ID ||
         #endif
         UI_MENU_GetCurrentMenuId() == MENU_DEL_CH) && gAskForConfirmation) {    // display confirmation
        char *pPrintStr = (gAskForConfirmation == 1) ? "SURE?" : "WAIT!";
        if (UI_MENU_GetCurrentMenuId() == MENU_MEM_CH || UI_MENU_GetCurrentMenuId() == MENU_MEM_NAME ||
             UI_MENU_GetCurrentMenuId() == MENU_DEL_CH)
            UI_PrintStringSmall(pPrintStr, menu_item_x1 - 12, menu_item_x2, 5);
        else UI_PrintStringSmall(pPrintStr, menu_item_x1, menu_item_x2, 5);

        gRequestSaveSettings = 1;

    }

    ST7565_BlitFullScreen();
}

//
