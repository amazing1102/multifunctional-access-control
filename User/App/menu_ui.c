#include "menu_ui.h"

#include <stdio.h>
#include <string.h>

#include "oled.h"
#include "rtc.h"

static void MenuUI_ShowUtf8Line(uint8_t Line, const char *Text)
{
    OLED_ClearLine(Line);
    if (Text != 0)
    {
        OLED_ShowUTF8String(Line, 0U, Text);
    }
}

static uint8_t MenuUI_GetUtf8TextWidth(const char *Text)
{
    uint8_t Width = 0U;
    const uint8_t *Ptr;

    if (Text == 0)
    {
        return 0U;
    }

    Ptr = (const uint8_t *)Text;
    while (*Ptr != '\0')
    {
        if (*Ptr < 0x80U)
        {
            Width = (uint8_t)(Width + 8U);
            Ptr++;
        }
        else if ((*Ptr & 0xE0U) == 0xC0U)
        {
            Width = (uint8_t)(Width + 16U);
            Ptr = (const uint8_t *)(Ptr + 2U);
        }
        else if ((*Ptr & 0xF0U) == 0xE0U)
        {
            Width = (uint8_t)(Width + 16U);
            Ptr = (const uint8_t *)(Ptr + 3U);
        }
        else if ((*Ptr & 0xF8U) == 0xF0U)
        {
            Width = (uint8_t)(Width + 16U);
            Ptr = (const uint8_t *)(Ptr + 4U);
        }
        else
        {
            Width = (uint8_t)(Width + 8U);
            Ptr++;
        }
    }

    return Width;
}

static void MenuUI_ShowCenteredUtf8Line(uint8_t Line, const char *Text)
{
    uint8_t Width;
    uint8_t X;

    Width = MenuUI_GetUtf8TextWidth(Text);
    if (Width >= 128U)
    {
        X = 0U;
    }
    else
    {
        X = (uint8_t)((128U - Width) / 2U);
    }

    OLED_ClearLine(Line);
    if (Text != 0)
    {
        OLED_ShowUTF8String(Line, X, Text);
    }
}

void MenuUI_Init(void)
{
    OLED_Init();
}

void MenuUI_DisplayOn(void)
{
    OLED_DisplayOn();
}

void MenuUI_DisplayOff(void)
{
    OLED_DisplayOff();
}

void MenuUI_Clear(void)
{
    OLED_Clear();
}

void MenuUI_ShowBootScreen(void)
{
    OLED_Clear();
    MenuUI_ShowUtf8Line(1U, UI_ZH_SYSTEM_START);
    MenuUI_ShowUtf8Line(2U, UI_ZH_SELF_TEST);
    MenuUI_ShowUtf8Line(3U, UI_ZH_FP_FLASH_RFID);
    MenuUI_ShowUtf8Line(4U, UI_ZH_PLEASE_WAIT);
}

void MenuUI_ShowSelfTestResult(uint8_t FpOk, uint8_t RfidOk, uint8_t FlashOk)
{
    OLED_Clear();
    MenuUI_ShowUtf8Line(1U, UI_ZH_BOOT_DONE);
    MenuUI_ShowUtf8Line(2U, FpOk != 0U ? UI_ZH_FP_OK : UI_ZH_FP_FAIL);
    MenuUI_ShowUtf8Line(3U, RfidOk != 0U ? UI_ZH_RFID_OK : UI_ZH_RFID_FAIL);
    MenuUI_ShowUtf8Line(4U, FlashOk != 0U ? UI_ZH_FLASH_OK : UI_ZH_FLASH_FAIL);
}

void MenuUI_ShowIdleScreen(const char *IdleHint)
{
    OLED_Clear();
    MenuUI_ShowUtf8Line(1U, UI_ZH_STANDBY);
    MenuUI_ShowUtf8Line(2U, (IdleHint != 0) ? IdleHint : UI_ZH_IDLE_HINT);
}

