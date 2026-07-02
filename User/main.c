#include "stm32f10x.h"                  // Device header
#include "App/access_core.h"
#include "iwdg.h"

int main(void)
{
    AccessCore_Init();   /* 初始化所有外设、加载配置、自检 */
    IWDG_Config();       /* 系统稳定后再启动看门狗，防止自检时被超时复位 */

    while (1)
    {
        AccessCore_Task();
        IWDG_Feed();
        __WFI();
    }
}


