#include "access_core.h"

#include <stdio.h>
#include <string.h>

#include "as608.h"
#include "bluetooth.h"
#include "buzzer.h"
#include "delay.h"
#include "JQ8900.h"
#include "keypad.h"
#include "led.h"
#include "rc522.h"
#include "rtc.h"
#include "my_spi.h"
#include "servo.h"
#include "timer.h"
#include "w25q64.h"
#include "iwdg.h"
#include "menu_ui.h"
#include "oled.h"

#define ACCESS_CONTROL_STORAGE_SECTOR_A          0x000000U
#define ACCESS_CONTROL_STORAGE_SECTOR_B          W25Q64_SECTOR_SIZE
#define ACCESS_CONTROL_STORAGE_MAGIC             0x4F4C4341UL /* "ACLO" */
#define ACCESS_CONTROL_STORAGE_VERSION           0x0002U
/* Serialized flash image size, not sizeof(AccessConfig_t). */
#define ACCESS_CONTROL_STORAGE_IMAGE_SIZE         196U
#define ACCESS_CONTROL_LOG_SECTOR_FIRST           2U
#define ACCESS_CONTROL_LOG_SECTOR_COUNT           8U
#define ACCESS_CONTROL_LOG_RECORD_SIZE           32U
#define ACCESS_CONTROL_LOG_ENTRIES_PER_SECTOR    (W25Q64_SECTOR_SIZE / ACCESS_CONTROL_LOG_RECORD_SIZE)
#define ACCESS_CONTROL_LOG_MAGIC                 0x474F4C41UL /* "ALOG" */
#define ACCESS_CONTROL_LOG_VERSION               0x0001U
#define ACCESS_CONTROL_AS608_NO_FINGER            0x02U

#define ACCESS_CONTROL_ENROLL_TIMEOUT_MS         60000U
#define ACCESS_CONTROL_ENROLL_RELEASE_DEBOUNCE_MS  200U

#define ACCESS_CONTROL_DEFAULT_USER_PASSWORD      123456UL
#define ACCESS_CONTROL_DEFAULT_ADMIN_PASSWORD     888888UL

#define ACCESS_CONTROL_MAX_CARD_SLOTS             8U
#define ACCESS_CONTROL_FINGERPRINT_BITMAP_BYTES   128U
#define ACCESS_CONTROL_FINGERPRINT_MAX_ID         1023U
#define ACCESS_CONTROL_UNLOCK_HISTORY_MAX         30U
/* Keep passwords within uint32_t without any decimal overflow ambiguity. */
#define ACCESS_CONTROL_MAX_PASSWORD_LEN           10U
#define ACCESS_CONTROL_UNLOCK_HOLD_MS            5000U
#define ACCESS_CONTROL_IDLE_SLEEP_MS            30000U
#define ACCESS_CONTROL_LOCKOUT_MS              180000U
#define ACCESS_CONTROL_MAX_FAILURES               5U

#define ACCESS_CONTROL_CARD_SCAN_PERIOD_MS         200U
#define ACCESS_CONTROL_FP_SCAN_PERIOD_MS           800U
#define ACCESS_CONTROL_DISPLAY_PERIOD_MS          1000U
#define ACCESS_CONTROL_LOWPOWER_WAKE_SECONDS         1U
#define ACCESS_CONTROL_LOWPOWER_ACTIVE_WINDOW_MS   250U
#define ACCESS_CONTROL_USE_WKUP_PIN                  1U
#define ACCESS_CONTROL_VOICE_VOLUME                 15U

/*
 * JQ8900-16P bundled voice files are stored in the module's SPI flash.
 * The manual defines disk 0x02 as FLASH and 0x07 as "specified track play".
 */
#define ACCESS_CONTROL_VOICE_DISK                  JQ8900_DISK_FLASH

/* Voice track numbers on JQ8900 storage */
#define ACCESS_CONTROL_TRACK_BOOT                   1U
#define ACCESS_CONTROL_TRACK_AUTO_LOCK              2U
#define ACCESS_CONTROL_TRACK_MODULE_FAULT           3U

#define ACCESS_CONTROL_TRACK_WELCOME                4U
#define ACCESS_CONTROL_TRACK_KEYPAD_UNLOCK          5U
#define ACCESS_CONTROL_TRACK_FINGERPRINT_UNLOCK     6U
#define ACCESS_CONTROL_TRACK_CARD_UNLOCK            7U

#define ACCESS_CONTROL_TRACK_PASSWORD_FAIL          8U
#define ACCESS_CONTROL_TRACK_FINGERPRINT_FAIL       9U
#define ACCESS_CONTROL_TRACK_CARD_FAIL              10U
#define ACCESS_CONTROL_TRACK_LOCKED                 11U

#define ACCESS_CONTROL_TRACK_ADMIN_LOGIN            12U
#define ACCESS_CONTROL_TRACK_ENROLL_FP_SUCCESS      13U
#define ACCESS_CONTROL_TRACK_ENROLL_FP_FAIL         14U
#define ACCESS_CONTROL_TRACK_ADD_CARD_SUCCESS       15U
#define ACCESS_CONTROL_TRACK_ADD_CARD_FAIL          16U
#define ACCESS_CONTROL_TRACK_DELETE_SUCCESS         17U
#define ACCESS_CONTROL_TRACK_DELETE_FAIL            18U
#define ACCESS_CONTROL_TRACK_SETTING_SUCCESS        19U
#define ACCESS_CONTROL_PASSWORD_HINT                UI_ZH_MAX_10_DIGITS
#define ACCESS_CONTROL_IDLE_HINT                    UI_ZH_IDLE_HINT
#define ACCESS_CONTROL_BT_CMD_SET_TIME            0x19U
#define ACCESS_CONTROL_BT_RSP_SET_TIME            0x99U

typedef struct
{
    uint32_t Magic;
    uint16_t Version;
    uint16_t NextFingerprintId;
    uint32_t UserPassword;
    uint32_t AdminPassword;
    uint8_t CardCount;
    uint8_t IsLockedOut;
    uint8_t Reserved[2];
    uint8_t CardUids[ACCESS_CONTROL_MAX_CARD_SLOTS][5];
    uint8_t FingerprintBitmap[ACCESS_CONTROL_FINGERPRINT_BITMAP_BYTES];
} AccessConfig_t;

static AccessConfig_t s_config;
static AccessState_t s_state = ACCESS_STATE_INIT;
static AuthSource_t s_auth_source = AUTH_SOURCE_NONE;
static AdminAction_t s_admin_action = ADMIN_ACTION_NONE;

static uint8_t s_flash_available = 0U;
static uint8_t s_fp_available = 0U;
static uint8_t s_rfid_available = 0U;
static uint8_t s_voice_awake = 0U;
static uint8_t s_fault_mask = ACCESS_FAULT_NONE;

static uint16_t s_fp_capacity = 0U;
static uint8_t s_failure_count = 0U;
static uint8_t s_password_failure_count = 0U;
static uint8_t s_input_buffer[ACCESS_CONTROL_MAX_PASSWORD_LEN + 1U];
static uint8_t s_input_length = 0U;

static uint32_t s_storage_sequence = 0U;
static uint8_t s_storage_active_sector = 0U;

static uint32_t s_state_deadline = 0U;
static uint32_t s_last_activity = 0U;
static uint32_t s_last_refresh = 0U;
static uint32_t s_last_card_scan = 0U;
static uint32_t s_last_fp_scan = 0U;
static uint32_t s_lowpower_wake_deadline = 0U;
static uint32_t s_log_sequence = 0U;
static uint16_t s_log_sector = ACCESS_CONTROL_LOG_SECTOR_FIRST;
static uint16_t s_log_entry = 0U;
static uint8_t s_log_sector_needs_erase = 1U;
static uint8_t s_log_available = 0U;
static uint8_t s_boot_prompt_played = 0U;

static EnrollStage_t s_enroll_stage = ENROLL_STAGE_IDLE;
static uint8_t s_admin_menu_page = 0U;
static uint16_t s_enroll_target_page = 0U;
static uint32_t s_enroll_deadline = 0U;
static uint32_t s_enroll_release_deadline = 0U;
static uint8_t s_lowpower_window_active = 0U;

static AS608_SysPara_t s_fp_syspara;

static void AccessControl_WakeVoiceModule(void);
static void AccessControl_SelectVoiceDisk(void);
static void AccessControl_ResetEnrollment(void);
static void AccessControl_AbortEnrollmentToAdmin(void);
static void AccessControl_ReturnToAdminMenu(void);
static void AccessControl_ForceSpiCsHigh(void);
static void AccessControl_ConfigWakeupPin(void);
static void AccessControl_EnterLowPowerStopMode(void);
static void AccessControl_ResumeFromStopMode(void);
static void AccessControl_InitLogStorage(void);
static uint8_t AccessControl_LogEvent(AccessLogEvent_t Event, uint8_t Result, uint32_t Value, uint32_t Extra);
static void AccessControl_SetLockoutFlag(uint8_t Enable);
static uint16_t AccessControl_GetFingerprintCapacity(void);

static uint32_t AccessControl_Now(void)
{
    return GetSystemTick();
}

static uint32_t AccessControl_CalcChecksum(const uint8_t *Data, uint32_t Length)
{
    uint32_t Hash;
    uint32_t i;

    Hash = 2166136261U;
    if (Data == 0)
    {
        return Hash;
    }

    for (i = 0U; i < Length; i++)
    {
        Hash ^= (uint32_t)Data[i];
        Hash *= 16777619U;
    }

    return Hash;
}

static void AccessControl_WriteU16(uint8_t *Buffer, uint32_t Offset, uint16_t Value)
{
    Buffer[Offset + 0U] = (uint8_t)(Value & 0xFFU);
    Buffer[Offset + 1U] = (uint8_t)((Value >> 8) & 0xFFU);
}

static void AccessControl_WriteU32(uint8_t *Buffer, uint32_t Offset, uint32_t Value)
{
    Buffer[Offset + 0U] = (uint8_t)(Value & 0xFFU);
    Buffer[Offset + 1U] = (uint8_t)((Value >> 8) & 0xFFU);
    Buffer[Offset + 2U] = (uint8_t)((Value >> 16) & 0xFFU);
    Buffer[Offset + 3U] = (uint8_t)((Value >> 24) & 0xFFU);
}

static uint16_t AccessControl_ReadU16(const uint8_t *Buffer, uint32_t Offset)
{
    return (uint16_t)((uint16_t)Buffer[Offset + 0U] | ((uint16_t)Buffer[Offset + 1U] << 8));
}

static uint32_t AccessControl_ReadU32(const uint8_t *Buffer, uint32_t Offset)
{
    return (uint32_t)Buffer[Offset + 0U] |
           ((uint32_t)Buffer[Offset + 1U] << 8) |
           ((uint32_t)Buffer[Offset + 2U] << 16) |
           ((uint32_t)Buffer[Offset + 3U] << 24);
}

static void AccessControl_FingerprintBitmapClearAll(void)
{
    memset(s_config.FingerprintBitmap, 0, sizeof(s_config.FingerprintBitmap));
}

static uint8_t AccessControl_FingerprintBitmapTest(uint16_t PageId)
{
    uint16_t Index;
    uint8_t Mask;

    if ((PageId == 0U) || (PageId > ACCESS_CONTROL_FINGERPRINT_MAX_ID))
    {
        return 0U;
    }

    Index = (uint16_t)(PageId >> 3);
    Mask = (uint8_t)(1U << (PageId & 0x07U));
    return (uint8_t)((s_config.FingerprintBitmap[Index] & Mask) != 0U);
}

static void AccessControl_FingerprintBitmapSet(uint16_t PageId)
{
    uint16_t Index;
    uint8_t Mask;

    if ((PageId == 0U) || (PageId > ACCESS_CONTROL_FINGERPRINT_MAX_ID))
    {
        return;
    }

    Index = (uint16_t)(PageId >> 3);
    Mask = (uint8_t)(1U << (PageId & 0x07U));
    s_config.FingerprintBitmap[Index] |= Mask;
}

static void AccessControl_FingerprintBitmapClear(uint16_t PageId)
{
    uint16_t Index;
    uint8_t Mask;

    if ((PageId == 0U) || (PageId > ACCESS_CONTROL_FINGERPRINT_MAX_ID))
    {
        return;
    }

    Index = (uint16_t)(PageId >> 3);
    Mask = (uint8_t)(1U << (PageId & 0x07U));
    s_config.FingerprintBitmap[Index] &= (uint8_t)~Mask;
}

static uint16_t AccessControl_FingerprintBitmapCount(void)
{
    uint16_t Count = 0U;
    uint16_t PageId;

    for (PageId = 1U; PageId <= ACCESS_CONTROL_FINGERPRINT_MAX_ID; PageId++)
    {
        if (AccessControl_FingerprintBitmapTest(PageId) != 0U)
        {
            Count++;
        }
    }

    return Count;
}

static uint16_t AccessControl_FindNextFingerprintPage(uint16_t StartPage)
{
    uint16_t PageId;
    uint16_t Capacity;

    Capacity = AccessControl_GetFingerprintCapacity();
    if (Capacity > ACCESS_CONTROL_FINGERPRINT_MAX_ID)
    {
        Capacity = ACCESS_CONTROL_FINGERPRINT_MAX_ID;
    }

    if (Capacity == 0U)
    {
        return 0U;
    }

    if ((StartPage == 0U) || (StartPage > Capacity))
    {
        StartPage = 1U;
    }

    for (PageId = StartPage; PageId <= Capacity; PageId++)
    {
        if (AccessControl_FingerprintBitmapTest(PageId) == 0U)
        {
            return PageId;
        }
    }

    for (PageId = 1U; PageId < StartPage; PageId++)
    {
        if (AccessControl_FingerprintBitmapTest(PageId) == 0U)
        {
            return PageId;
        }
    }

    return 0U;
}