void MenuUI_UpdateIdleClock(uint8_t FaultMask)
{
    AppRTC_DateTime_t DateTime;
    char DateBuffer[16];
    char TimeBuffer[16];
    char LineBuffer[48];

    if (AppRTC_IsReady() == 0U)
    {
        MenuUI_ShowUtf8Line(3U, UI_ZH_RTC_OFFLINE);
        if (FaultMask != 0U)
        {
            MenuUI_ShowFaultBanner(FaultMask);
        }
        else
        {
            MenuUI_ShowUtf8Line(4U, UI_ZH_CHECK_MODULES);
        }
        return;
    }

    AppRTC_GetDateTime(&DateTime);
    AppRTC_FormatDate(&DateTime, DateBuffer, sizeof(DateBuffer));
    AppRTC_FormatTime(&DateTime, TimeBuffer, sizeof(TimeBuffer));
    if (FaultMask != 0U)
    {
        MenuUI_ShowUtf8Line(3U, DateBuffer);
        MenuUI_ShowFaultBanner(FaultMask);
    }
    else
    {
        MenuUI_ShowUtf8Line(3U, DateBuffer);
        MenuUI_ShowUtf8Line(4U, TimeBuffer);
    }
}

void MenuUI_ShowAuthScreen(void)
{
    OLED_Clear();
    MenuUI_ShowUtf8Line(1U, UI_ZH_AUTH);
    MenuUI_ShowUtf8Line(2U, UI_ZH_INPUT_PASSWORD);
    MenuUI_ShowUtf8Line(3U, ">");
    MenuUI_ShowUtf8Line(4U, UI_ZH_CONFIRM_CLEAR);
}

void MenuUI_UpdateInputDisplay(const uint8_t *InputBuffer)
{
    OLED_ClearLine(3U);
    OLED_ShowUTF8String(3U, 0U, ">");
    if ((InputBuffer != 0) && (InputBuffer[0] != '\0'))
    {
        OLED_ShowUTF8String(3U, 8U, (const char *)InputBuffer);
    }
}

void MenuUI_ShowLines(const char *Line1, const char *Line2, const char *Line3, const char *Line4)
{
    OLED_Clear();
    MenuUI_ShowUtf8Line(1U, Line1);
    MenuUI_ShowUtf8Line(2U, Line2);
    MenuUI_ShowUtf8Line(3U, Line3);
    MenuUI_ShowUtf8Line(4U, Line4);
}

void MenuUI_ShowUnlockedScreen(const char *TopLine)
{
    OLED_Clear();
    MenuUI_ShowUtf8Line(1U, TopLine);
    MenuUI_ShowCenteredUtf8Line(2U, UI_ZH_UNLOCKED);
    MenuUI_ShowCenteredUtf8Line(3U, UI_ZH_WELCOME_HOME);
}

void MenuUI_ShowAdminMenu(uint8_t Page)
{
    OLED_Clear();
    if (Page == 0U)
    {
        MenuUI_ShowUtf8Line(1U, UI_ZH_ADMIN_MODE);
        MenuUI_ShowUtf8Line(2U, UI_ZH_ADMIN_MENU_1);
        MenuUI_ShowUtf8Line(3U, UI_ZH_ADMIN_MENU_2);
        MenuUI_ShowUtf8Line(4U, UI_ZH_ADMIN_MENU_3);
    }
    else
    {
        MenuUI_ShowUtf8Line(1U, UI_ZH_ADMIN_MODE);
        MenuUI_ShowUtf8Line(2U, UI_ZH_ADMIN_MENU_4);
        MenuUI_ShowUtf8Line(3U, UI_ZH_ADMIN_MENU_5);
        MenuUI_ShowUtf8Line(4U, "#\xE8\xBF\x94\xE5\x9B\x9E");  /* #返回 */
    }
}

