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
#include "driver/uart.h"
#include "ui_strings.h"
#include <string.h>
#include "driver/keyboard.h"
#include "driver/eeprom.h"
#include "driver/st7565.h"
#include "external/printf/printf.h"
#include "helper/battery.h"
#include "settings.h"
#include "misc.h"
#include "ui/helper.h"
#include "ui/welcome.h"
#include "ui/status.h"
#include "version.h"
#include "driver/system.h"
//void UI_DisplayReleaseKeys(BOOT_Mode_t BootMode) {
//    memset(gStatusLine, 0, sizeof(gStatusLine));
//    memset(gFrameBuffer, 0, sizeof(gFrameBuffer));
//
//    if (BootMode == BOOT_MODE_F_LOCK) {
//        //解锁，STR_ALL按键
//        UI_PrintStringSmall(解锁, 0, 127, 1);
//        UI_PrintStringSmall(STR_ALL按键, 0, 127, 3);
//        ST7565_BlitStatusLine();  // blank status line
//        ST7565_BlitFullScreen();
//    }
//
//
//}

void UI_DisplayWelcome(void) {

    char WelcomeString0[19] = {0};
    char WelcomeString1[19] = {0};

    memset(gStatusLine, 0, sizeof(gStatusLine));
    memset(gFrameBuffer, 0, sizeof(gFrameBuffer));
    ST7565_BlitStatusLine();
    ST7565_BlitFullScreen();

    if (gEeprom.BACKLIGHT_TIME > 0) {
        BACKLIGHT_SetBrightness(0);
        BACKLIGHT_TurnOn();
        for (uint8_t i = 1; i <= 30; i++) {
            BACKLIGHT_SetBrightness((gEeprom.BACKLIGHT_MAX * i) / 30);
            SYSTEM_DelayMs(10);
        }
    }
#if ENABLE_CHINESE_FULL == 4

    if (gEeprom.POWER_ON_DISPLAY_MODE == POWER_ON_DISPLAY_MODE_MESSAGE) {

    uint8_t welcome_len[2];
    EEPROM_ReadBuffer(0x02024, welcome_len, 2) ;
    welcome_len[0]=welcome_len[0]>18?0:welcome_len[0];
    welcome_len[1]=welcome_len[1]>18?0:welcome_len[1];
    EEPROM_ReadBuffer(0x02000, WelcomeString0, welcome_len[0]) ;
    EEPROM_ReadBuffer(0x02012, WelcomeString1, welcome_len[1]);

#elif ENABLE_CHINESE_FULL == 0

    EEPROM_ReadBuffer(0x0EB0, WelcomeString0, 16);
    EEPROM_ReadBuffer(0x0EC0, WelcomeString1, 16);

#endif
    UI_PrintStringSmall(WelcomeString0, 0, 127, 0);
    UI_PrintStringSmall(WelcomeString1, 0, 127, 2);

    UI_PrintStringSmall(Version, 0, 127, 4);

    for (uint8_t i = 0; i < 128; i++)
    {
        gFrameBuffer[3][i] ^= 0x80;
        gFrameBuffer[4][i] ^= 0xFF;
    }

    sprintf(WelcomeString1, "%u.%02uV %u%%",
            gBatteryVoltageAverage / 100,
            gBatteryVoltageAverage % 100,
            BATTERY_VoltsToPercent(gBatteryVoltageAverage));
    UI_PrintStringSmall(WelcomeString1, 0, 127, 6);
#if ENABLE_CHINESE_FULL == 4
    }
    else if(gEeprom.POWER_ON_DISPLAY_MODE == POWER_ON_DISPLAY_MODE_PIC)
        {
             EEPROM_ReadBuffer( 0x02080, gStatusLine, 128);
    for (int i = 0; i < 7; ++i)  EEPROM_ReadBuffer(0x02080+128+128*i, &gFrameBuffer[i], 128);
        }

#endif

    ST7565_BlitStatusLine();
    ST7565_BlitFullScreen();
}