static const char *AccessControl_GetUnlockSuccessText(AuthSource_t Source)
{
    switch (Source)
    {
        case AUTH_SOURCE_KEYPAD_USER:
            return UI_ZH_KEYPAD_UNLOCK_SUCCESS;

        case AUTH_SOURCE_CARD:
            return UI_ZH_CARD_UNLOCK_SUCCESS;

        case AUTH_SOURCE_FINGERPRINT:
            return UI_ZH_FINGERPRINT_UNLOCK_SUCCESS;

        case AUTH_SOURCE_BLUETOOTH:
            return UI_ZH_BLUETOOTH_UNLOCK_SUCCESS;

        default:
            return UI_ZH_UNLOCKED;
    }
}

static uint8_t AccessControl_GetUnlockTrack(AuthSource_t Source)
{
    switch (Source)
    {
        case AUTH_SOURCE_KEYPAD_USER:
            return ACCESS_CONTROL_TRACK_KEYPAD_UNLOCK;

        case AUTH_SOURCE_CARD:
            return ACCESS_CONTROL_TRACK_CARD_UNLOCK;

        case AUTH_SOURCE_FINGERPRINT:
            return ACCESS_CONTROL_TRACK_FINGERPRINT_UNLOCK;

        case AUTH_SOURCE_BLUETOOTH:
            /* No dedicated Bluetooth voice prompt exists in the current pack. */
            return ACCESS_CONTROL_TRACK_WELCOME;

        default:
            return ACCESS_CONTROL_TRACK_WELCOME;
    }
}

static void AccessControl_ClearLockoutIfActive(void)
{
    if (s_state == ACCESS_STATE_LOCKOUT)
    {
        (void)AccessControl_LogEvent(ACCESS_LOG_EVENT_LOCKOUT_END, 1U, ACCESS_CONTROL_MAX_FAILURES, ACCESS_CONTROL_LOCKOUT_MS);
        AccessControl_SetLockoutFlag(0U);
    }
}

static void AccessControl_LogWriteRecord(uint8_t *Buffer, AccessLogEvent_t Event, uint8_t Result, uint32_t Value, uint32_t Extra, uint32_t Sequence)
{
    AppRTC_DateTime_t DateTime;
    uint32_t Checksum;

    memset(Buffer, 0, ACCESS_CONTROL_LOG_RECORD_SIZE);

    AccessControl_WriteU32(Buffer, 0U, ACCESS_CONTROL_LOG_MAGIC);
    AccessControl_WriteU16(Buffer, 4U, ACCESS_CONTROL_LOG_VERSION);
    Buffer[6U] = (uint8_t)Event;
    Buffer[7U] = Result;

    if (AppRTC_IsReady() != 0U)
    {
        AppRTC_GetDateTime(&DateTime);
        AccessControl_WriteU16(Buffer, 8U, DateTime.Year);
        Buffer[10U] = DateTime.Month;
        Buffer[11U] = DateTime.Day;
        Buffer[12U] = DateTime.DayOfWeek;
        Buffer[13U] = DateTime.Hour;
        Buffer[14U] = DateTime.Minute;
        Buffer[15U] = DateTime.Second;
    }

    AccessControl_WriteU32(Buffer, 16U, Value);
    AccessControl_WriteU32(Buffer, 20U, Extra);
    AccessControl_WriteU32(Buffer, 24U, Sequence);
    Checksum = AccessControl_CalcChecksum(Buffer, 28U);
    AccessControl_WriteU32(Buffer, 28U, Checksum);
}

static uint8_t AccessControl_LogDecodeRecord(const uint8_t *Buffer, uint32_t *Sequence)
{
    uint32_t StoredChecksum;
    uint32_t CalcChecksum;

    if ((Buffer == 0) || (Sequence == 0))
    {
        return 0U;
    }

    if ((AccessControl_ReadU32(Buffer, 0U) != ACCESS_CONTROL_LOG_MAGIC) ||
        (AccessControl_ReadU16(Buffer, 4U) != ACCESS_CONTROL_LOG_VERSION))
    {
        return 0U;
    }

    StoredChecksum = AccessControl_ReadU32(Buffer, 28U);
    CalcChecksum = AccessControl_CalcChecksum(Buffer, 28U);
    if (StoredChecksum != CalcChecksum)
    {
        return 0U;
    }

    *Sequence = AccessControl_ReadU32(Buffer, 24U);
    return 1U;
}

static uint8_t AccessControl_LogSlotIsBlank(const uint8_t *Buffer)
{
    uint8_t i;

    if (Buffer == 0)
    {
        return 0U;
    }

    for (i = 0U; i < ACCESS_CONTROL_LOG_RECORD_SIZE; i++)
    {
        if (Buffer[i] != 0xFFU)
        {
            return 0U;
        }
    }

    return 1U;
}

static uint32_t AccessControl_LogSectorAddress(uint16_t SectorIndex)
{
    return ((uint32_t)SectorIndex * W25Q64_SECTOR_SIZE);
}

static uint16_t AccessControl_LogNextSector(uint16_t SectorIndex)
{
    SectorIndex++;
    if (SectorIndex >= (uint16_t)(ACCESS_CONTROL_LOG_SECTOR_FIRST + ACCESS_CONTROL_LOG_SECTOR_COUNT))
    {
        SectorIndex = ACCESS_CONTROL_LOG_SECTOR_FIRST;
    }

    return SectorIndex;
}

static void AccessControl_InitLogStorage(void)
{
    uint8_t Buffer[ACCESS_CONTROL_LOG_RECORD_SIZE];
    uint32_t Sequence;
    uint32_t LatestSequence = 0U;
    uint16_t LatestSector = ACCESS_CONTROL_LOG_SECTOR_FIRST;
    uint16_t LatestEntry = 0U;
    uint16_t Sector;
    uint16_t Entry;
    uint32_t Address;
    uint8_t Found = 0U;

    s_log_available = 0U;
    s_log_sequence = 0U;
    s_log_sector = ACCESS_CONTROL_LOG_SECTOR_FIRST;
    s_log_entry = 0U;
    s_log_sector_needs_erase = 1U;

    if (s_flash_available == 0U)
    {
        return;
    }

    s_log_available = 1U;

    for (Sector = ACCESS_CONTROL_LOG_SECTOR_FIRST;
         Sector < (uint16_t)(ACCESS_CONTROL_LOG_SECTOR_FIRST + ACCESS_CONTROL_LOG_SECTOR_COUNT);
         Sector++)
    {
        Address = AccessControl_LogSectorAddress(Sector);
        for (Entry = 0U; Entry < ACCESS_CONTROL_LOG_ENTRIES_PER_SECTOR; Entry++)
        {
            W25Q64_ReadBytes(Address + ((uint32_t)Entry * ACCESS_CONTROL_LOG_RECORD_SIZE), Buffer, ACCESS_CONTROL_LOG_RECORD_SIZE);
            if (AccessControl_LogDecodeRecord(Buffer, &Sequence) != 0U)
            {
                if ((Found == 0U) || (Sequence > LatestSequence))
                {
                    Found = 1U;
                    LatestSequence = Sequence;
                    LatestSector = Sector;
                    LatestEntry = Entry;
                }
            }
        }
    }

    if (Found == 0U)
    {
        return;
    }

    s_log_sequence = LatestSequence;
    s_log_sector = LatestSector;
    s_log_entry = (uint16_t)(LatestEntry + 1U);
    if (s_log_entry >= ACCESS_CONTROL_LOG_ENTRIES_PER_SECTOR)
    {
        s_log_entry = 0U;
        s_log_sector = AccessControl_LogNextSector(s_log_sector);
        s_log_sector_needs_erase = 1U;
    }
    else
    {
        s_log_sector_needs_erase = 0U;
    }
}

static uint8_t AccessControl_LogPrepareWritableSlot(void)
{
    uint8_t Buffer[ACCESS_CONTROL_LOG_RECORD_SIZE];
    uint16_t StartSector;
    uint16_t StartEntry;
    uint32_t Address;

    if (s_log_available == 0U)
    {
        return 0U;
    }

    StartSector = s_log_sector;
    StartEntry = s_log_entry;

    for (;;)
    {
        if (s_log_sector_needs_erase != 0U)
        {
            if (W25Q64_EraseSector(AccessControl_LogSectorAddress(s_log_sector)) != MI_OK)
            {
                s_log_available = 0U;
                return 0U;
            }
            s_log_sector_needs_erase = 0U;
        }

        Address = AccessControl_LogSectorAddress(s_log_sector) + ((uint32_t)s_log_entry * ACCESS_CONTROL_LOG_RECORD_SIZE);
        W25Q64_ReadBytes(Address, Buffer, ACCESS_CONTROL_LOG_RECORD_SIZE);
        if (AccessControl_LogSlotIsBlank(Buffer) != 0U)
        {
            return 1U;
        }

        s_log_entry++;
        if (s_log_entry >= ACCESS_CONTROL_LOG_ENTRIES_PER_SECTOR)
        {
            s_log_entry = 0U;
            s_log_sector = AccessControl_LogNextSector(s_log_sector);
            s_log_sector_needs_erase = 1U;
        }

        if ((s_log_sector == StartSector) && (s_log_entry == StartEntry))
        {
            return 0U;
        }
    }
}

static uint8_t AccessControl_LogEvent(AccessLogEvent_t Event, uint8_t Result, uint32_t Value, uint32_t Extra)
{
    uint8_t Buffer[ACCESS_CONTROL_LOG_RECORD_SIZE];
    uint32_t Address;
    uint16_t Sector;
    uint16_t Entry;
    uint32_t Sequence;

    if ((s_flash_available == 0U) || (s_log_available == 0U))
    {
        return 0U;
    }

    Sector = s_log_sector;
    Entry = s_log_entry;
    if ((Sector < ACCESS_CONTROL_LOG_SECTOR_FIRST) ||
        (Sector >= (uint16_t)(ACCESS_CONTROL_LOG_SECTOR_FIRST + ACCESS_CONTROL_LOG_SECTOR_COUNT)) ||
        (Entry >= ACCESS_CONTROL_LOG_ENTRIES_PER_SECTOR))
    {
        AccessControl_InitLogStorage();
        Sector = s_log_sector;
        Entry = s_log_entry;
    }

    if (AccessControl_LogPrepareWritableSlot() == 0U)
    {
        return 0U;
    }

    Sector = s_log_sector;
    Entry = s_log_entry;

    Sequence = s_log_sequence + 1U;
    if (Sequence == 0U)
    {
        Sequence = 1U;
    }

    AccessControl_LogWriteRecord(Buffer, Event, Result, Value, Extra, Sequence);
    Address = AccessControl_LogSectorAddress(Sector) + ((uint32_t)Entry * ACCESS_CONTROL_LOG_RECORD_SIZE);
    if (W25Q64_WriteBytes(Address, Buffer, ACCESS_CONTROL_LOG_RECORD_SIZE) != MI_OK)
    {
        return 0U;
    }

    s_log_sequence = Sequence;
    s_log_entry++;
    if (s_log_entry >= ACCESS_CONTROL_LOG_ENTRIES_PER_SECTOR)
    {
        s_log_entry = 0U;
        s_log_sector = AccessControl_LogNextSector(s_log_sector);
        s_log_sector_needs_erase = 1U;
    }

    return 1U;
}

static void AccessControl_SetDefaultConfig(void)
{
    uint8_t i;

    memset(&s_config, 0, sizeof(s_config));
    s_config.Magic = ACCESS_CONTROL_STORAGE_MAGIC;
    s_config.Version = ACCESS_CONTROL_STORAGE_VERSION;
    s_config.NextFingerprintId = 1U;
    s_config.UserPassword = ACCESS_CONTROL_DEFAULT_USER_PASSWORD;
    s_config.AdminPassword = ACCESS_CONTROL_DEFAULT_ADMIN_PASSWORD;
    s_config.CardCount = 0U;
    AccessControl_FingerprintBitmapClearAll();

    for (i = 0U; i < ACCESS_CONTROL_MAX_CARD_SLOTS; i++)
    {
        memset(s_config.CardUids[i], 0, sizeof(s_config.CardUids[i]));
    }
}

static uint8_t AccessControl_EncodeConfig(uint8_t *Buffer, uint32_t BufferSize, uint32_t Sequence)
{
    uint32_t Checksum;
    uint32_t Offset;
    uint8_t i;

    if ((Buffer == 0) || (BufferSize < ACCESS_CONTROL_STORAGE_IMAGE_SIZE))
    {
        return 0U;
    }

    memset(Buffer, 0, ACCESS_CONTROL_STORAGE_IMAGE_SIZE);

    AccessControl_WriteU32(Buffer, 0U, s_config.Magic);
    AccessControl_WriteU16(Buffer, 4U, s_config.Version);
    AccessControl_WriteU32(Buffer, 6U, Sequence);
    AccessControl_WriteU16(Buffer, 10U, s_config.NextFingerprintId);
    AccessControl_WriteU32(Buffer, 12U, s_config.UserPassword);
    AccessControl_WriteU32(Buffer, 16U, s_config.AdminPassword);
    Buffer[20U] = s_config.CardCount;
    Buffer[21U] = s_config.IsLockedOut;

    Offset = 24U;
    for (i = 0U; i < ACCESS_CONTROL_MAX_CARD_SLOTS; i++)
    {
        memcpy(&Buffer[Offset], s_config.CardUids[i], 5U);
        Offset += 5U;
    }

    memcpy(&Buffer[Offset], s_config.FingerprintBitmap, sizeof(s_config.FingerprintBitmap));
    Offset += (uint32_t)sizeof(s_config.FingerprintBitmap);

    Checksum = AccessControl_CalcChecksum(Buffer, Offset);
    AccessControl_WriteU32(Buffer, Offset, Checksum);
    return 1U;
}

