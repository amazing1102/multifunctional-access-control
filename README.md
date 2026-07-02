# 多功能智能门禁系统

基于 STM32F103C8T6 的多锁式智能门禁系统，支持指纹、IC卡、密码及蓝牙四种认证方式，配备 OLED 人机交互界面和管理员后台。

## 硬件平台

| 组件 | 型号 | 接口 |
|------|------|------|
| 主控 | STM32F103C8T6 (Cortex-M3, 72MHz) | — |
| 指纹模块 | AS608 | UART |
| RFID 读卡器 | RC522 | SPI |
| 蓝牙模块 | JDY-31 | UART |
| 语音模块 | JQ8900 | UART |
| OLED 显示屏 | SSD1306 (128×64) | I2C |
| 矩阵键盘 | 4×4 | GPIO |
| SPI Flash | W25Q64 (8MB) | SPI |
| 舵机 | SG90 | PWM (TIM) |
| 蜂鸣器 | 有源 | GPIO |
| LED 指示灯 | 双色 | GPIO |

## 功能特性

### 四种认证方式

| 方式 | 说明 |
|------|------|
| 指纹 | AS608 光学指纹模块，1:N 比对，支持录入/删除 |
| IC卡 | RC522 读取 UID，支持注册/注销 |
| 密码 | 4-6 位用户密码 + 管理员独立密码 |
| 蓝牙 | JDY-31 串口透传，手机端发送开锁指令 |

### 管理员系统

- OLED 多级菜单操作
- 指纹录入/删除、IC卡注册/注销
- 用户密码修改、管理员密码修改
- 系统时间/日期设置
- 日志查询（最近 8 条开锁记录和异常记录）

### 安全策略

- 连续 5 次认证失败 → 锁定 30 秒（声光提示）
- 低功耗休眠唤醒（按键中断唤醒）
- 模块故障检测：指纹/RFID/Flash/RTC/语音/蓝牙，异常时界面提示

### 事件日志

- 11 种事件类型（开锁、失败、录入、删除、锁定等）
- RTC 时间戳（年/月/日/时/分/秒）
- W25Q64 Flash 存储，掉电保存

## 目录结构

```
├─ Hardware/        # 外设驱动（AS608/RC522/蓝牙/OLED/键盘/W25Q64/舵机/蜂鸣器）
├─ Library/         # STM32F10x 标准外设库
├─ Start/           # 启动文件（core_cm3, system_stm32f10x）
├─ System/          # 系统组件（Delay/IWDG/SPI/RTC/Timer）
├─ User/
│   ├─ App/         # 应用层（access_core 状态机 + menu_ui 菜单界面）
│   └─ main.c       # 入口
├─ project.uvprojx  # Keil MDK 工程文件
└─ README.md
```

## 构建与烧录

使用 Keil MDK v5 打开 `project.uvprojx`，编译后通过 ST-Link / CMSIS-DAP 烧录。

## 系统状态机

```
INIT → IDLE → AUTH → UNLOCKED → IDLE
                 ↓
              LOCKOUT (连续失败) → IDLE
                 ↓
              ADMIN (管理员密码) → 各项管理操作 → IDLE
                 ↓
              SLEEP (低功耗) → IDLE (按键唤醒)
```
