#ifndef __ACCESS_CORE_H
#define __ACCESS_CORE_H

#include "stm32f10x.h"

typedef enum
{
    ACCESS_LOG_EVENT_UNLOCK = 1U,
    ACCESS_LOG_EVENT_FAILURE = 2U,
    ACCESS_LOG_EVENT_ADMIN_ADD_FINGERPRINT = 3U,
    ACCESS_LOG_EVENT_ADMIN_DELETE_FINGERPRINT = 4U,
    ACCESS_LOG_EVENT_ADMIN_ADD_CARD = 5U,
    ACCESS_LOG_EVENT_ADMIN_DELETE_CARD = 6U,
    ACCESS_LOG_EVENT_ADMIN_CHANGE_USER_PASSWORD = 7U,
    ACCESS_LOG_EVENT_ADMIN_CHANGE_ADMIN_PASSWORD = 8U,
    ACCESS_LOG_EVENT_LOCKOUT_START = 9U,
    ACCESS_LOG_EVENT_LOCKOUT_END = 10U,
    ACCESS_LOG_EVENT_MODULE_FAULT = 11U
} AccessLogEvent_t;

typedef enum
{
    ACCESS_LOG_SOURCE_UNKNOWN = 0U,
    ACCESS_LOG_SOURCE_KEYPAD_USER = 1U,
    ACCESS_LOG_SOURCE_KEYPAD_ADMIN = 2U,
    ACCESS_LOG_SOURCE_CARD = 3U,
    ACCESS_LOG_SOURCE_FINGERPRINT = 4U,
    ACCESS_LOG_SOURCE_BLUETOOTH = 5U
} AccessLogSource_t;

typedef enum
{
    ACCESS_STATE_INIT = 0U,
    ACCESS_STATE_IDLE,
    ACCESS_STATE_AUTH,
    ACCESS_STATE_UNLOCKED,
    ACCESS_STATE_ADMIN,
    ACCESS_STATE_ADMIN_ENROLLING,
    ACCESS_STATE_LOCKOUT,
    ACCESS_STATE_SLEEP,
    ACCESS_STATE_FAULT
} AccessState_t;

typedef enum
{
    AUTH_SOURCE_NONE = 0U,
    AUTH_SOURCE_KEYPAD_USER,
    AUTH_SOURCE_KEYPAD_ADMIN,
    AUTH_SOURCE_CARD,
    AUTH_SOURCE_FINGERPRINT,
    AUTH_SOURCE_BLUETOOTH
} AuthSource_t;

typedef enum
{
    ADMIN_ACTION_NONE = 0U,
    ADMIN_ACTION_DELETE_FINGERPRINT,
    ADMIN_ACTION_ADD_CARD,
    ADMIN_ACTION_DELETE_CARD,
    ADMIN_ACTION_CHANGE_USER_PASSWORD,
    ADMIN_ACTION_CHANGE_ADMIN_PASSWORD,
    ADMIN_ACTION_SET_TIME,
    ADMIN_ACTION_SET_DATE
} AdminAction_t;

typedef enum
{
    ENROLL_STAGE_IDLE = 0U,
    ENROLL_STAGE_WAIT_FIRST_PRESS,
    ENROLL_STAGE_CAPTURE_FIRST,
    ENROLL_STAGE_WAIT_FIRST_RELEASE,
    ENROLL_STAGE_WAIT_SECOND_PRESS,
    ENROLL_STAGE_CAPTURE_SECOND,
    ENROLL_STAGE_REG_MODEL,
    ENROLL_STAGE_DUP_CHECK,
    ENROLL_STAGE_STORE_MODEL
} EnrollStage_t;

void AccessCore_Init(void);
void AccessCore_Task(void);

#define ACCESS_CORE_LOG_QUERY_MAX_COUNT  8U

typedef struct
{
    uint32_t Sequence;
    uint16_t Year;
    uint8_t  Month;
    uint8_t  Day;
    uint8_t  Hour;
    uint8_t  Minute;
    uint8_t  Second;
    uint8_t  Event;
    uint8_t  Result;
    uint32_t Value;
    uint32_t Extra;
} AccessCore_LogEntry_t;

uint8_t AccessCore_QueryRecentLogs(AccessCore_LogEntry_t *OutEntries,
                                   uint8_t MaxCount,
                                   uint8_t *OutActualCount);
uint8_t AccessCore_QueryRecentUnlockLogs(AccessCore_LogEntry_t *OutEntries,
                                         uint8_t MaxCount,
                                         uint8_t *OutActualCount);

/* 故障掩码位定义 */
#define ACCESS_FAULT_NONE        0x00U
#define ACCESS_FAULT_FINGERPRINT 0x01U  /* Bit 0: AS608 */
#define ACCESS_FAULT_RFID        0x02U  /* Bit 1: RC522 */
#define ACCESS_FAULT_FLASH       0x04U  /* Bit 2: W25Q64 */
#define ACCESS_FAULT_RTC         0x08U  /* Bit 3: 内部 RTC */
#define ACCESS_FAULT_VOICE       0x10U  /* Bit 4: JQ8900 */
#define ACCESS_FAULT_BLUETOOTH   0x20U  /* Bit 5: JDY-31 */

#endif /* __ACCESS_CORE_H */