static uint8_t AccessControl_DecodeConfig(const uint8_t *Buffer, uint32_t BufferSize, AccessConfig_t *OutConfig, uint32_t *Sequence)
{
    uint32_t StoredChecksum;
    uint32_t CalcChecksum;
    uint32_t Offset;
    uint8_t i;
    uint16_t Version;

    if ((Buffer == 0) || (OutConfig == 0) || (Sequence == 0) || (BufferSize < ACCESS_CONTROL_STORAGE_IMAGE_SIZE))
    {
        return 0U;
    }

    if ((AccessControl_ReadU32(Buffer, 0U) != ACCESS_CONTROL_STORAGE_MAGIC) ||
        ((Version = AccessControl_ReadU16(Buffer, 4U)) == 0U))
    {
        return 0U;
    }

    memset(OutConfig, 0, sizeof(*OutConfig));
    OutConfig->Magic = AccessControl_ReadU32(Buffer, 0U);
    OutConfig->Version = AccessControl_ReadU16(Buffer, 4U);
    *Sequence = AccessControl_ReadU32(Buffer, 6U);
    OutConfig->NextFingerprintId = AccessControl_ReadU16(Buffer, 10U);
    OutConfig->UserPassword = AccessControl_ReadU32(Buffer, 12U);
    OutConfig->AdminPassword = AccessControl_ReadU32(Buffer, 16U);
    OutConfig->CardCount = Buffer[20U];
    OutConfig->IsLockedOut = Buffer[21U];

    if (OutConfig->CardCount > ACCESS_CONTROL_MAX_CARD_SLOTS)
    {
        return 0U;
    }

    Offset = 24U;
    for (i = 0U; i < ACCESS_CONTROL_MAX_CARD_SLOTS; i++)
    {
        memcpy(OutConfig->CardUids[i], &Buffer[Offset], 5U);
        Offset += 5U;
    }

    if (Version == ACCESS_CONTROL_STORAGE_VERSION)
    {
        memcpy(OutConfig->FingerprintBitmap, &Buffer[Offset], sizeof(OutConfig->FingerprintBitmap));
        Offset += (uint32_t)sizeof(OutConfig->FingerprintBitmap);
        StoredChecksum = AccessControl_ReadU32(Buffer, Offset);
        CalcChecksum = AccessControl_CalcChecksum(Buffer, Offset);
        if (StoredChecksum != CalcChecksum)
        {
            return 0U;
        }
    }
    else if (Version == 0x0001U)
    {
        AccessControl_FingerprintBitmapClearAll();
        StoredChecksum = AccessControl_ReadU32(Buffer, 64U);
        CalcChecksum = AccessControl_CalcChecksum(Buffer, 64U);
        if (StoredChecksum != CalcChecksum)
        {
            return 0U;
        }
    }
    else
    {
        return 0U;
    }

    if ((OutConfig->NextFingerprintId == 0U) ||
        (OutConfig->UserPassword == 0U) ||
        (OutConfig->AdminPassword == 0U))
    {
        return 0U;
    }

    return 1U;
}

static void AccessControl_ClearInput(void)
{
    memset(s_input_buffer, 0, sizeof(s_input_buffer));
    s_input_length = 0U;
}

static void AccessControl_AppendInputChar(uint8_t Ch)
{
    if (s_input_length >= ACCESS_CONTROL_MAX_PASSWORD_LEN)
    {
        return;
    }

    s_input_buffer[s_input_length++] = Ch;
    s_input_buffer[s_input_length] = '\0';
}

static uint8_t AccessControl_ParseDecimal(const uint8_t *Buffer, uint8_t Length, uint32_t *Value)
{
    uint32_t Result = 0U;
    uint32_t Digit;
    uint8_t i;

    if ((Buffer == 0) || (Value == 0) || (Length == 0U) || (Length > ACCESS_CONTROL_MAX_PASSWORD_LEN))
    {
        return 0U;
    }

    for (i = 0U; i < Length; i++)
    {
        if ((Buffer[i] < '0') || (Buffer[i] > '9'))
        {
            return 0U;
        }

        Digit = (uint32_t)(Buffer[i] - '0');
        if (Result > ((0xFFFFFFFFU - Digit) / 10U))
        {
            return 0U;
        }

        Result = (Result * 10U) + Digit;
    }

    *Value = Result;
    return 1U;
}

static uint8_t AccessControl_CardEquals(const uint8_t *A, const uint8_t *B)
{
    return (uint8_t)(memcmp(A, B, 5U) == 0);
}

static int8_t AccessControl_FindCardIndex(const uint8_t *Uid)
{
    uint8_t i;

    if (Uid == 0)
    {
        return -1;
    }

    for (i = 0U; i < s_config.CardCount; i++)
    {
        if (AccessControl_CardEquals(Uid, s_config.CardUids[i]) != 0U)
        {
            return (int8_t)i;
        }
    }

    return -1;
}

static uint8_t AccessControl_SaveConfig(void)
{
    uint8_t Image[ACCESS_CONTROL_STORAGE_IMAGE_SIZE];
    uint32_t Sequence;
    uint8_t PrimarySector;
    uint8_t SecondarySector;
    uint32_t PrimaryAddress;
    uint32_t SecondaryAddress;

    if (s_flash_available == 0U)
    {
        return 0U;
    }

    if (s_storage_active_sector > 1U)
    {
        s_storage_active_sector = 0U;
    }

    Sequence = s_storage_sequence + 1U;
    if (AccessControl_EncodeConfig(Image, sizeof(Image), Sequence) == 0U)
    {
        return 0U;
    }

    PrimarySector = s_storage_active_sector;
    SecondarySector = (uint8_t)(PrimarySector ^ 1U);
    PrimaryAddress = (uint32_t)PrimarySector * W25Q64_SECTOR_SIZE;
    SecondaryAddress = (uint32_t)SecondarySector * W25Q64_SECTOR_SIZE;

    if (W25Q64_EraseSector(PrimaryAddress) != MI_OK)
    {
        return 0U;
    }

    if (W25Q64_WriteBytes(PrimaryAddress, Image, sizeof(Image)) != MI_OK)
    {
        return 0U;
    }

    s_storage_active_sector = PrimarySector;
    s_storage_sequence = Sequence;

    if (W25Q64_EraseSector(SecondaryAddress) != MI_OK)
    {
        return 1U;
    }

    if (W25Q64_WriteBytes(SecondaryAddress, Image, sizeof(Image)) != MI_OK)
    {
        return 1U;
    }

    return 1U;
}

static void AccessControl_LoadConfig(void)
{
    uint8_t ManufacturerID;
    uint8_t MemoryType;
    uint8_t CapacityID;
    uint8_t ImageA[ACCESS_CONTROL_STORAGE_IMAGE_SIZE];
    uint8_t ImageB[ACCESS_CONTROL_STORAGE_IMAGE_SIZE];
    AccessConfig_t ConfigA;
    AccessConfig_t ConfigB;
    uint32_t SeqA = 0U;
    uint32_t SeqB = 0U;
    uint8_t ValidA;
    uint8_t ValidB;

    s_flash_available = 0U;
    s_storage_active_sector = 0U;
    s_storage_sequence = 0U;
    AccessControl_SetDefaultConfig();

    if (W25Q64_ReadID(&ManufacturerID, &MemoryType, &CapacityID) != MI_OK)
    {
        return;
    }

    s_flash_available = 1U;
    W25Q64_ReadBytes(ACCESS_CONTROL_STORAGE_SECTOR_A, ImageA, sizeof(ImageA));
    W25Q64_ReadBytes(ACCESS_CONTROL_STORAGE_SECTOR_B, ImageB, sizeof(ImageB));

    ValidA = AccessControl_DecodeConfig(ImageA, sizeof(ImageA), &ConfigA, &SeqA);
    ValidB = AccessControl_DecodeConfig(ImageB, sizeof(ImageB), &ConfigB, &SeqB);

    if ((ValidA == 0U) && (ValidB == 0U))
    {
        s_storage_active_sector = 0U;
        AccessControl_SetDefaultConfig();
        AccessControl_SaveConfig();
    }
    else if ((ValidB != 0U) && ((ValidA == 0U) || (SeqB > SeqA)))
    {
        s_config = ConfigB;
        s_storage_sequence = SeqB;
        s_storage_active_sector = 1U;
    }
    else
    {
        s_config = ConfigA;
        s_storage_sequence = SeqA;
        s_storage_active_sector = 0U;
    }

    AccessControl_InitLogStorage();
}

static void AccessControl_PlayPrompt(uint8_t Track)
{
    if (s_fault_mask & ACCESS_FAULT_VOICE)
    {
        return;
    }

    AccessControl_WakeVoiceModule();
    delay_ms(30U);
    JQ8900_StopInsert();
    delay_ms(30U);
    AccessControl_SelectVoiceDisk();
    delay_ms(30U);
    JQ8900_PlayTrack(Track);
}

static void AccessControl_ResetActivity(void)
{
    s_last_activity = AccessControl_Now();
}

static void AccessControl_EnterState(AccessState_t NewState)
{
    s_state = NewState;
    s_last_refresh = 0U;
    if (s_state != ACCESS_STATE_SLEEP)
    {
        s_lowpower_window_active = 0U;
        s_lowpower_wake_deadline = 0U;
    }
    AccessControl_ResetActivity();
    Buzzer_Off();

    switch (s_state)
    {
        case ACCESS_STATE_INIT:
            AccessControl_ResetEnrollment();
            MenuUI_DisplayOn();
            MenuUI_ShowBootScreen();
            Servo_Lock();
            LED_AllOff();
            break;

        case ACCESS_STATE_IDLE:
            AccessControl_ResetEnrollment();
            MenuUI_DisplayOn();
            AccessControl_ClearInput();
            s_admin_action = ADMIN_ACTION_NONE;
            s_auth_source = AUTH_SOURCE_NONE;
            s_last_card_scan = 0U;
            s_last_fp_scan = 0U;
            AppRTC_CancelAlarm();
            AccessControl_WakeVoiceModule();
            MenuUI_ShowIdleScreen(UI_ZH_IDLE_HINT);
            MenuUI_UpdateIdleClock(s_fault_mask);
            Servo_Lock();
            LED_AllOff();
            break;

        case ACCESS_STATE_AUTH:
            MenuUI_DisplayOn();
            MenuUI_ShowAuthScreen();
            break;

        case ACCESS_STATE_UNLOCKED:
            AccessControl_ResetEnrollment();
            MenuUI_DisplayOn();
            AccessControl_ClearInput();
            AccessControl_WakeVoiceModule();
            MenuUI_ShowUnlockedScreen(AccessControl_GetUnlockSuccessText(s_auth_source));
            Servo_Unlock();
            LED_ShowUnlocked();
            s_state_deadline = AccessControl_Now() + ACCESS_CONTROL_UNLOCK_HOLD_MS;
            break;

        case ACCESS_STATE_ADMIN:
            AccessControl_ResetEnrollment();
            MenuUI_DisplayOn();
            AccessControl_ClearInput();
            AccessControl_WakeVoiceModule();
            s_admin_menu_page = 0U;
            MenuUI_ShowAdminMenu(s_admin_menu_page);
            break;

        case ACCESS_STATE_ADMIN_ENROLLING:
            MenuUI_DisplayOn();
            AccessControl_ClearInput();
            s_admin_action = ADMIN_ACTION_NONE;
            s_auth_source = AUTH_SOURCE_NONE;
            AccessControl_WakeVoiceModule();
            MenuUI_ShowEnrollScreen((uint8_t)s_enroll_stage, s_enroll_target_page);
            break;

        case ACCESS_STATE_LOCKOUT:
            AccessControl_ResetEnrollment();
            MenuUI_DisplayOn();
            AccessControl_ClearInput();
            s_admin_action = ADMIN_ACTION_NONE;
            s_auth_source = AUTH_SOURCE_NONE;
            AccessControl_WakeVoiceModule();
            MenuUI_ShowLockoutScreen(0U);
            Servo_Lock();
            LED_ShowLocked();
            AccessControl_SetLockoutFlag(1U);
            (void)AccessControl_LogEvent(ACCESS_LOG_EVENT_LOCKOUT_START, 1U, ACCESS_CONTROL_MAX_FAILURES, ACCESS_CONTROL_LOCKOUT_MS);
            AccessControl_PlayPrompt(ACCESS_CONTROL_TRACK_LOCKED);
            s_state_deadline = AccessControl_Now() + ACCESS_CONTROL_LOCKOUT_MS;
            s_last_card_scan = 0U;
            s_last_fp_scan = 0U;
            break;

        case ACCESS_STATE_SLEEP:
            AccessControl_ResetEnrollment();
            AccessControl_ClearInput();
            s_admin_action = ADMIN_ACTION_NONE;
            s_auth_source = AUTH_SOURCE_NONE;
            s_last_card_scan = 0U;
            s_last_fp_scan = 0U;
            s_lowpower_window_active = 0U;
            s_lowpower_wake_deadline = 0U;
            MenuUI_DisplayOff();
            JQ8900_Sleep();
            s_voice_awake = 0U;
            Servo_Lock();
            LED_AllOff();
            if ((AppRTC_IsReady() != 0U) &&
                (AppRTC_SetAlarm(ACCESS_CONTROL_LOWPOWER_WAKE_SECONDS) != 0U))
            {
                AccessControl_EnterLowPowerStopMode();
                (void)AppRTC_ConsumeAlarmFlag();
                s_lowpower_window_active = 1U;
                s_lowpower_wake_deadline = AccessControl_Now() + ACCESS_CONTROL_LOWPOWER_ACTIVE_WINDOW_MS;
            }
            break;

        case ACCESS_STATE_FAULT:
        default:
            AccessControl_ResetEnrollment();
            AccessControl_ClearInput();
            s_admin_action = ADMIN_ACTION_NONE;
            s_auth_source = AUTH_SOURCE_NONE;
            MenuUI_DisplayOn();
            MenuUI_ShowFaultScreen();
            LED_ShowFail();
            (void)AccessControl_LogEvent(ACCESS_LOG_EVENT_MODULE_FAULT, 0U, 0U, 0U);
            break;
    }
}

static uint8_t AccessControl_RecordFailure(void)
{
    if (s_failure_count < 0xFFU)
    {
        s_failure_count++;
    }

    (void)AccessControl_LogEvent(ACCESS_LOG_EVENT_FAILURE, (uint8_t)s_auth_source, s_failure_count, 0U);
    Buzzer_Beep(2200U, 80U);
    LED_ShowFail();
    AccessControl_PlayPrompt(ACCESS_CONTROL_TRACK_PASSWORD_FAIL);
    AccessControl_ResetActivity();
    return 0U;
}