void MenuUI_ShowAdminPrompt(uint8_t AdminAction, const uint8_t *InputBuffer)
{
    OLED_Clear();

    if (AdminAction == 1U)
    {
        MenuUI_ShowUtf8Line(1U, UI_ZH_DEL_FINGERPRINT);
        MenuUI_ShowUtf8Line(2U, UI_ZH_INPUT_ID);
        OLED_ShowUTF8String(3U, 0U, UI_ZH_ID_PREFIX);
        if ((InputBuffer != 0) && (InputBuffer[0] != '\0'))
        {
            OLED_ShowUTF8String(3U, 48U, (const char *)InputBuffer);
        }
        MenuUI_ShowUtf8Line(4U, UI_ZH_CONFIRM_BACK);
    }
    else if (AdminAction == 2U)
    {
        MenuUI_ShowUtf8Line(1U, UI_ZH_ADD_CARD);
        MenuUI_ShowUtf8Line(2U, UI_ZH_SWIPE_CARD);
        MenuUI_ShowUtf8Line(4U, UI_ZH_BACK);
    }
    else if (AdminAction == 3U)
    {
        MenuUI_ShowUtf8Line(1U, UI_ZH_DEL_CARD);
        MenuUI_ShowUtf8Line(2U, UI_ZH_SWIPE_CARD);
        MenuUI_ShowUtf8Line(4U, UI_ZH_BACK);
    }
    else if (AdminAction == 4U)
    {
        MenuUI_ShowUtf8Line(1U, UI_ZH_NEW_USER_PWD);
        MenuUI_ShowUtf8Line(2U, UI_ZH_MAX_10_DIGITS);
        OLED_ShowUTF8String(3U, 0U, ">");
        if ((InputBuffer != 0) && (InputBuffer[0] != '\0'))
        {
            OLED_ShowUTF8String(3U, 8U, (const char *)InputBuffer);
        }
        MenuUI_ShowUtf8Line(4U, UI_ZH_CONFIRM_BACK);
    }
    else if (AdminAction == 5U)
    {
        MenuUI_ShowUtf8Line(1U, UI_ZH_NEW_ADMIN_PWD);
        MenuUI_ShowUtf8Line(2U, UI_ZH_MAX_10_DIGITS);
        OLED_ShowUTF8String(3U, 0U, ">");
        if ((InputBuffer != 0) && (InputBuffer[0] != '\0'))
        {
            OLED_ShowUTF8String(3U, 8U, (const char *)InputBuffer);
        }
        MenuUI_ShowUtf8Line(4U, UI_ZH_CONFIRM_BACK);
    }
    else if (AdminAction == 6U)
    {
        MenuUI_ShowUtf8Line(1U, UI_ZH_SET_TIME);
        MenuUI_ShowUtf8Line(2U, UI_ZH_INPUT_TIME);
        OLED_ShowUTF8String(3U, 0U, ">");
        if ((InputBuffer != 0) && (InputBuffer[0] != '\0'))
        {
            OLED_ShowUTF8String(3U, 8U, (const char *)InputBuffer);
        }
        MenuUI_ShowUtf8Line(4U, UI_ZH_CONFIRM_BACK);
    }
    else if (AdminAction == 7U)
    {
        MenuUI_ShowUtf8Line(1U, UI_ZH_SET_DATE);
        MenuUI_ShowUtf8Line(2U, UI_ZH_INPUT_DATE);
        OLED_ShowUTF8String(3U, 0U, ">");
        if ((InputBuffer != 0) && (InputBuffer[0] != '\0'))
        {
            OLED_ShowUTF8String(3U, 8U, (const char *)InputBuffer);
        }
        MenuUI_ShowUtf8Line(4U, UI_ZH_CONFIRM_BACK);
    }
}

void MenuUI_ShowLockoutScreen(uint32_t SecondsLeft)
{
    (void)SecondsLeft;
    OLED_Clear();
    MenuUI_ShowUtf8Line(1U, UI_ZH_LOCKOUT);
    MenuUI_ShowUtf8Line(2U, UI_ZH_TOO_MANY_FAILS);
    MenuUI_ShowUtf8Line(3U, "\xE5\x88\xB7\xE5\x8D\xA1/\xE6\x8C\x87\xE7\xBA\xB9\xE4\xBB\x8D\xE5\x8F\xAF\xE7\x94\xA8");  /* 刷卡/指纹仍可用 */
    MenuUI_ShowUtf8Line(4U, UI_ZH_AUTO_RECOVER);
}

void MenuUI_ShowFaultScreen(void)
{
    OLED_Clear();
    MenuUI_ShowUtf8Line(1U, UI_ZH_FAULT);
    MenuUI_ShowUtf8Line(2U, UI_ZH_DEGRADED_MODE);
    MenuUI_ShowUtf8Line(3U, UI_ZH_CHECK_MODULES);
}

