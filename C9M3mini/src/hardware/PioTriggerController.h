#ifndef PIO_TRIGGER_CONTROLLER_H
#define PIO_TRIGGER_CONTROLLER_H

#include <QObject>
#include <QThread>
#include <atomic>
#include <mutex>
#include <functional>
#include <cstdint>

struct pio_instance;

class PioTriggerController : public QThread {
    Q_OBJECT
public:
    explicit PioTriggerController(QObject* parent = nullptr);
    ~PioTriggerController() override;

    bool init();

    void startPreview(int fps, int exposureUs);
    void stopPreview();

    // 触发带闪光的异步拍摄
    // @param exposureUs       XTR 曝光时长 (us)
    // @param flashPreDelayUs  闪光灯预热时间 (us)
    void triggerCapturePulse(int exposureUs,
                             int flashPreDelayUs = 500,
                             std::function<void()> preFireCallback = nullptr);

signals:
    // 硬件曝光完成信号
    void exposureCompleted();

protected:
    void run() override;

public:
    // 获取最近一次曝光的时间戳 (纳秒)
    int64_t lastFireTimestampNs() const {
        return m_lastFireTimestampNs.load(std::memory_order_acquire);
    }

private:


    pio_instance* m_pio;
    int           m_sm_trigger;
    uint          m_offset_trigger;

    std::atomic<bool>    m_running;
    std::atomic<int>     m_previewFps;
    std::atomic<int>     m_previewExposureUs;

    // 曝光时间戳
    std::atomic<int64_t> m_lastFireTimestampNs{0};

    // 异步拍摄状态
    std::atomic<bool> m_captureRequested{false};
    std::atomic<int>  m_captureExposureUs{0};
    std::atomic<int>  m_captureFlashDelayUs{0};
    std::function<void()> m_preFireCallback;

    std::mutex m_fifoMutex;
};

#endif // PIO_TRIGGER_CONTROLLER_H