static void AccessControl_RecordPasswordFailure(void)
{
    if (s_password_failure_count < 0xFFU)
    {
        s_password_failure_count++;
    }

    (void)AccessControl_LogEvent(ACCESS_LOG_EVENT_FAILURE, (uint8_t)s_auth_source, s_password_failure_count, 0U);
    Buzzer_Beep(2200U, 80U);

    if (s_password_failure_count >= ACCESS_CONTROL_MAX_FAILURES)
    {
        s_password_failure_count = 0U;
        AccessControl_EnterState(ACCESS_STATE_LOCKOUT);
        return;
    }

    LED_RedBlink(5U, 80U, 80U);
    AccessControl_PlayPrompt(ACCESS_CONTROL_TRACK_PASSWORD_FAIL);
    AccessControl_ResetActivity();
}

static void AccessControl_RecordSuccess(void)
{
    s_failure_count = 0U;
    s_password_failure_count = 0U;
    AccessControl_ResetActivity();
}

static uint8_t AccessControl_UpdatePassword(uint32_t NewValue, AdminAction_t Action)
{
    uint8_t OK = 0U;
    AccessLogEvent_t Event;

    if (Action == ADMIN_ACTION_CHANGE_USER_PASSWORD)
    {
        Event = ACCESS_LOG_EVENT_ADMIN_CHANGE_USER_PASSWORD;
    }
    else if (Action == ADMIN_ACTION_CHANGE_ADMIN_PASSWORD)
    {
        Event = ACCESS_LOG_EVENT_ADMIN_CHANGE_ADMIN_PASSWORD;
    }
    else
    {
        return 0U;
    }

    if (NewValue == 0U)
    {
        (void)AccessControl_LogEvent(Event, 0U, 0U, 0U);
        return 0U;
    }

    if (Action == ADMIN_ACTION_CHANGE_USER_PASSWORD)
    {
        s_config.UserPassword = NewValue;
    }
    else
    {
        s_config.AdminPassword = NewValue;
    }

    if (AccessControl_SaveConfig() != 0U)
    {
        OK = 1U;
    }
    (void)AccessControl_LogEvent(Event, OK, 0U, 0U);
    return OK;
}

static uint8_t AccessControl_AddCard(const uint8_t *Uid)
{
    uint8_t Index;
    uint32_t CardHash;
    uint8_t OK = 0U;

    CardHash = (Uid != 0) ? AccessControl_CalcChecksum(Uid, 5U) : 0U;

    if ((Uid != 0) &&
        (s_config.CardCount < ACCESS_CONTROL_MAX_CARD_SLOTS) &&
        (AccessControl_FindCardIndex(Uid) < 0))
    {
        Index = s_config.CardCount;
        memcpy(s_config.CardUids[Index], Uid, 5U);
        s_config.CardCount++;
        if (AccessControl_SaveConfig() != 0U)
        {
            OK = 1U;
        }
    }

    (void)AccessControl_LogEvent(ACCESS_LOG_EVENT_ADMIN_ADD_CARD, OK, CardHash, (uint32_t)s_config.CardCount);
    return OK;
}

static uint8_t AccessControl_DeleteCard(const uint8_t *Uid)
{
    int8_t Index;
    uint8_t i;
    uint32_t CardHash;
    uint8_t OK = 0U;

    Index = AccessControl_FindCardIndex(Uid);
    CardHash = (Uid != 0) ? AccessControl_CalcChecksum(Uid, 5U) : 0U;

    if (Index >= 0)
    {
        for (i = (uint8_t)Index; (i + 1U < s_config.CardCount) && (i + 1U < ACCESS_CONTROL_MAX_CARD_SLOTS); i++)
        {
            memcpy(s_config.CardUids[i], s_config.CardUids[i + 1U], 5U);
        }

        if (s_config.CardCount > 0U)
        {
            memset(s_config.CardUids[s_config.CardCount - 1U], 0, 5U);
            s_config.CardCount--;
        }

        if (AccessControl_SaveConfig() != 0U)
        {
            OK = 1U;
        }
    }

    (void)AccessControl_LogEvent(ACCESS_LOG_EVENT_ADMIN_DELETE_CARD, OK, CardHash, (uint32_t)s_config.CardCount);
    return OK;
}

static uint16_t AccessControl_GetFingerprintCapacity(void)
{
    if (s_fp_capacity != 0U)
    {
        return s_fp_capacity;
    }

    if (s_fp_syspara.Capacity != 0U)
    {
        s_fp_capacity = s_fp_syspara.Capacity;
    }
    else
    {
        s_fp_capacity = 200U;
    }

    if (s_fp_capacity > ACCESS_CONTROL_FINGERPRINT_MAX_ID)
    {
        s_fp_capacity = ACCESS_CONTROL_FINGERPRINT_MAX_ID;
    }

    return s_fp_capacity;
}

static uint8_t AccessControl_DeleteFingerprint(uint16_t PageId)
{
    uint8_t OK = 0U;

    if ((PageId == 0U) || (PageId > AccessControl_GetFingerprintCapacity()))
    {
        (void)AccessControl_LogEvent(ACCESS_LOG_EVENT_ADMIN_DELETE_FINGERPRINT, 0U, PageId, 0U);
        return 0U;
    }

    if (AS608_DeleteModel(PageId, 1U) != AS608_OK)
    {
        (void)AccessControl_LogEvent(ACCESS_LOG_EVENT_ADMIN_DELETE_FINGERPRINT, 0U, PageId, 0U);
        return 0U;
    }

    AccessControl_FingerprintBitmapClear(PageId);
    (void)AccessControl_SaveConfig();
    OK = 1U;
    (void)AccessControl_LogEvent(ACCESS_LOG_EVENT_ADMIN_DELETE_FINGERPRINT, OK, PageId, 0U);
    return OK;
}

static void AccessControl_WakeVoiceModule(void)
{
    if (s_voice_awake == 0U)
    {
        s_voice_awake = 1U;
        JQ8900_SetVolume(ACCESS_CONTROL_VOICE_VOLUME);
        delay_ms(50U);
        JQ8900_SetChannel(JQ8900_CHANNEL_MP3);
        delay_ms(50U);
    }
}

static void AccessControl_SelectVoiceDisk(void)
{
    uint8_t Disk = ACCESS_CONTROL_VOICE_DISK;

    JQ8900_SendCommand(0x0BU, &Disk, 1U);
    delay_ms(100U);
}

static void AccessControl_ResetEnrollment(void)
{
    s_enroll_stage = ENROLL_STAGE_IDLE;
    s_enroll_target_page = 0U;
    s_enroll_deadline = 0U;
    s_enroll_release_deadline = 0U;
}

static void AccessControl_SetLockoutFlag(uint8_t Enable)
{
    if (Enable > 0U)
    {
        Enable = 1U;
    }

    if (s_config.IsLockedOut == Enable)
    {
        return;
    }

    s_config.IsLockedOut = Enable;
    AccessControl_SaveConfig();
}

static void AccessControl_ForceSpiCsHigh(void)
{
    GPIO_SetBits(GPIOA, GPIO_Pin_4 | GPIO_Pin_15);
}

static void AccessControl_ConfigWakeupPin(void)
{
#if ACCESS_CONTROL_USE_WKUP_PIN
    GPIO_InitTypeDef GPIO_InitStructure;

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE);

    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_0;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IPD;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOA, &GPIO_InitStructure);
#else
    (void)0;
#endif
}

static void AccessControl_ResumeFromStopMode(void)
{
    SystemInit();
    SystemCoreClockUpdate();
    delay_init();
    Timer_Init();
    AccessControl_ForceSpiCsHigh();
    Bluetooth_Init();
    AS608_Init();
    JQ8900_Init();
    IWDG_Feed();
}

static void AccessControl_EnterLowPowerStopMode(void)
{
    Timer_Stop();
    AccessControl_ForceSpiCsHigh();
    PWR_ClearFlag(PWR_FLAG_WU);
#if ACCESS_CONTROL_USE_WKUP_PIN
    PWR_WakeUpPinCmd(ENABLE);
#endif
    SCB->SCR |= SCB_SCR_SLEEPDEEP_Msk;
    PWR_EnterSTOPMode(PWR_Regulator_LowPower, PWR_STOPEntry_WFI);
    SCB->SCR &= (uint32_t)(~SCB_SCR_SLEEPDEEP_Msk);
    AccessControl_ResumeFromStopMode();
}

static void AccessControl_AbortEnrollmentToAdmin(void)
{
    AccessControl_ResetEnrollment();
    AccessControl_EnterState(ACCESS_STATE_ADMIN);
}

static uint8_t AccessControl_StartEnrollment(void)
{
    uint16_t NextPage;

    if (s_fp_available == 0U)
    {
        LED_ShowFail();
        MenuUI_ShowLines(UI_ZH_ADMIN_ENROLL, UI_ZH_ENROLL_FAIL, 0, 0);
        delay_ms(1200U);
        AccessControl_EnterState(ACCESS_STATE_ADMIN);
        return 0U;
    }

    if (s_config.NextFingerprintId == 0U)
    {
        s_config.NextFingerprintId = 1U;
    }

    NextPage = AccessControl_FindNextFingerprintPage(s_config.NextFingerprintId);
    if (NextPage == 0U)
    {
        LED_ShowFail();
        MenuUI_ShowLines(UI_ZH_ADMIN_ENROLL, UI_ZH_ENROLL_FAIL, 0, 0);
        delay_ms(1200U);
        AccessControl_EnterState(ACCESS_STATE_ADMIN);
        return 0U;
    }

    s_enroll_target_page = NextPage;
    s_enroll_stage = ENROLL_STAGE_WAIT_FIRST_PRESS;
    s_enroll_deadline = AccessControl_Now() + ACCESS_CONTROL_ENROLL_TIMEOUT_MS;
    s_enroll_release_deadline = 0U;
    AccessControl_EnterState(ACCESS_STATE_ADMIN_ENROLLING);
    return 1U;
}

static void AccessControl_HandleEnrollment(void)
{
    uint8_t Result;
    uint16_t FoundPage = 0U;
    uint16_t Score = 0U;
    uint32_t Now;

    if (s_state != ACCESS_STATE_ADMIN_ENROLLING)
    {
        return;
    }

    Now = AccessControl_Now();
    if ((int32_t)(Now - s_enroll_deadline) >= 0)
    {
        LED_ShowFail();
        MenuUI_ShowLines(UI_ZH_ADMIN_ENROLL, UI_ZH_ENROLL_FAIL, 0, 0);
        delay_ms(1200U);
        AccessControl_AbortEnrollmentToAdmin();
        return;
    }

    MenuUI_ShowEnrollScreen((uint8_t)s_enroll_stage, s_enroll_target_page);

    switch (s_enroll_stage)
    {
        case ENROLL_STAGE_WAIT_FIRST_PRESS:
            Result = AS608_GetImage();
            if (Result == AS608_OK)
            {
                s_enroll_stage = ENROLL_STAGE_CAPTURE_FIRST;
            }
            break;

        case ENROLL_STAGE_CAPTURE_FIRST:
            Result = AS608_Image2Tz(AS608_BUFFER_1);
            if (Result == AS608_OK)
            {
                s_enroll_stage = ENROLL_STAGE_WAIT_FIRST_RELEASE;
                s_enroll_release_deadline = 0U;
            }
            else
            {
                s_enroll_stage = ENROLL_STAGE_WAIT_FIRST_PRESS;
            }
            break;

        case ENROLL_STAGE_WAIT_FIRST_RELEASE:
            Result = AS608_GetImage();
            if (Result == AS608_OK)
            {
                s_enroll_release_deadline = 0U;
            }
            else if (Result == ACCESS_CONTROL_AS608_NO_FINGER)
            {
                if (s_enroll_release_deadline == 0U)
                {
                    s_enroll_release_deadline = Now + ACCESS_CONTROL_ENROLL_RELEASE_DEBOUNCE_MS;
                }
                else if ((int32_t)(Now - s_enroll_release_deadline) >= 0)
                {
                    s_enroll_stage = ENROLL_STAGE_WAIT_SECOND_PRESS;
                    s_enroll_release_deadline = 0U;
                }
            }
            else
            {
                s_enroll_release_deadline = 0U;
            }
            break;

        case ENROLL_STAGE_WAIT_SECOND_PRESS:
            Result = AS608_GetImage();
            if (Result == AS608_OK)
            {
                s_enroll_stage = ENROLL_STAGE_CAPTURE_SECOND;
            }
            break;

        case ENROLL_STAGE_CAPTURE_SECOND:
            Result = AS608_Image2Tz(AS608_BUFFER_2);
            if (Result == AS608_OK)
            {
                s_enroll_stage = ENROLL_STAGE_REG_MODEL;
            }
            else
            {
                s_enroll_stage = ENROLL_STAGE_WAIT_SECOND_PRESS;
            }
            break;

        case ENROLL_STAGE_REG_MODEL:
            Result = AS608_RegModel();
            if (Result == AS608_OK)
            {
                s_enroll_stage = ENROLL_STAGE_DUP_CHECK;
            }
            else
            {
                LED_ShowFail();
                MenuUI_ShowLines(UI_ZH_ADMIN_ENROLL, UI_ZH_ENROLL_FAIL, 0, 0);
                delay_ms(1200U);
                AccessControl_AbortEnrollmentToAdmin();
            }
            break;

        case ENROLL_STAGE_DUP_CHECK:
            Result = AS608_Search(AS608_BUFFER_1, 0U, 0x03FFU, &FoundPage, &Score);
            if ((Result == AS608_OK) && (FoundPage == s_enroll_target_page))
            {
                (void)AccessControl_LogEvent(ACCESS_LOG_EVENT_ADMIN_ADD_FINGERPRINT, 0U, s_enroll_target_page, 0U);
                LED_ShowFail();
                MenuUI_ShowLines(UI_ZH_ADMIN_ENROLL, UI_ZH_ENROLL_FAIL, 0, 0);
                delay_ms(1200U);
                AccessControl_AbortEnrollmentToAdmin();
            }
            else
            {
                s_enroll_stage = ENROLL_STAGE_STORE_MODEL;
            }
            break;

        case ENROLL_STAGE_STORE_MODEL:
            Result = AS608_StoreModel(AS608_BUFFER_1, s_enroll_target_page);
            if (Result == AS608_OK)
            {
                AccessControl_FingerprintBitmapSet(s_enroll_target_page);
                s_config.NextFingerprintId = AccessControl_FindNextFingerprintPage((uint16_t)(s_enroll_target_page + 1U));
                if (s_config.NextFingerprintId == 0U)
                {
                    s_config.NextFingerprintId = 1U;
                }
                AccessControl_SaveConfig();
                (void)AccessControl_LogEvent(ACCESS_LOG_EVENT_ADMIN_ADD_FINGERPRINT, 1U, s_enroll_target_page, 0U);
                AccessControl_RecordSuccess();
                AccessControl_PlayPrompt(ACCESS_CONTROL_TRACK_ENROLL_FP_SUCCESS);
                LED_ShowSuccess();
                MenuUI_ShowLines(UI_ZH_ADMIN_ENROLL, UI_ZH_ENROLL_SUCCESS, 0, 0);
                delay_ms(1200U);
                AccessControl_AbortEnrollmentToAdmin();
            }
            else
            {
                (void)AccessControl_LogEvent(ACCESS_LOG_EVENT_ADMIN_ADD_FINGERPRINT, 0U, s_enroll_target_page, 0U);
                AccessControl_PlayPrompt(ACCESS_CONTROL_TRACK_ENROLL_FP_FAIL);
                LED_ShowFail();
                MenuUI_ShowLines(UI_ZH_ADMIN_ENROLL, UI_ZH_ENROLL_FAIL, 0, 0);
                delay_ms(1200U);
                AccessControl_AbortEnrollmentToAdmin();
            }
            break;

        default:
            AccessControl_AbortEnrollmentToAdmin();
            break;
    }
}