void MenuUI_ShowEnrollScreen(uint8_t EnrollStage, uint16_t TargetPage)
{
    char Buf[8];

    OLED_Clear();
    MenuUI_ShowUtf8Line(1U, UI_ZH_ADMIN_ENROLL);

    switch (EnrollStage)
    {
        case 1U:
            MenuUI_ShowUtf8Line(2U, UI_ZH_PLACE_FIRST_FINGER);
            break;

        case 2U:
            MenuUI_ShowUtf8Line(2U, UI_ZH_CAPTURE_FIRST);
            break;

        case 3U:
            MenuUI_ShowUtf8Line(2U, UI_ZH_WAIT_RELEASE_200MS);
            break;

        case 4U:
            MenuUI_ShowUtf8Line(2U, UI_ZH_PLACE_SECOND_FINGER);
            break;

        case 5U:
            MenuUI_ShowUtf8Line(2U, UI_ZH_CAPTURE_SECOND);
            break;

        case 6U:
            MenuUI_ShowUtf8Line(2U, UI_ZH_REG_MODEL);
            break;

        case 7U:
            MenuUI_ShowUtf8Line(2U, UI_ZH_DUP_CHECK);
            break;

        case 8U:
            MenuUI_ShowUtf8Line(2U, UI_ZH_STORE_MODEL);
            break;

        default:
            MenuUI_ShowUtf8Line(2U, UI_ZH_WAITING);
            break;
    }

    OLED_ShowUTF8String(3U, 0U, UI_ZH_ID_PREFIX);
    sprintf(Buf, "%04u", TargetPage);
    OLED_ShowUTF8String(3U, 56U, Buf);
    MenuUI_ShowUtf8Line(4U, UI_ZH_CANCEL);
}

void MenuUI_ShowLogPage(const char *EventName, uint8_t Result,
                        uint8_t Hour, uint8_t Minute, uint8_t Second,
                        uint16_t Year, uint8_t Month, uint8_t Day)
{
    char Buf[32];

    OLED_Clear();
    MenuUI_ShowUtf8Line(1U, UI_ZH_LOG_DETAIL);

    sprintf(Buf, "%s:%s", EventName != 0 ? EventName : "",
            Result != 0U ? UI_ZH_SUCCESS : UI_ZH_FAIL);
    MenuUI_ShowUtf8Line(2U, Buf);

    if (Year == 0U)
    {
        MenuUI_ShowUtf8Line(3U, UI_ZH_NO_TIMESTAMP);
        MenuUI_ShowUtf8Line(4U, UI_ZH_RTC_OFFLINE);
    }
    else
    {
        sprintf(Buf, "%04u/%02u/%02u", Year, Month, Day);
        MenuUI_ShowUtf8Line(3U, Buf);
        sprintf(Buf, "%02u:%02u:%02u", Hour, Minute, Second);
        MenuUI_ShowUtf8Line(4U, Buf);
    }
}

void MenuUI_ShowLogEmpty(void)
{
    OLED_Clear();
    MenuUI_ShowUtf8Line(1U, UI_ZH_LOG_DETAIL);
    MenuUI_ShowUtf8Line(2U, UI_ZH_NO_LOGS_FOUND);
}

void MenuUI_ShowFaultBanner(uint8_t FaultMask)
{
    char Buf[32] = "";

    if (FaultMask == 0U)
    {
        return;
    }

    if ((FaultMask & 0x01U) != 0U) { strcat(Buf, UI_ZH_FAULT_FP); }
    if ((FaultMask & 0x02U) != 0U) { strcat(Buf, UI_ZH_FAULT_RFID); }
    if ((FaultMask & 0x04U) != 0U) { strcat(Buf, UI_ZH_FAULT_FLASH); }
    if ((FaultMask & 0x08U) != 0U) { strcat(Buf, UI_ZH_FAULT_RTC); }
    if ((FaultMask & 0x10U) != 0U) { strcat(Buf, UI_ZH_FAULT_VOICE); }
    if ((FaultMask & 0x20U) != 0U) { strcat(Buf, UI_ZH_FAULT_BT); }

    OLED_ClearLine(4U);
    OLED_ShowUTF8String(4U, 0U, Buf);
}
