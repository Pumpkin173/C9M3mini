#ifndef GPIO_MONITOR_H
#define GPIO_MONITOR_H

#include <QThread>
#include <QDebug>
#include <QString>
#include <string>
#include <cstdio>
#include <memory>
#include <array>
#include <atomic>

class GpioMonitor : public QThread {
    Q_OBJECT
public:
    explicit GpioMonitor(int pin, QObject* parent = nullptr) : QThread(parent), m_pin(pin), m_running(true) {}
    ~GpioMonitor() override { m_running = false; wait(); }

protected:
    void run() override {
        QString lastState = "";
        char cmd[64];
        snprintf(cmd, sizeof(cmd), "pinctrl get %d", m_pin);
        
        while (m_running) {
            std::array<char, 128> buffer;
            std::string result;
            std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd, "re"), pclose);
            if (!pipe) {
                msleep(100);
                continue;
            }
            while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
                result += buffer.data();
            }
            
            QString currentState = QString::fromStdString(result).trimmed();
            if (currentState != lastState && !lastState.isEmpty()) {
                qDebug() << "GpioMonitor: GPIO" << m_pin << "changed state to:" << currentState;
            }
            lastState = currentState;
            
            msleep(20); // 50Hz 轮询频率
        }
    }
private:
    int m_pin;
    std::atomic<bool> m_running;
};
#endif
