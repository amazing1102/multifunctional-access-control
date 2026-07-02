#include "iwdg.h"

/**
  * @brief  Configure the Independent Watchdog (IWDG)
  *         LSI is 40kHz, Prescaler is 64, Reload value is 1250
  *         Timeout = 1250 / (40000 / 64) = 1250 / 625 = 2.0s
  * @param  None
  * @retval None
  */
void IWDG_Config(void)
{
    /* Enable write access to IWDG_PR and IWDG_RLR registers */
    IWDG_WriteAccessCmd(IWDG_WriteAccess_Enable);
    
    /* Set IWDG prescaler to 64 */
    IWDG_SetPrescaler(IWDG_Prescaler_64);
    
    /* Set IWDG reload value to 1250 */
    IWDG_SetReload(1250);
    
    /* Reload IWDG counter */
    IWDG_ReloadCounter();
    
    /* Enable IWDG (starts the watchdog) */
    IWDG_Enable();
}

/**
  * @brief  Feed the Independent Watchdog (Reload counter)
  * @param  None
  * @retval None
  */
void IWDG_Feed(void)
{
    IWDG_ReloadCounter();
}
