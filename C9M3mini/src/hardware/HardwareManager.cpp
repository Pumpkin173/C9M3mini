#include "HardwareManager.h"
#include <QDebug>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>
#include <errno.h>
#include <cstring>
#include <linux/gpio.h>

#ifndef GPIOHANDLE_REQUEST_BIAS_PULL_UP
#define GPIOHANDLE_REQUEST_BIAS_PULL_UP (1UL << 5)
#endif

// AW9523 寄存器定义
#define AW9523_REG_INPUT0     0x00
#define AW9523_REG_INPUT1     0x01  // P1 Input Port
#define AW9523_REG_OUTPUT1    0x03  // P1 Output Port
#define AW9523_REG_CONFIG0    0x04  // P0 Config
#define AW9523_REG_CONFIG1    0x05  // P1 Config (0=Output, 1=Input)
#define AW9523_REG_LEDMODE_P1 0x13  // P1 LED/GPIO Mode (0=LED, 1=GPIO)
#define AW9523_REG_SW_RSTN    0x7F  // Software Reset
#define AW9523_REG_CHIP_ID    0x10  // ID Register (Read only, should be 0x23)

// AD0=GND, AD1=GND -> Address is 0x58
#define AW9523_ADDR           0x58

HardwareManager::HardwareManager(QObject* parent) : QThread(parent) {
    stopFlag = false;
}

HardwareManager::~HardwareManager() {
    stop();
    wait();
}

void HardwareManager::stop() {
    stopFlag = true;
}

void HardwareManager::setTallyLed(bool turnOn) {
    m_tallyLedState = turnOn;
    m_tallyLedUpdatePending = true;
}