static void AccessControl_HandlePasswordConfirm(void)
{
    uint32_t Value = 0U;
    uint32_t Expect = 0U;

    if (AccessControl_ParseDecimal(s_input_buffer, s_input_length, &Value) == 0U)
    {
        AccessControl_RecordPasswordFailure();
        AccessControl_ClearInput();
        if (s_state != ACCESS_STATE_LOCKOUT)
        {
            AccessControl_EnterState(ACCESS_STATE_IDLE);
        }
        return;
    }

    if (s_auth_source == AUTH_SOURCE_KEYPAD_ADMIN)
    {
        Expect = s_config.AdminPassword;
    }
    else
    {
        Expect = s_config.UserPassword;
    }

    AccessControl_ClearInput();

    if (Value == Expect)
    {
        AccessControl_RecordSuccess();

        if (s_auth_source == AUTH_SOURCE_KEYPAD_ADMIN)
        {
            AccessControl_PlayPrompt(ACCESS_CONTROL_TRACK_ADMIN_LOGIN);
            AccessControl_EnterState(ACCESS_STATE_ADMIN);
        }
        else
        {
            (void)AccessControl_LogEvent(ACCESS_LOG_EVENT_UNLOCK, ACCESS_LOG_SOURCE_KEYPAD_USER, 0U, 0U);
            AccessControl_ClearLockoutIfActive();
            AccessControl_PlayPrompt(ACCESS_CONTROL_TRACK_KEYPAD_UNLOCK);
            AccessControl_EnterState(ACCESS_STATE_UNLOCKED);
        }
    }
    else
    {
        AccessControl_RecordPasswordFailure();
        if (s_state != ACCESS_STATE_LOCKOUT)
        {
            AccessControl_EnterState(ACCESS_STATE_IDLE);
        }
    }
}

static void AccessControl_ExitAdminToIdle(void)
{
    s_admin_action = ADMIN_ACTION_NONE;
    AccessControl_ClearInput();
    AccessControl_ResetEnrollment();
    AccessControl_EnterState(ACCESS_STATE_IDLE);
}

static void AccessControl_ReturnToAdminMenu(void)
{
    s_admin_action = ADMIN_ACTION_NONE;
    AccessControl_ClearInput();
    AccessControl_ResetEnrollment();
    s_admin_menu_page = 0U;
    MenuUI_ShowAdminMenu(s_admin_menu_page);
    AccessControl_ResetActivity();
}

static const char *AccessControl_GetUnlockSourceName(uint8_t Source)
{
    switch (Source)
    {
        case ACCESS_LOG_SOURCE_KEYPAD_USER:
            return "按键";

        case ACCESS_LOG_SOURCE_CARD:
            return "刷卡";

        case ACCESS_LOG_SOURCE_FINGERPRINT:
            return "指纹";

        case ACCESS_LOG_SOURCE_BLUETOOTH:
            return "蓝牙";

        default:
            return UI_ZH_UNKNOWN;
    }
}

static uint8_t AccessControl_GetCardList(uint8_t OutCards[ACCESS_CONTROL_MAX_CARD_SLOTS][5], uint8_t MaxCount, uint8_t *OutActualCount)
{
    uint8_t Count;
    uint8_t i;

    if ((OutCards == 0) || (OutActualCount == 0) || (MaxCount == 0U))
    {
        return 0U;
    }

    Count = 0U;
    for (i = 0U; (i < s_config.CardCount) && (Count < MaxCount); i++)
    {
        memcpy(OutCards[Count], s_config.CardUids[i], 5U);
        Count++;
    }

    *OutActualCount = Count;
    return 1U;
}

uint8_t AccessCore_QueryRecentUnlockLogs(AccessCore_LogEntry_t *OutEntries,
                                         uint8_t MaxCount,
                                         uint8_t *OutActualCount)
{
    uint8_t Buffer[ACCESS_CONTROL_LOG_RECORD_SIZE];
    uint16_t Sector, Entry;
    uint16_t CheckedSlots = 0U;
    uint16_t TotalSlots = ACCESS_CONTROL_LOG_SECTOR_COUNT * ACCESS_CONTROL_LOG_ENTRIES_PER_SECTOR;
    uint8_t FoundCount = 0U;
    uint32_t Address;
    uint32_t Seq;

    if ((OutEntries == 0) || (OutActualCount == 0) || (MaxCount == 0U))
    {
        return 0U;
    }

    *OutActualCount = 0U;
    if ((s_flash_available == 0U) || (s_log_available == 0U))
    {
        return 0U;
    }

    Sector = s_log_sector;
    Entry = s_log_entry;

    while ((CheckedSlots < TotalSlots) && (FoundCount < MaxCount))
    {
        if (Entry > 0U)
        {
            Entry--;
        }
        else
        {
            if (Sector == ACCESS_CONTROL_LOG_SECTOR_FIRST)
            {
                Sector = (uint16_t)(ACCESS_CONTROL_LOG_SECTOR_FIRST + ACCESS_CONTROL_LOG_SECTOR_COUNT - 1U);
            }
            else
            {
                Sector--;
            }
            Entry = (uint16_t)(ACCESS_CONTROL_LOG_ENTRIES_PER_SECTOR - 1U);
        }

        CheckedSlots++;

        Address = AccessControl_LogSectorAddress(Sector) + ((uint32_t)Entry * ACCESS_CONTROL_LOG_RECORD_SIZE);
        W25Q64_ReadBytes(Address, Buffer, ACCESS_CONTROL_LOG_RECORD_SIZE);

        if (AccessControl_LogDecodeRecord(Buffer, &Seq) != 0U)
        {
            if (Buffer[6U] == ACCESS_LOG_EVENT_UNLOCK)
            {
                OutEntries[FoundCount].Sequence = Seq;
                OutEntries[FoundCount].Event = Buffer[6U];
                OutEntries[FoundCount].Result = Buffer[7U];
                OutEntries[FoundCount].Year = AccessControl_ReadU16(Buffer, 8U);
                OutEntries[FoundCount].Month = Buffer[10U];
                OutEntries[FoundCount].Day = Buffer[11U];
                OutEntries[FoundCount].Hour = Buffer[13U];
                OutEntries[FoundCount].Minute = Buffer[14U];
                OutEntries[FoundCount].Second = Buffer[15U];
                OutEntries[FoundCount].Value = AccessControl_ReadU32(Buffer, 16U);
                OutEntries[FoundCount].Extra = AccessControl_ReadU32(Buffer, 20U);
                FoundCount++;
            }
        }
    }

    *OutActualCount = FoundCount;
    return 1U;
}

static uint8_t AccessControl_WaitForAnyKey(uint8_t *OutKey)
{
    if (OutKey == 0)
    {
        return 0U;
    }

    Keypad_KeyFlag = 0U;
    while (Keypad_KeyFlag == 0U)
    {
        IWDG_Feed();
    }

    *OutKey = Keypad_KeyValue;
    Keypad_KeyFlag = 0U;
    return 1U;
}

