#pragma once

#include <QObject>
#include <QThread>
#include <atomic>

class HardwareManager : public QThread {
    Q_OBJECT

public:
    explicit HardwareManager(QObject* parent = nullptr);
    ~HardwareManager();

    void stop();

public slots:
    void setTallyLed(bool turnOn);

signals:
    void shutterPressed();
    void videoRecordPressed();      // 视频录制按键触发
    void modeSwitched(bool isVideo); // 模式切换开关触发
    void albumButtonPressed();       // 相册按键触发
    void fnPressed();                // Fn 实体键触发（AW9523 P0_6，用于退出播放等）
    void fnReleased();               // Fn 实体键松开触发
    void menuPressed();              // Menu 实体键触发（AW9523 P1_4）
    void deletePressed();            // Del 实体键触发（AW9523 P0_4）
    void joystickCenterPressed();    // 摇杆居中按下触发（GPIO 16）
    void joystickLeftPressed();      // 摇杆向左按下触发（GPIO 5）
    void joystickRightPressed();     // 摇杆向右按下触发（GPIO 26）
    void joystickUpPressed();        // 摇杆向上按下触发（GPIO 22）
    void joystickDownPressed();      // 摇杆向下按下触发（GPIO 6）
    void exitAppRequested();         // 长按 Del + Menu 3秒退出程序
    void driveModeChanged(int modeIndex); // 档位转盘变化触发
    void rotaryEncoderScroll(int delta, bool switchPressed); // EC11 旋转触发

protected:
    void run() override;

private:
    bool stopFlag;

    std::atomic<bool> m_tallyLedState{false};
    std::atomic<bool> m_tallyLedUpdatePending{false};

    // EC11 State Tracking
    bool m_lastEncoderA = true;
    bool m_lastEncoderB = true;
    bool m_lastEncoderSw = true;

    // I2C 辅助函数：增加 bool 返回值表示成功/失败
    bool i2cWriteByte(int fd, unsigned char reg, unsigned char val);
    // 读函数：通过引用传出数据，返回值表示成功/失败
    bool i2cReadByte(int fd, unsigned char reg, unsigned char& val);
};