void HardwareManager::run() {
    qDebug() << "Hardware: Thread starting...";

    // 初始化 I2C (AW9523)
    int fd = open("/dev/i2c-1", O_RDWR);
    if (fd < 0 || ioctl(fd, I2C_SLAVE, AW9523_ADDR) < 0) {
        qDebug() << "Hardware: I2C init failed.";
        if (fd >= 0) close(fd);
        return;
    }

    // 复位并设置 P1 为 GPIO 模式
    i2cWriteByte(fd, AW9523_REG_SW_RSTN, 0x00);
    usleep(50000); // 加长等待确保复位完成 (50ms)
    i2cWriteByte(fd, AW9523_REG_LEDMODE_P1, 0xFF); // P1 全部设为 GPIO 模式

    // 默认关闭 Tally LED (P1_0 拉高，因为发光二极管阴极接在 P1_0)
    i2cWriteByte(fd, AW9523_REG_OUTPUT1, 0x01); // 保证 P1_0 输出高电平

    // 确认 LEDMODE 写入成功
    unsigned char ledmode = 0;
    i2cReadByte(fd, AW9523_REG_LEDMODE_P1, ledmode);
    qDebug() << "Hardware: AW9523 P1 LEDMODE register =" << Qt::hex << (int)ledmode
             << "(expected: 0xFF = all GPIO mode)";

    // 配置引脚输入输出
    unsigned char cfg0 = 0, cfg1 = 0;
    i2cReadByte(fd, AW9523_REG_CONFIG0, cfg0);
    i2cReadByte(fd, AW9523_REG_CONFIG1, cfg1);

    cfg0 |= 0x80; // P0_7 配置为输入 (拍照/视频切换)
    cfg0 |= 0x20; // P0_5 配置为输入 (相册按钮)
    cfg0 |= 0x10; // P0_4 配置为输入 (del 按键)
    cfg0 |= 0x40; // P0_6 配置为输入 (Fn 键，用于退出播放)
    cfg0 |= 0x0F; // P0_0~P0_3 配置为输入 (档位转盘，公共端接GND)

    cfg1 &= ~0x01; // P1_0 配置为输出 (Tally LED)
    cfg1 |= 0x0E;  // P1_1(E), P1_2(B), P1_3(A) 配置为输入 (EC11)
    cfg1 |= 0x10;  // P1_4 配置为输入 (Menu 键)
    cfg1 |= 0x20; // P1_5 配置为输入 (快门)
    cfg1 |= 0x80; // P1_7 配置为输入 (录制按键 1)

    i2cWriteByte(fd, AW9523_REG_CONFIG0, cfg0);
    i2cWriteByte(fd, AW9523_REG_CONFIG1, cfg1);
    qDebug() << "Hardware: AW9523 CONFIG0 =" << Qt::hex << (int)cfg0
             << "| CONFIG1 =" << Qt::hex << (int)cfg1;

    // 初始化 GPIO
    int gpio_fd = open("/dev/gpiochip4", O_RDWR);
    int line_fd_25 = -1;
    if (gpio_fd >= 0) {
        struct gpiohandle_request req;
        memset(&req, 0, sizeof(req));
        req.lineoffsets[0] = 25;
        req.flags = GPIOHANDLE_REQUEST_INPUT | GPIOHANDLE_REQUEST_BIAS_PULL_UP;
        req.lines = 1;
        strcpy(req.consumer_label, "video_rec_redundant");
        if (ioctl(gpio_fd, GPIO_GET_LINEHANDLE_IOCTL, &req) == 0) {
            line_fd_25 = req.fd;
            qDebug() << "Hardware: GPIO 25 initialized for redundant trigger.";
        } else {
            qDebug() << "Hardware: Request input failed.";
        }
    } else {
        qDebug() << "Hardware: Failed to open gpiochip4. Redundant trigger offline.";
    }

    auto requestJoystickPin = [&](int offset, const char* label) -> int {
        if (gpio_fd < 0) return -1;
        struct gpiohandle_request req;
        memset(&req, 0, sizeof(req));
        req.lineoffsets[0] = offset;
        req.flags = GPIOHANDLE_REQUEST_INPUT | GPIOHANDLE_REQUEST_BIAS_PULL_UP;
        req.lines = 1;
        strcpy(req.consumer_label, label);
        if (ioctl(gpio_fd, GPIO_GET_LINEHANDLE_IOCTL, &req) == 0) {
            qDebug() << "Hardware: GPIO" << offset << "initialized for" << label;
            return req.fd;
        }
        return -1;
    };

    int line_fd_16 = requestJoystickPin(16, "joystick_center");
    int line_fd_5  = requestJoystickPin(5,  "joystick_left");
    int line_fd_6  = requestJoystickPin(6,  "joystick_down");
    int line_fd_22 = requestJoystickPin(22, "joystick_up");
    int line_fd_26 = requestJoystickPin(26, "joystick_right");

    // 闪光灯与硬件触发引脚 (GPIO 17, 23) 已移交 PioTriggerController
    // m_flashLineFd = -1; 


    // 状态记录变量
    bool lastShutterState = true;
    bool lastRecState = true;
    bool lastModeIsVideo = false; // 初始假设为拍照模式
    bool lastAlbumState = true;   // 初始假设相册按键未按下 (主动低电平)
    bool lastMenuState  = true;   // P1_4 Menu 按键，初始未按下
    bool lastFnState    = true;   // P0_6 Fn 按键
    bool lastDelState   = true;   // P0_4 Del 按键
    
    // Joystick state
    bool lastJoyCenter = false; 
    bool lastJoyLeft   = false;
    bool lastJoyRight  = false;
    bool lastJoyUp     = false;
    bool lastJoyDown   = false;

    int delMenuHoldCount = 0;
    bool exitTriggered = false;
    int lastDriveMode = -1;       // 记录上一次的档位转盘状态
    int m_rawDriveModeState = -1; // 用于防震荡的中间状态
    int m_driveModeStableCount = 0;

    qDebug() << "Hardware: Polling started.";

    while (!stopFlag) {
        if (m_tallyLedUpdatePending) {
            m_tallyLedUpdatePending = false;
            unsigned char out1 = 0;
            if (i2cReadByte(fd, AW9523_REG_OUTPUT1, out1)) {
                if (m_tallyLedState) {
                    out1 &= ~0x01; // 拉低 P1_0 点亮
                } else {
                    out1 |= 0x01;  // 拉高 P1_0 熄灭
                }
                i2cWriteByte(fd, AW9523_REG_OUTPUT1, out1);
            }
        }

        unsigned char input0 = 0, input1 = 0;

        //  读取 P0 (模式开关)
        if (i2cReadByte(fd, AW9523_REG_INPUT0, input0)) {
            // P0_0 ~ P0_3: 档位转盘 (SKAR-10S) 二进制输出防抖
            int currentDriveMode = (~input0) & 0x0F;
            
            // 采用帧连续防抖，必须连续多次读到相同数值才认可，解决机械转盘中途断开导致的 0000 (AutoMode) 闪烁问题
            if (currentDriveMode == m_rawDriveModeState) {
                if (m_driveModeStableCount < 5) {
                    m_driveModeStableCount++;
                    if (m_driveModeStableCount == 5) { // 连续5次相同 (~100ms)，确认稳定
                        if (currentDriveMode != lastDriveMode) {
                            qDebug() << "Hardware: Drive Mode definitely switched to" << currentDriveMode;
                            emit driveModeChanged(currentDriveMode);
                            lastDriveMode = currentDriveMode;
                        }
                    }
                }
            } else {
                m_rawDriveModeState = currentDriveMode;
                m_driveModeStableCount = 0;
            }

            // P0_7 确认的极性：调试时观察到 input0=0x80，说明拨到视频位置时 bit7=HIGH
            // 所以 != 0 (HIGH/主动高电平) 代表视频模式，与录制按键极性一致
            bool currentModeIsVideo = (input0 & 0x80) != 0;

            if (currentModeIsVideo != lastModeIsVideo) {
                qDebug() << "Hardware: Mode switched to" << (currentModeIsVideo ? "VIDEO" : "PHOTO");
                emit modeSwitched(currentModeIsVideo);
                lastModeIsVideo = currentModeIsVideo;
                usleep(100000); // 防抖
            }

            // P0_5 相册按键 (主动低电平：默认HIGH，按下LOW)
            bool currentAlbum = (input0 & 0x20) != 0;
            if (lastAlbumState == true && currentAlbum == false) {
                qDebug() << "Hardware: >>> albumButtonPressed EMITTED <<<";
                emit albumButtonPressed();
                usleep(200000); // 防抖 200ms
            }
            lastAlbumState = currentAlbum;
            
            // P0_6 Fn 键（主动低电平）
            bool currentFn = (input0 & 0x40) != 0;
            if (lastFnState == true && currentFn == false) {
                printf("===== [HARDWARE] press fn =====\n");
                fflush(stdout); 
                qDebug() << "press fn";
                emit fnPressed();
                usleep(200000); // 防抖 200ms
            } else if (lastFnState == false && currentFn == true) {
                printf("===== [HARDWARE] release fn =====\n");
                fflush(stdout);
                qDebug() << "release fn";
                emit fnReleased();
                usleep(200000); // 防抖 200ms
            }
            lastFnState = currentFn;

            // P0_4 Del 键（主动低电平）
            bool currentDel = (input0 & 0x10) != 0;
            if (lastDelState == true && currentDel == false) {
                qDebug() << "press del";
                emit deletePressed();
                usleep(200000); // 防抖 200ms
            }
            lastDelState = currentDel;
        }

        // 读取 P1 (按键)
        if (i2cReadByte(fd, AW9523_REG_INPUT1, input1)) {

            // EC11 Rotary Encoder (P1_3=A, P1_2=B, P1_1=Sw)
            bool currentA = (input1 & 0x08) != 0;
            bool currentB = (input1 & 0x04) != 0;
            bool currentSw = (input1 & 0x02) != 0;

            if (m_lastEncoderA != currentA) {
                // Either Falling or Rising edge on A (handles half-step encoders)
                int delta = (currentA != currentB) ? 1 : -1;
                // Switch is usually active-low
                bool isSwPressed = !currentSw;
                emit rotaryEncoderScroll(delta, isSwPressed);
            }
            m_lastEncoderA = currentA;
            m_lastEncoderB = currentB;
            m_lastEncoderSw = currentSw;

            // 快门按键 (P1_5)
            bool currentShutter = (input1 & 0x20) != 0;
            if (lastShutterState == true && currentShutter == false) {
                emit shutterPressed();
                usleep(200000);
            }
            lastShutterState = currentShutter;

            // 录制按键：AW9523 (P1_7) 主动低电平（与快门按键 P1_5 一致）
            // 空闲时 P1_7=HIGH（内部上拉），按下将 P1_7 拥到 GND = LOW = 被按下
            bool aw_rec_pressed = (input1 & 0x80) == 0; // LOW = 被按下（主动低电平）
            
            bool gpio_rec_pressed = false; 
            if (line_fd_25 >= 0) {
                struct gpiohandle_data data;
                if (ioctl(line_fd_25, GPIOHANDLE_GET_LINE_VALUES_IOCTL, &data) == 0) {
                    gpio_rec_pressed = (data.values[0] == 0); // LOW = 被按下
                }
            }

            // input1 发生变化时立刻打印
            static unsigned char lastInput1Debug = 0xFF;
            if (input1 != lastInput1Debug) {
                printf("Hardware: input1 CHANGED 0x%X -> 0x%X | P1_7(rec)=%d | P1_5(shutter)=%d | P1_4(menu)=%d\n",
                       (int)lastInput1Debug, (int)input1,
                       (bool)(input1 & 0x80), (bool)(input1 & 0x20), (bool)(input1 & 0x10));
                fflush(stdout);
                lastInput1Debug = input1;
            }

            bool currentRecState = aw_rec_pressed || gpio_rec_pressed;

            if (lastRecState == false && currentRecState == true) {
                printf("===== [HARDWARE] >>> videoRecordPressed EMITTED <<<\n");
                fflush(stdout);
                emit videoRecordPressed();
                usleep(300000);
            }
            lastRecState = currentRecState;

            // P1_4 Menu 键（主动低电平）
            bool currentMenu = (input1 & 0x10) != 0;
            if (lastMenuState == true && currentMenu == false) {
                qDebug() << "press menu";
                emit menuPressed();
                usleep(200000); // 防抖 200ms
            }
            lastMenuState = currentMenu;
        }

        // Long press logic for Del (P0_4) + Menu (P1_4)
        bool currentDel = (input0 & 0x10) != 0;
        bool currentMenu = (input1 & 0x10) != 0;
        if (!currentDel || !currentMenu) { // Both pressed (active low)
            delMenuHoldCount++;
            if (delMenuHoldCount >= 300 && !exitTriggered) { // ~3 seconds
                qDebug() << "Hardware: Del + Menu held for 3 seconds, requesting exit.";
                emit exitAppRequested();
                exitTriggered = true;
            }
        } else {
            delMenuHoldCount = 0;
            exitTriggered = false;
        }

        // 读取摇杆 GPIO (轮询)
        auto checkJoystickPin = [&](int fd, bool& lastState, const char* name, auto emitSignal) {
            if (fd >= 0) {
                struct gpiohandle_data data;
                if (ioctl(fd, GPIOHANDLE_GET_LINE_VALUES_IOCTL, &data) == 0) {
                    bool pressed = (data.values[0] == 0); // 低电平触发
                    if (lastState == false && pressed == true) {
                        qDebug() << "press" << name;
                        emitSignal();
                        usleep(200000);
                    }
                    lastState = pressed;
                }
            }
        };

        checkJoystickPin(line_fd_16, lastJoyCenter, "joystick center", [&](){ emit joystickCenterPressed(); });
        checkJoystickPin(line_fd_5,  lastJoyLeft,   "joystick left",   [&](){ emit joystickLeftPressed(); });
        checkJoystickPin(line_fd_6,  lastJoyDown,   "joystick down",   [&](){ emit joystickDownPressed(); });
        checkJoystickPin(line_fd_22, lastJoyUp,     "joystick up",     [&](){ emit joystickUpPressed(); });
        checkJoystickPin(line_fd_26, lastJoyRight,  "joystick right",  [&](){ emit joystickRightPressed(); });


        usleep(10000); // 10ms 轮询
    }

    // 释放资源
    if (line_fd_16 >= 0) close(line_fd_16);
    if (line_fd_5 >= 0)  close(line_fd_5);
    if (line_fd_6 >= 0)  close(line_fd_6);
    if (line_fd_22 >= 0) close(line_fd_22);
    if (line_fd_26 >= 0) close(line_fd_26);
    if (line_fd_25 >= 0) close(line_fd_25);
    if (gpio_fd >= 0) close(gpio_fd);
    close(fd);
    qDebug() << "Hardware: Thread stopped.";
}


bool HardwareManager::i2cWriteByte(int fd, unsigned char reg, unsigned char val) {
    unsigned char buf[2] = { reg, val };
    if (write(fd, buf, 2) != 2) {
        // 避免刷屏，不输出 I2C Write Error
        return false;
    }
    return true;
}

bool HardwareManager::i2cReadByte(int fd, unsigned char reg, unsigned char& val) {
    if (write(fd, &reg, 1) != 1) return false;
    if (read(fd, &val, 1) != 1) return false;
    return true;
}