static void AccessControl_HandleKey(uint8_t Key)
{
    uint32_t Now;

    if (Key == 0U)
    {
        return;
    }

    Now = AccessControl_Now();
    s_last_activity = Now;

    if (s_state == ACCESS_STATE_SLEEP)
    {
        AccessControl_EnterState(ACCESS_STATE_IDLE);
        return;
    }

    if (s_state == ACCESS_STATE_IDLE)
    {
        if ((Key >= '0') && (Key <= '9'))
        {
            s_auth_source = AUTH_SOURCE_KEYPAD_USER;
            AccessControl_ClearInput();
            AccessControl_AppendInputChar(Key);
            AccessControl_EnterState(ACCESS_STATE_AUTH);
            MenuUI_UpdateInputDisplay(s_input_buffer);
            return;
        }

        if (Key == 'A')
        {
            s_auth_source = AUTH_SOURCE_KEYPAD_ADMIN;
            AccessControl_ClearInput();
            AccessControl_EnterState(ACCESS_STATE_AUTH);
            return;
        }
    }

    if (s_state == ACCESS_STATE_AUTH)
    {
        if ((Key >= '0') && (Key <= '9'))
        {
            AccessControl_AppendInputChar(Key);
            MenuUI_UpdateInputDisplay(s_input_buffer);
            return;
        }

        if (Key == '*')
        {
            AccessControl_ClearInput();
            MenuUI_UpdateInputDisplay(s_input_buffer);
            return;
        }

        if (Key == '#')
        {
            AccessControl_HandlePasswordConfirm();
            return;
        }

        if (Key == 'D')
        {
            AccessControl_ClearInput();
            AccessControl_EnterState(ACCESS_STATE_IDLE);
            return;
        }
    }

    if (s_state == ACCESS_STATE_ADMIN_ENROLLING)
    {
        if (Key == '*')
        {
            AccessControl_AbortEnrollmentToAdmin();
        }
        return;
    }

    if (s_state == ACCESS_STATE_ADMIN)
    {
        if (s_admin_action == ADMIN_ACTION_NONE)
        {
            switch (Key)
            {
                case '1':
                    if (AccessControl_StartEnrollment() != 0U)
                    {
                        AccessControl_RecordSuccess();
                        LED_ShowSuccess();
                    }
                    return;

                case '2':
                    s_admin_action = ADMIN_ACTION_DELETE_FINGERPRINT;
                    AccessControl_ClearInput();
                    MenuUI_ShowAdminPrompt((uint8_t)s_admin_action, s_input_buffer);
                    return;

                case '3':
                    s_admin_action = ADMIN_ACTION_ADD_CARD;
                    s_last_card_scan = 0U;
                    AccessControl_ResetActivity();
                    MenuUI_ShowAdminPrompt((uint8_t)s_admin_action, s_input_buffer);
                    return;

                case '4':
                    s_admin_action = ADMIN_ACTION_DELETE_CARD;
                    s_last_card_scan = 0U;
                    AccessControl_ResetActivity();
                    MenuUI_ShowAdminPrompt((uint8_t)s_admin_action, s_input_buffer);
                    return;

                case '5':
                    s_admin_action = ADMIN_ACTION_CHANGE_USER_PASSWORD;
                    AccessControl_ClearInput();
                    MenuUI_ShowAdminPrompt((uint8_t)s_admin_action, s_input_buffer);
                    return;

                case '6':
                    s_admin_action = ADMIN_ACTION_CHANGE_ADMIN_PASSWORD;
                    AccessControl_ClearInput();
                    MenuUI_ShowAdminPrompt((uint8_t)s_admin_action, s_input_buffer);
                    return;

                case '7':
                    {
                        char Line1[32];
                        char Line2[32];
                        char Line3[32];
                        char Line4[32];
                        uint8_t Key = 0U;
                        uint16_t FingerIds[3];
                        uint8_t FingerCount = AccessControl_FingerprintBitmapCount();
                        uint16_t FingerCapacity = AccessControl_GetFingerprintCapacity();
                        uint16_t FingerPage;
                        uint8_t FingerGroupCount = 0U;
                        uint8_t CardGroupCount = 0U;
                        uint8_t i;
                        uint8_t CardCount = 0U;
                        uint8_t Cards[ACCESS_CONTROL_MAX_CARD_SLOTS][5];
                        AccessCore_LogEntry_t Logs[ACCESS_CONTROL_UNLOCK_HISTORY_MAX];
                        uint8_t LogCount = 0U;

                        if (FingerCapacity > ACCESS_CONTROL_FINGERPRINT_MAX_ID)
                        {
                            FingerCapacity = ACCESS_CONTROL_FINGERPRINT_MAX_ID;
                        }

                        (void)sprintf(Line1, "%s", UI_ZH_QUERY_INFO);
                        (void)sprintf(Line2, "%s:%lu", UI_ZH_USER_PWD, (unsigned long)s_config.UserPassword);
                        MenuUI_ShowLines(Line1, Line2, 0, UI_ZH_PRESS_KEY_CONTINUE);
                        if (AccessControl_WaitForAnyKey(&Key) == 0U)
                        {
                            return;
                        }
                        if (Key == '*')
                        {
                            MenuUI_ShowAdminMenu(s_admin_menu_page);
                            return;
                        }

                        if (FingerCount == 0U)
                        {
                            (void)sprintf(Line1, "%s", UI_ZH_FP_LIBRARY);
                            MenuUI_ShowLines(Line1, UI_ZH_NO_DATA, 0, UI_ZH_PRESS_KEY_CONTINUE);
                            if (AccessControl_WaitForAnyKey(&Key) == 0U)
                            {
                                return;
                            }
                            if (Key == '*')
                            {
                                MenuUI_ShowAdminMenu(s_admin_menu_page);
                                return;
                            }
                        }
                        else
                        {
                            FingerGroupCount = 0U;
                            for (FingerPage = 1U; FingerPage <= FingerCapacity; FingerPage++)
                            {
                                if (AccessControl_FingerprintBitmapTest(FingerPage) == 0U)
                                {
                                    continue;
                                }

                                FingerIds[FingerGroupCount++] = FingerPage;
                                if (FingerGroupCount >= 3U)
                                {
                                    (void)sprintf(Line1, "%s(%u)", UI_ZH_FP_LIBRARY, (unsigned int)FingerCount);
                                    (void)sprintf(Line2, "ID:%04u", (unsigned int)FingerIds[0]);
                                    (void)sprintf(Line3, "ID:%04u", (unsigned int)FingerIds[1]);
                                    (void)sprintf(Line4, "ID:%04u", (unsigned int)FingerIds[2]);
                                    MenuUI_ShowLines(Line1, Line2, Line3, Line4);
                                    if (AccessControl_WaitForAnyKey(&Key) == 0U)
                                    {
                                        return;
                                    }
                                    if (Key == '*')
                                    {
                                        MenuUI_ShowAdminMenu(s_admin_menu_page);
                                        return;
                                    }
                                    FingerGroupCount = 0U;
                                }
                            }

                            if (FingerGroupCount > 0U)
                            {
                                (void)sprintf(Line1, "%s(%u)", UI_ZH_FP_LIBRARY, (unsigned int)FingerCount);
                                (void)sprintf(Line2, "%s", (FingerGroupCount > 0U) ? "ID:" : "");
                                if (FingerGroupCount > 0U)
                                {
                                    (void)sprintf(Line2, "ID:%04u", (unsigned int)FingerIds[0]);
                                }
                                if (FingerGroupCount > 1U)
                                {
                                    (void)sprintf(Line3, "ID:%04u", (unsigned int)FingerIds[1]);
                                }
                                else
                                {
                                    Line3[0] = '\0';
                                }
                                Line4[0] = '\0';
                                MenuUI_ShowLines(Line1, Line2, (FingerGroupCount > 1U) ? Line3 : 0, UI_ZH_PRESS_KEY_CONTINUE);
                                if (AccessControl_WaitForAnyKey(&Key) == 0U)
                                {
                                    return;
                                }
                                if (Key == '*')
                                {
                                    MenuUI_ShowAdminMenu(s_admin_menu_page);
                                    return;
                                }
                            }
                        }

                        if (AccessControl_GetCardList(Cards, ACCESS_CONTROL_MAX_CARD_SLOTS, &CardCount) != 0U)
                        {
                            if (CardCount == 0U)
                            {
                                (void)sprintf(Line1, "%s", UI_ZH_CARD_LIBRARY);
                                MenuUI_ShowLines(Line1, UI_ZH_NO_DATA, 0, UI_ZH_PRESS_KEY_CONTINUE);
                                if (AccessControl_WaitForAnyKey(&Key) == 0U)
                                {
                                    return;
                                }
                                if (Key == '*')
                                {
                                    MenuUI_ShowAdminMenu(s_admin_menu_page);
                                    return;
                                }
                            }
                            else
                            {
                                CardGroupCount = 0U;
                                for (i = 0U; i < CardCount; i++)
                                {
                                    if (CardGroupCount == 0U)
                                    {
                                        (void)sprintf(Line1, "%s(%u)", UI_ZH_CARD_LIBRARY, (unsigned int)CardCount);
                                        (void)sprintf(Line2, "%u:%02X%02X%02X%02X%02X",
                                                      (unsigned int)(i + 1U),
                                                      (unsigned int)Cards[i][0], (unsigned int)Cards[i][1],
                                                      (unsigned int)Cards[i][2], (unsigned int)Cards[i][3],
                                                      (unsigned int)Cards[i][4]);
                                        CardGroupCount = 1U;
                                    }
                                    else if (CardGroupCount == 1U)
                                    {
                                        (void)sprintf(Line3, "%u:%02X%02X%02X%02X%02X",
                                                      (unsigned int)(i + 1U),
                                                      (unsigned int)Cards[i][0], (unsigned int)Cards[i][1],
                                                      (unsigned int)Cards[i][2], (unsigned int)Cards[i][3],
                                                      (unsigned int)Cards[i][4]);
                                        CardGroupCount = 2U;
                                    }
                                    else
                                    {
                                        (void)sprintf(Line4, "%u:%02X%02X%02X%02X%02X",
                                                      (unsigned int)(i + 1U),
                                                      (unsigned int)Cards[i][0], (unsigned int)Cards[i][1],
                                                      (unsigned int)Cards[i][2], (unsigned int)Cards[i][3],
                                                      (unsigned int)Cards[i][4]);
                                        MenuUI_ShowLines(Line1, Line2, Line3, Line4);
                                        if (AccessControl_WaitForAnyKey(&Key) == 0U)
                                        {
                                            return;
                                        }
                                        if (Key == '*')
                                        {
                                            MenuUI_ShowAdminMenu(s_admin_menu_page);
                                            return;
                                        }
                                        CardGroupCount = 0U;
                                    }
                                }

                                if (CardGroupCount > 0U)
                                {
                                    if (CardGroupCount == 1U)
                                    {
                                        Line3[0] = '\0';
                                        Line4[0] = '\0';
                                    }
                                    else if (CardGroupCount == 2U)
                                    {
                                        Line4[0] = '\0';
                                    }
                                    MenuUI_ShowLines(Line1,
                                                     Line2,
                                                     (CardGroupCount > 1U) ? Line3 : 0,
                                                     UI_ZH_PRESS_KEY_CONTINUE);
                                    if (AccessControl_WaitForAnyKey(&Key) == 0U)
                                    {
                                        return;
                                    }
                                    if (Key == '*')
                                    {
                                        MenuUI_ShowAdminMenu(s_admin_menu_page);
                                        return;
                                    }
                                }
                            }
                        }

                        if (AccessCore_QueryRecentUnlockLogs(Logs, ACCESS_CONTROL_UNLOCK_HISTORY_MAX, &LogCount) != 0U)
                        {
                            if (LogCount == 0U)
                            {
                                (void)sprintf(Line1, "%s", UI_ZH_UNLOCK_HISTORY);
                                MenuUI_ShowLines(Line1, UI_ZH_NO_DATA, 0, UI_ZH_PRESS_KEY_CONTINUE);
                                if (AccessControl_WaitForAnyKey(&Key) == 0U)
                                {
                                    return;
                                }
                                if (Key == '*')
                                {
                                    MenuUI_ShowAdminMenu(s_admin_menu_page);
                                    return;
                                }
                            }
                            else
                            {
                                for (i = 0U; i < LogCount; i++)
                                {
                                    (void)sprintf(Line1, "%s(%u)", UI_ZH_UNLOCK_HISTORY, (unsigned int)LogCount);
                                    (void)sprintf(Line2, "%s", AccessControl_GetUnlockSourceName(Logs[i].Result));
                                    if (Logs[i].Year != 0U)
                                    {
                                        (void)sprintf(Line3, "%04u/%02u/%02u",
                                                      (unsigned int)Logs[i].Year,
                                                      (unsigned int)Logs[i].Month,
                                                      (unsigned int)Logs[i].Day);
                                        (void)sprintf(Line4, "%02u:%02u:%02u",
                                                      (unsigned int)Logs[i].Hour,
                                                      (unsigned int)Logs[i].Minute,
                                                      (unsigned int)Logs[i].Second);
                                    }
                                    else
                                    {
                                        (void)sprintf(Line3, "%s", UI_ZH_NO_TIMESTAMP);
                                        (void)sprintf(Line4, "%s", UI_ZH_RTC_OFFLINE);
                                    }

                                    MenuUI_ShowLines(Line1, Line2, Line3, Line4);
                                    if (AccessControl_WaitForAnyKey(&Key) == 0U)
                                    {
                                        return;
                                    }
                                    if (Key == '*')
                                    {
                                        MenuUI_ShowAdminMenu(s_admin_menu_page);
                                        return;
                                    }
                                }
                            }
                        }

                        MenuUI_ShowAdminMenu(s_admin_menu_page);
                    }
                    return;

                case '8':
                    {
                        uint8_t FpOk = 1U;
                        uint8_t RfidOk = 1U;
                        uint8_t FlashOk = 1U;
                        uint8_t RtcOk = 1U;

                        if (s_fault_mask & ACCESS_FAULT_FINGERPRINT)
                        {
                            uint32_t Address = AS608_DEFAULT_ADDRESS;
                            AS608_Init();
                            if (AS608_Handshake(&Address) == AS608_OK)
                            {
                                s_fp_available = 1U;
                                AS608_ReadSysPara(&s_fp_syspara);
                                s_fault_mask &= (uint8_t)~ACCESS_FAULT_FINGERPRINT;
                            }
                            else
                            {
                                FpOk = 0U;
                            }
                        }

                        if (s_fault_mask & ACCESS_FAULT_RFID)
                        {
                            RC522_Init();
                            uint8_t Version = RC522_GetVersion();
                            if ((Version != 0U) && (Version != 0xFFU))
                            {
                                s_rfid_available = 1U;
                                s_fault_mask &= (uint8_t)~ACCESS_FAULT_RFID;
                            }
                            else
                            {
                                RfidOk = 0U;
                            }
                        }

                        if (s_fault_mask & ACCESS_FAULT_FLASH)
                        {
                            W25Q64_Init();
                            uint8_t ManufacturerID, MemoryType, CapacityID;
                            if (W25Q64_ReadID(&ManufacturerID, &MemoryType, &CapacityID) == MI_OK)
                            {
                                s_flash_available = 1U;
                                s_fault_mask &= (uint8_t)~ACCESS_FAULT_FLASH;
                                AccessControl_InitLogStorage();
                            }
                            else
                            {
                                FlashOk = 0U;
                            }
                        }

                        if (s_fault_mask & ACCESS_FAULT_RTC)
                        {
                            AppRTC_Init();
                            if (AppRTC_IsReady() != 0U)
                            {
                                s_fault_mask &= (uint8_t)~ACCESS_FAULT_RTC;
                            }
                            else
                            {
                                RtcOk = 0U;
                            }
                        }

                        OLED_Clear();
                        OLED_ShowUTF8String(1U, 0U, UI_ZH_RETRY_RESULTS);
                        OLED_ShowUTF8String(2U, 0U, FpOk != 0U ? UI_ZH_FP_OK : UI_ZH_FP_FAIL);
                        OLED_ShowUTF8String(3U, 0U, RfidOk != 0U ? UI_ZH_RFID_OK : UI_ZH_RFID_FAIL);
                        OLED_ShowUTF8String(4U, 0U, FlashOk != 0U ? UI_ZH_FLASH_OK : UI_ZH_FLASH_FAIL);
                        
                        delay_ms(2000U);
                        
                        (void)AccessControl_LogEvent(ACCESS_LOG_EVENT_MODULE_FAULT, 1U, s_fault_mask, 0U);
                        MenuUI_ShowAdminMenu(s_admin_menu_page);
                    }
                    return;

                case '9':
                    s_admin_action = ADMIN_ACTION_SET_TIME;
                    AccessControl_ClearInput();
                    MenuUI_ShowAdminPrompt((uint8_t)s_admin_action, s_input_buffer);
                    return;

                case '0':
                    s_admin_action = ADMIN_ACTION_SET_DATE;
                    AccessControl_ClearInput();
                    MenuUI_ShowAdminPrompt((uint8_t)s_admin_action, s_input_buffer);
                    return;

                case '#':
                    s_admin_menu_page = (uint8_t)(s_admin_menu_page ^ 1U);
                    MenuUI_ShowAdminMenu(s_admin_menu_page);
                    return;

                case '*':
                    AccessControl_ExitAdminToIdle();
                    return;

                default:
                    break;
            }
        }
        else
        {
            if (Key == '*')
            {
                AccessControl_ReturnToAdminMenu();
                return;
            }

            if ((s_admin_action == ADMIN_ACTION_ADD_CARD) || (s_admin_action == ADMIN_ACTION_DELETE_CARD))
            {
                return;
            }

            if ((Key >= '0') && (Key <= '9'))
            {
                AccessControl_AppendInputChar(Key);
                MenuUI_UpdateInputDisplay(s_input_buffer);
                return;
            }

            if (Key == '#')
            {
                uint32_t Value = 0U;
                uint8_t OK = 0U;

                if (s_admin_action == ADMIN_ACTION_SET_TIME)
                {
                    /* Parse HHMMSS format (exactly 6 digits) */
                    if ((s_input_length == 6U) &&
                        (AccessControl_ParseDecimal(s_input_buffer, s_input_length, &Value) != 0U))
                    {
                        uint8_t Hour = (uint8_t)(Value / 10000U);
                        uint8_t Minute = (uint8_t)((Value / 100U) % 100U);
                        uint8_t Second = (uint8_t)(Value % 100U);

                        if ((Hour <= 23U) && (Minute <= 59U) && (Second <= 59U))
                        {
                            AppRTC_DateTime_t DateTime;
                            AppRTC_GetDateTime(&DateTime);
                            DateTime.Hour = Hour;
                            DateTime.Minute = Minute;
                            DateTime.Second = Second;
                            if (AppRTC_SetDateTime(&DateTime) != 0U)
                            {
                                OK = 1U;
                            }
                        }
                    }
                }
                else if (s_admin_action == ADMIN_ACTION_SET_DATE)
                {
                    /* Parse YYYYMMDD format (exactly 8 digits) */
                    if ((s_input_length == 8U) &&
                        (AccessControl_ParseDecimal(s_input_buffer, s_input_length, &Value) != 0U))
                    {
                        uint16_t Year = (uint16_t)(Value / 10000U);
                        uint8_t Month = (uint8_t)((Value / 100U) % 100U);
                        uint8_t Day = (uint8_t)(Value % 100U);

                        if ((Year >= 2000U) && (Year <= 2099U) &&
                            (Month >= 1U) && (Month <= 12U) &&
                            (Day >= 1U) && (Day <= 31U))
                        {
                            AppRTC_DateTime_t DateTime;
                            AppRTC_GetDateTime(&DateTime);
                            DateTime.Year = Year;
                            DateTime.Month = Month;
                            DateTime.Day = Day;
                            if (AppRTC_SetDateTime(&DateTime) != 0U)
                            {
                                OK = 1U;
                            }
                        }
                    }
                }
                else if (AccessControl_ParseDecimal(s_input_buffer, s_input_length, &Value) != 0U)
                {
                    if (s_admin_action == ADMIN_ACTION_DELETE_FINGERPRINT)
                    {
                        OK = AccessControl_DeleteFingerprint((uint16_t)Value);
                    }
                    else if ((s_admin_action == ADMIN_ACTION_CHANGE_USER_PASSWORD) ||
                             (s_admin_action == ADMIN_ACTION_CHANGE_ADMIN_PASSWORD))
                    {
                        OK = AccessControl_UpdatePassword(Value, s_admin_action);
                    }
                }

                AccessControl_ClearInput();

                if (OK != 0U)
                {
                    AccessControl_RecordSuccess();
                    if ((s_admin_action == ADMIN_ACTION_CHANGE_USER_PASSWORD) ||
                        (s_admin_action == ADMIN_ACTION_CHANGE_ADMIN_PASSWORD) ||
                        (s_admin_action == ADMIN_ACTION_SET_TIME) ||
                        (s_admin_action == ADMIN_ACTION_SET_DATE))
                    {
                        AccessControl_PlayPrompt(ACCESS_CONTROL_TRACK_SETTING_SUCCESS);
                    }
                    else
                    {
                        AccessControl_PlayPrompt(ACCESS_CONTROL_TRACK_DELETE_SUCCESS);
                    }
                    LED_ShowSuccess();
                    AccessControl_ReturnToAdminMenu();
                }
                else
                {
                    AccessControl_PlayPrompt(ACCESS_CONTROL_TRACK_DELETE_FAIL);
                    LED_ShowFail();
                    MenuUI_ShowAdminPrompt((uint8_t)s_admin_action, s_input_buffer);
                }
                return;
            }
        }
    }
}

static void AccessControl_HandleBluetoothFrame(void)
{
    Bluetooth_Frame_t Frame;
    uint32_t Value = 0U;

    if (s_fault_mask & ACCESS_FAULT_BLUETOOTH)
    {
        return;
    }

    /* Diagnostic: brief green LED flash on any BT RX activity */
    if (Bluetooth_RxActivityFlag != 0U)
    {
        Bluetooth_RxActivityFlag = 0U;
        LED_AllOff();
        GPIO_ResetBits(GPIOA, GPIO_Pin_12);
        delay_ms(5U);
        GPIO_SetBits(GPIOA, GPIO_Pin_12);
    }

    while (Bluetooth_GetFrame(&Frame) == BLUETOOTH_OK)
    {
        s_last_activity = AccessControl_Now();

        if (s_state == ACCESS_STATE_ADMIN_ENROLLING)
        {
            if (Frame.Cmd == 0x17U)
            {
                uint8_t Reply[4];
                Reply[0] = (uint8_t)s_state;
                Reply[1] = s_failure_count;
                Reply[2] = (uint8_t)(s_config.CardCount & 0xFFU);
                Reply[3] = (uint8_t)(AccessControl_GetFingerprintCapacity() & 0xFFU);
                Bluetooth_SendFrame(0x97U, Reply, 4U);
            }
            else if (Frame.Cmd == 0x18U)
            {
                AccessControl_ExitAdminToIdle();
            }
            else if (Frame.Cmd == ACCESS_CONTROL_BT_CMD_SET_TIME)
            {
                if (Frame.DataLen == 7U)
                {
                    AppRTC_DateTime_t DateTime;
                    uint8_t Reply[9];
                    uint8_t OK = 0U;

                    DateTime.Year = (uint16_t)(((uint16_t)Frame.Data[0] << 8) | (uint16_t)Frame.Data[1]);
                    DateTime.Month = Frame.Data[2];
                    DateTime.Day = Frame.Data[3];
                    DateTime.Hour = Frame.Data[4];
                    DateTime.Minute = Frame.Data[5];
                    DateTime.Second = Frame.Data[6];

                    if (AppRTC_SetDateTime(&DateTime) != 0U)
                    {
                        OK = 1U;
                    }

                    Reply[0] = OK;
                    if (OK != 0U)
                    {
                        AppRTC_GetDateTime(&DateTime);
                        Reply[1] = (uint8_t)((DateTime.Year >> 8) & 0xFFU);
                        Reply[2] = (uint8_t)(DateTime.Year & 0xFFU);
                        Reply[3] = DateTime.Month;
                        Reply[4] = DateTime.Day;
                        Reply[5] = DateTime.DayOfWeek;
                        Reply[6] = DateTime.Hour;
                        Reply[7] = DateTime.Minute;
                        Reply[8] = DateTime.Second;
                    }
                    else
                    {
                        memset(&Reply[1], 0, 8U);
                    }

                    Bluetooth_SendFrame(ACCESS_CONTROL_BT_RSP_SET_TIME, Reply, 9U);
                }
            }

            continue;
        }

        switch (Frame.Cmd)
        {
            case 0x01U:
                s_auth_source = AUTH_SOURCE_BLUETOOTH;
                if (AccessControl_ParseDecimal(Frame.Data, Frame.DataLen, &Value) != 0U)
                {
                    if (Value == s_config.UserPassword)
                    {
                        AccessControl_RecordSuccess();
                        AccessControl_ClearLockoutIfActive();
                        AccessControl_PlayPrompt(ACCESS_CONTROL_TRACK_WELCOME);
                        AccessControl_EnterState(ACCESS_STATE_UNLOCKED);
                    }
                    else
                    {
                        AccessControl_RecordPasswordFailure();
                    }
                }
                else
                {
                    AccessControl_RecordPasswordFailure();
                }
                break;

            case 0x10U:
                s_auth_source = AUTH_SOURCE_BLUETOOTH;
                if (AccessControl_ParseDecimal(Frame.Data, Frame.DataLen, &Value) != 0U)
                {
                    if (Value == s_config.AdminPassword)
                    {
                        AccessControl_RecordSuccess();
                        AccessControl_PlayPrompt(ACCESS_CONTROL_TRACK_ADMIN_LOGIN);
                        AccessControl_EnterState(ACCESS_STATE_ADMIN);
                    }
                    else
                    {
                        AccessControl_RecordPasswordFailure();
                    }
                }
                else
                {
                    AccessControl_RecordPasswordFailure();
                }
                break;

            case 0x11U:
                if (s_state == ACCESS_STATE_ADMIN)
                {
                    if (AccessControl_StartEnrollment() != 0U)
                    {
                        AccessControl_RecordSuccess();
                        LED_ShowSuccess();
                    }
                }
                break;

            case 0x12U:
                if (s_state == ACCESS_STATE_ADMIN)
                {
                    if (AccessControl_ParseDecimal(Frame.Data, Frame.DataLen, &Value) != 0U)
                    {
                        if (AccessControl_DeleteFingerprint((uint16_t)Value) != 0U)
                        {
                            AccessControl_RecordSuccess();
                            LED_ShowSuccess();
                        }
                        else
                        {
                            if (AccessControl_RecordFailure() != 0U)
                            {
                                break;
                            }
                        }
                    }
                    AccessControl_ReturnToAdminMenu();
                }
                break;

            case 0x13U:
                if (s_state == ACCESS_STATE_ADMIN)
                {
                    s_admin_action = ADMIN_ACTION_ADD_CARD;
                    MenuUI_ShowAdminPrompt((uint8_t)s_admin_action, s_input_buffer);
                }
                break;

            case 0x14U:
                if (s_state == ACCESS_STATE_ADMIN)
                {
                    s_admin_action = ADMIN_ACTION_DELETE_CARD;
                    MenuUI_ShowAdminPrompt((uint8_t)s_admin_action, s_input_buffer);
                }
                break;

            case 0x15U:
                if (s_state == ACCESS_STATE_ADMIN)
                {
                    if (AccessControl_ParseDecimal(Frame.Data, Frame.DataLen, &Value) != 0U)
                    {
                        if (AccessControl_UpdatePassword(Value, ADMIN_ACTION_CHANGE_USER_PASSWORD) != 0U)
                        {
                            AccessControl_RecordSuccess();
                            LED_ShowSuccess();
                        }
                        else
                        {
                            if (AccessControl_RecordFailure() != 0U)
                            {
                                break;
                            }
                        }
                    }
                    AccessControl_ReturnToAdminMenu();
                }
                break;

            case 0x16U:
                if (s_state == ACCESS_STATE_ADMIN)
                {
                    if (AccessControl_ParseDecimal(Frame.Data, Frame.DataLen, &Value) != 0U)
                    {
                        if (AccessControl_UpdatePassword(Value, ADMIN_ACTION_CHANGE_ADMIN_PASSWORD) != 0U)
                        {
                            AccessControl_RecordSuccess();
                            LED_ShowSuccess();
                        }
                        else
                        {
                            if (AccessControl_RecordFailure() != 0U)
                            {
                                break;
                            }
                        }
                    }
                    AccessControl_ReturnToAdminMenu();
                }
                break;

            case 0x17U:
                {
                    uint8_t Reply[4];
                    Reply[0] = (uint8_t)s_state;
                    Reply[1] = s_failure_count;
                    Reply[2] = (uint8_t)(s_config.CardCount & 0xFFU);
                    Reply[3] = (uint8_t)(AccessControl_GetFingerprintCapacity() & 0xFFU);
                    Bluetooth_SendFrame(0x97U, Reply, 4U);
                }
                break;

            case 0x18U:
                AccessControl_ExitAdminToIdle();
                break;

            case ACCESS_CONTROL_BT_CMD_SET_TIME:
                if (Frame.DataLen == 7U)
                {
                    AppRTC_DateTime_t DateTime;
                    uint8_t Reply[9];
                    uint8_t OK = 0U;

                    DateTime.Year = (uint16_t)(((uint16_t)Frame.Data[0] << 8) | (uint16_t)Frame.Data[1]);
                    DateTime.Month = Frame.Data[2];
                    DateTime.Day = Frame.Data[3];
                    DateTime.Hour = Frame.Data[4];
                    DateTime.Minute = Frame.Data[5];
                    DateTime.Second = Frame.Data[6];

                    if (AppRTC_SetDateTime(&DateTime) != 0U)
                    {
                        OK = 1U;
                    }

                    Reply[0] = OK;
                    if (OK != 0U)
                    {
                        AppRTC_GetDateTime(&DateTime);
                        Reply[1] = (uint8_t)((DateTime.Year >> 8) & 0xFFU);
                        Reply[2] = (uint8_t)(DateTime.Year & 0xFFU);
                        Reply[3] = DateTime.Month;
                        Reply[4] = DateTime.Day;
                        Reply[5] = DateTime.DayOfWeek;
                        Reply[6] = DateTime.Hour;
                        Reply[7] = DateTime.Minute;
                        Reply[8] = DateTime.Second;
                    }
                    else
                    {
                        memset(&Reply[1], 0, 8U);
                    }

                    Bluetooth_SendFrame(ACCESS_CONTROL_BT_RSP_SET_TIME, Reply, 9U);
                }
                break;

            default:
                break;
        }
    }
}

static void AccessControl_HandleCardScan(void)
{
    uint8_t TagType[2];
    uint8_t Uid[5];

    if ((s_state != ACCESS_STATE_IDLE) &&
        (s_state != ACCESS_STATE_SLEEP) &&
        (s_state != ACCESS_STATE_LOCKOUT) &&
        !((s_state == ACCESS_STATE_ADMIN) &&
          ((s_admin_action == ADMIN_ACTION_ADD_CARD) || (s_admin_action == ADMIN_ACTION_DELETE_CARD))))
    {
        return;
    }

    if (s_rfid_available == 0U)
    {
        return;
    }

    if (s_fault_mask & ACCESS_FAULT_RFID)
    {
        return;
    }

    if ((AccessControl_Now() - s_last_card_scan) < ACCESS_CONTROL_CARD_SCAN_PERIOD_MS)
    {
        return;
    }
    s_last_card_scan = AccessControl_Now();

    if (RC522_Request(PICC_REQIDL, TagType) != MI_OK)
    {
        return;
    }

    s_auth_source = AUTH_SOURCE_CARD;

    if (RC522_Anticoll(Uid) != MI_OK)
    {
        if (s_state == ACCESS_STATE_SLEEP)
        {
            AccessControl_EnterState(ACCESS_STATE_IDLE);
        }
        return;
    }

    if ((s_state == ACCESS_STATE_SLEEP) || (s_state == ACCESS_STATE_LOCKOUT))
    {
        if (AccessControl_FindCardIndex(Uid) >= 0)
        {
            AccessControl_RecordSuccess();
            (void)AccessControl_LogEvent(ACCESS_LOG_EVENT_UNLOCK, ACCESS_LOG_SOURCE_CARD, AccessControl_CalcChecksum(Uid, 5U), 0U);
            AccessControl_ClearLockoutIfActive();
            AccessControl_PlayPrompt(ACCESS_CONTROL_TRACK_CARD_UNLOCK);
            AccessControl_EnterState(ACCESS_STATE_UNLOCKED);
        }
        else if (s_state == ACCESS_STATE_SLEEP)
        {
            if (AccessControl_RecordFailure() != 0U)
            {
                return;
            }

            AccessControl_EnterState(ACCESS_STATE_IDLE);
        }
        return;
    }

    if ((s_state == ACCESS_STATE_ADMIN) && (s_admin_action == ADMIN_ACTION_ADD_CARD))
    {
        if (AccessControl_AddCard(Uid) != 0U)
        {
            AccessControl_RecordSuccess();
            AccessControl_PlayPrompt(ACCESS_CONTROL_TRACK_ADD_CARD_SUCCESS);
            LED_ShowSuccess();
            RC522_Halt();
            MenuUI_ShowLines(UI_ZH_ADD_CARD, UI_ZH_ENROLL_SUCCESS, 0, 0);
            delay_ms(1200U);
        }
        else
        {
            AccessControl_PlayPrompt(ACCESS_CONTROL_TRACK_ADD_CARD_FAIL);
            RC522_Halt();
            LED_ShowFail();
            MenuUI_ShowLines(UI_ZH_ADD_CARD, UI_ZH_ENROLL_FAIL, 0, 0);
            delay_ms(1200U);
        }

        AccessControl_ReturnToAdminMenu();
        return;
    }

    if ((s_state == ACCESS_STATE_ADMIN) && (s_admin_action == ADMIN_ACTION_DELETE_CARD))
    {
        if (AccessControl_DeleteCard(Uid) != 0U)
        {
            AccessControl_RecordSuccess();
            AccessControl_PlayPrompt(ACCESS_CONTROL_TRACK_DELETE_SUCCESS);
            LED_ShowSuccess();
            RC522_Halt();
        }
        else
        {
            AccessControl_PlayPrompt(ACCESS_CONTROL_TRACK_DELETE_FAIL);
            RC522_Halt();
            LED_ShowFail();
        }

        AccessControl_ReturnToAdminMenu();
        return;
    }

    if (AccessControl_FindCardIndex(Uid) >= 0)
    {
        AccessControl_RecordSuccess();
        (void)AccessControl_LogEvent(ACCESS_LOG_EVENT_UNLOCK, ACCESS_LOG_SOURCE_CARD, AccessControl_CalcChecksum(Uid, 5U), 0U);
        AccessControl_ClearLockoutIfActive();
        AccessControl_PlayPrompt(ACCESS_CONTROL_TRACK_CARD_UNLOCK);
        AccessControl_EnterState(ACCESS_STATE_UNLOCKED);
    }
    else
    {
        AccessControl_PlayPrompt(ACCESS_CONTROL_TRACK_CARD_FAIL);
        Buzzer_Beep(2200U, 40U);
        delay_ms(60U);
        Buzzer_Beep(2200U, 40U);
        delay_ms(60U);
        Buzzer_Beep(2200U, 40U);
        LED_ShowFail();
        MenuUI_ShowLines(UI_ZH_UNKNOWN_CARD, 0, 0, 0);
        delay_ms(1000U);
        LED_AllOff();
        MenuUI_ShowIdleScreen(UI_ZH_IDLE_HINT);
        MenuUI_UpdateIdleClock(s_fault_mask);
    }
}

static void AccessControl_HandleFingerprintScan(void)
{
    uint16_t PageId = 0U;
    uint16_t Score = 0U;
    uint16_t Capacity;

    if ((s_state != ACCESS_STATE_IDLE) &&
        (s_state != ACCESS_STATE_SLEEP) &&
        (s_state != ACCESS_STATE_LOCKOUT))
    {
        return;
    }

    if ((AccessControl_Now() - s_last_fp_scan) < ACCESS_CONTROL_FP_SCAN_PERIOD_MS)
    {
        return;
    }
    s_last_fp_scan = AccessControl_Now();

    if (s_fp_available == 0U)
    {
        return;
    }

    if (s_fault_mask & ACCESS_FAULT_FINGERPRINT)
    {
        return;
    }

    s_auth_source = AUTH_SOURCE_FINGERPRINT;

    /* Step 1: Capture fingerprint image */
    if (AS608_GetImage() != AS608_OK)
    {
        if (s_state == ACCESS_STATE_SLEEP)
        {
            AccessControl_EnterState(ACCESS_STATE_IDLE);
        }
        return;
    }

    /* Step 2: Generate feature file in Buffer 1 */
    if (AS608_Image2Tz(AS608_BUFFER_1) != AS608_OK)
    {
        return;
    }

    /* Step 3: Search fingerprint library (1:N) */
    Capacity = AccessControl_GetFingerprintCapacity();
    if (Capacity > ACCESS_CONTROL_FINGERPRINT_MAX_ID)
    {
        Capacity = ACCESS_CONTROL_FINGERPRINT_MAX_ID;
    }
    if (Capacity == 0U)
    {
        Capacity = 200U;
    }

    if (AS608_Search(AS608_BUFFER_1, 1U, Capacity, &PageId, &Score) == AS608_OK)
    {
        AccessControl_RecordSuccess();
        (void)AccessControl_LogEvent(ACCESS_LOG_EVENT_UNLOCK, ACCESS_LOG_SOURCE_FINGERPRINT, PageId, Score);
        AccessControl_ClearLockoutIfActive();
        AccessControl_PlayPrompt(ACCESS_CONTROL_TRACK_FINGERPRINT_UNLOCK);
        AccessControl_EnterState(ACCESS_STATE_UNLOCKED);
    }
    else
    {
        AccessControl_PlayPrompt(ACCESS_CONTROL_TRACK_FINGERPRINT_FAIL);
        Buzzer_Beep(2200U, 40U);
        delay_ms(60U);
        Buzzer_Beep(2200U, 40U);
        delay_ms(60U);
        Buzzer_Beep(2200U, 40U);
        LED_ShowFail();
        MenuUI_ShowLines(UI_ZH_FP_NOT_MATCH, 0, 0, 0);
        delay_ms(1000U);
        LED_AllOff();
        if (s_state == ACCESS_STATE_SLEEP)
        {
            AccessControl_EnterState(ACCESS_STATE_IDLE);
        }
        else
        {
            MenuUI_ShowIdleScreen(UI_ZH_IDLE_HINT);
            MenuUI_UpdateIdleClock(s_fault_mask);
        }
    }
}

static void AccessControl_HandleTimeouts(void)
{
    uint32_t Now = AccessControl_Now();

    if (s_state == ACCESS_STATE_UNLOCKED)
    {
        if ((int32_t)(Now - s_state_deadline) >= 0)
        {
            AccessControl_PlayPrompt(ACCESS_CONTROL_TRACK_AUTO_LOCK);
            Servo_Lock();
            LED_AllOff();
            AccessControl_EnterState(ACCESS_STATE_IDLE);
        }
        return;
    }

    if (s_state == ACCESS_STATE_LOCKOUT)
    {
        if ((int32_t)(Now - s_state_deadline) >= 0)
        {
            (void)AccessControl_LogEvent(ACCESS_LOG_EVENT_LOCKOUT_END, 1U, ACCESS_CONTROL_LOCKOUT_MS, 0U);
            AccessControl_SetLockoutFlag(0U);
            s_password_failure_count = 0U;
            LED_AllOff();
            AccessControl_EnterState(ACCESS_STATE_IDLE);
        }
        return;
    }

    if (s_state == ACCESS_STATE_ADMIN_ENROLLING)
    {
        if ((int32_t)(Now - s_enroll_deadline) >= 0)
        {
            AccessControl_AbortEnrollmentToAdmin();
        }
        return;
    }

    if ((s_state == ACCESS_STATE_IDLE) ||
        (s_state == ACCESS_STATE_AUTH) ||
        (s_state == ACCESS_STATE_ADMIN))
    {
        if ((Now - s_last_refresh) >= ACCESS_CONTROL_DISPLAY_PERIOD_MS)
        {
            s_last_refresh = Now;

            if (s_state == ACCESS_STATE_IDLE)
            {
                MenuUI_UpdateIdleClock(s_fault_mask);
            }
        }

        if ((Now - s_last_activity) >= ACCESS_CONTROL_IDLE_SLEEP_MS)
        {
            AccessControl_EnterState(ACCESS_STATE_SLEEP);
        }
        return;
    }

    if (s_state == ACCESS_STATE_SLEEP)
    {
        if ((s_lowpower_window_active != 0U) &&
            ((int32_t)(Now - s_lowpower_wake_deadline) >= 0))
        {
            AccessControl_EnterState(ACCESS_STATE_SLEEP);
        }
        return;
    }
}

static void AccessControl_RunSelfTest(void)
{
    uint32_t Address;
    uint8_t Version;

    MenuUI_ShowBootScreen();

    s_fp_available = 0U;
    s_rfid_available = 0U;
    s_fault_mask = ACCESS_FAULT_NONE;

    Address = AS608_DEFAULT_ADDRESS;
    if (AS608_Handshake(&Address) == AS608_OK)
    {
        s_fp_available = 1U;
        AS608_ReadSysPara(&s_fp_syspara);
    }
    else
    {
        s_fault_mask |= ACCESS_FAULT_FINGERPRINT;
    }

    Version = RC522_GetVersion();
    if ((Version != 0U) && (Version != 0xFFU))
    {
        s_rfid_available = 1U;
    }
    else
    {
        s_fault_mask |= ACCESS_FAULT_RFID;
    }

    if (s_flash_available == 0U)
    {
        s_fault_mask |= ACCESS_FAULT_FLASH;
    }

    if (AppRTC_IsReady() == 0U)
    {
        s_fault_mask |= ACCESS_FAULT_RTC;
    }

    /* JQ8900 self-test: try to wake and query, retry if not ready */
    {
        uint8_t PlayState;
        uint8_t Retry;

        for (Retry = 0U; Retry < 5U; Retry++)
        {
            JQ8900_QueryPlayState();
            delay_ms(300U);
            if (JQ8900_ReadPlayState(&PlayState) == MI_OK)
            {
                break;
            }
        }
        if (Retry >= 5U)
        {
            s_fault_mask |= ACCESS_FAULT_VOICE;
        }
    }

    MenuUI_ShowSelfTestResult(s_fp_available, s_rfid_available, s_flash_available);

    if (s_fault_mask != ACCESS_FAULT_NONE)
    {
        (void)AccessControl_LogEvent(ACCESS_LOG_EVENT_MODULE_FAULT, 0U, s_fault_mask, 0U);
        AccessControl_PlayPrompt(ACCESS_CONTROL_TRACK_MODULE_FAULT);
        delay_ms(2000U);
    }
    else if (s_boot_prompt_played == 0U)
    {
        s_boot_prompt_played = 1U;
        AccessControl_PlayPrompt(ACCESS_CONTROL_TRACK_BOOT);
    }
    delay_ms(400U);
}

static void AccessControl_InitHardware(void)
{
    delay_init();
    AppRTC_Init();
    AccessControl_ConfigWakeupPin();
    LED_Init();
    Buzzer_Init();
    Servo_Init();
    MySPI_Init();
    AccessControl_ForceSpiCsHigh();
    MenuUI_Init();
    Keypad_Init();
    Bluetooth_Init();
    JQ8900_Init();
    AS608_Init();
    RC522_Init();
    W25Q64_Init();
    Buzzer_Off();
    LED_AllOff();
    Timer_Init();
}

uint8_t AccessCore_QueryRecentLogs(AccessCore_LogEntry_t *OutEntries,
                                   uint8_t MaxCount,
                                   uint8_t *OutActualCount)
{
    uint8_t Buffer[ACCESS_CONTROL_LOG_RECORD_SIZE];
    uint16_t Sector, Entry;
    uint16_t CheckedSlots = 0U;
    uint16_t TotalSlots = ACCESS_CONTROL_LOG_SECTOR_COUNT * ACCESS_CONTROL_LOG_ENTRIES_PER_SECTOR;
    uint8_t FoundCount = 0U;
    uint32_t Address;
    uint32_t Seq;

    if ((OutEntries == 0) || (OutActualCount == 0) || (MaxCount == 0U))
    {
        return 0U;
    }

    *OutActualCount = 0U;
    if ((s_flash_available == 0U) || (s_log_available == 0U))
    {
        return 0U;
    }

    Sector = s_log_sector;
    Entry = s_log_entry;

    while ((CheckedSlots < TotalSlots) && (FoundCount < MaxCount))
    {
        /* Move backwards to the previous slot */
        if (Entry > 0U)
        {
            Entry--;
        }
        else
        {
            if (Sector == ACCESS_CONTROL_LOG_SECTOR_FIRST)
            {
                Sector = (uint16_t)(ACCESS_CONTROL_LOG_SECTOR_FIRST + ACCESS_CONTROL_LOG_SECTOR_COUNT - 1U);
            }
            else
            {
                Sector--;
            }
            Entry = (uint16_t)(ACCESS_CONTROL_LOG_ENTRIES_PER_SECTOR - 1U);
        }

        CheckedSlots++;

        Address = AccessControl_LogSectorAddress(Sector) + ((uint32_t)Entry * ACCESS_CONTROL_LOG_RECORD_SIZE);
        W25Q64_ReadBytes(Address, Buffer, ACCESS_CONTROL_LOG_RECORD_SIZE);

        if (AccessControl_LogDecodeRecord(Buffer, &Seq) != 0U)
        {
            OutEntries[FoundCount].Sequence = Seq;
            OutEntries[FoundCount].Event = Buffer[6U];
            OutEntries[FoundCount].Result = Buffer[7U];
            OutEntries[FoundCount].Year = AccessControl_ReadU16(Buffer, 8U);
            OutEntries[FoundCount].Month = Buffer[10U];
            OutEntries[FoundCount].Day = Buffer[11U];
            OutEntries[FoundCount].Hour = Buffer[13U];
            OutEntries[FoundCount].Minute = Buffer[14U];
            OutEntries[FoundCount].Second = Buffer[15U];
            OutEntries[FoundCount].Value = AccessControl_ReadU32(Buffer, 16U);
            OutEntries[FoundCount].Extra = AccessControl_ReadU32(Buffer, 20U);
            FoundCount++;
        }
    }

    *OutActualCount = FoundCount;
    return 1U;
}

void AccessCore_Init(void)
{
    AccessControl_InitHardware();
    AccessControl_LoadConfig();
    AccessControl_EnterState(ACCESS_STATE_INIT);
    AccessControl_RunSelfTest();
    if (s_config.IsLockedOut != 0U)
    {
        AccessControl_EnterState(ACCESS_STATE_LOCKOUT);
    }
    else
    {
        AccessControl_EnterState(ACCESS_STATE_IDLE);
    }
}

void AccessCore_Task(void)
{
    uint8_t Key;

    Bluetooth_Task();
    AccessControl_HandleBluetoothFrame();

    if (Keypad_KeyFlag != 0U)
    {
        Key = Keypad_KeyValue;
        Keypad_KeyFlag = 0U;
        AccessControl_HandleKey(Key);
    }

    AccessControl_HandleEnrollment();
    AccessControl_HandleCardScan();
    AccessControl_HandleFingerprintScan();
    AccessControl_HandleTimeouts();
}
