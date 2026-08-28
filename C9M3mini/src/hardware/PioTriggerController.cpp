#include "PioTriggerController.h"
#include <QDebug>
#include <QCoreApplication>
#include <chrono>
#include <thread>
#include <ctime>

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <cerrno>
#include <cstring>

#include "piolib.h"
#include "imx296_trigger.pio.h"

#define PIN_XTR   17    // XTR Trigger
#define PIN_FLASH 23    // Flash

// 构造 / 析构
PioTriggerController::PioTriggerController(QObject* parent)
    : QThread(parent)
    , m_pio(nullptr)
    , m_sm_trigger(-1)
    , m_offset_trigger(0)
    , m_running(false)
    , m_previewFps(30)
    , m_previewExposureUs(5000)
{}

PioTriggerController::~PioTriggerController() {
    stopPreview();
    wait();

    if (m_pio) {
        if (m_sm_trigger >= 0) { 
            pio_sm_set_enabled(m_pio, m_sm_trigger, false); 
            pio_sm_unclaim(m_pio, m_sm_trigger); 
        }
        pio_close(m_pio);
    }
}

bool PioTriggerController::init() {
    qInfo() << "PioTrigger: Initializing for Raspberry Pi 5...";
    m_pio = pio_open(0);
    
    if (!m_pio || PIO_IS_ERR(m_pio)) {
        qCritical() << "PioTrigger: Failed to open RP1 PIO 0!";
        return false;
    }

    // 关闭 SM0
    pio_set_sm_mask_enabled(m_pio, 0x1, false);

    // 认领 SM0
    pio_sm_claim(m_pio, 0);
    m_sm_trigger = 0;
    
    // 清空指令内存
    errno = 0;
    pio_clear_instruction_memory(m_pio);
    qDebug() << "PioTrigger: Clear instruction memory completed. True errno:" << errno << strerror(errno);
    
    usleep(50000); // 等待 50ms 确保硬件清理完毕，防止总线繁忙导致超时

    errno = 0;
    int offset = (int)pio_add_program(m_pio, &camera_trigger_program);
    if (offset < 0) {
        qWarning() << "PioTrigger: camera_trigger add failed! errno:" << errno << strerror(errno);
        return false;
    } else {
        m_offset_trigger = offset;
        qDebug() << "PioTrigger: camera_trigger dynamically loaded at offset=" << m_offset_trigger;
    }

    // 配置 Trigger SM (统一控制 XTR 和 FLASH)
    pio_sm_config c = camera_trigger_program_get_default_config(m_offset_trigger);
    
    // XTR 控制: 使用 set pins (GPIO 17)
    sm_config_set_set_pins(&c, PIN_XTR, 1);
    // FLASH 控制: 使用 sideset pins (GPIO 23)
    sm_config_set_sideset_pins(&c, PIN_FLASH);
    
    sm_config_set_clkdiv(&c, 200.0f); // RP1 sys_clk 200MHz / 200.0 = 1MHz -> 1 cycle = 1us

    // 初始化 GPIO 引脚
    pio_gpio_init(m_pio, PIN_XTR);
    pio_gpio_init(m_pio, PIN_FLASH);
    
    // 设置引脚的初始状态 (XTR=HIGH, FLASH=LOW)
    pio_sm_set_pins_with_mask(m_pio, m_sm_trigger, 1u << PIN_XTR, (1u << PIN_XTR) | (1u << PIN_FLASH));
    
    // 配置引脚方向为输出
    pio_sm_set_consecutive_pindirs(m_pio, m_sm_trigger, PIN_XTR, 1, true);
    pio_sm_set_consecutive_pindirs(m_pio, m_sm_trigger, PIN_FLASH, 1, true);
    
    // 初始化状态机
    pio_sm_init(m_pio, m_sm_trigger, m_offset_trigger, &c);

    // 启动状态机
    pio_sm_set_enabled(m_pio, m_sm_trigger, true);

    qDebug() << "PioTrigger: Unified Trigger SM running.";
    return true;
}

void PioTriggerController::startPreview(int fps, int exposureUs) {
    m_previewFps        = fps;
    m_previewExposureUs = exposureUs;
    if (!m_running) {
        m_running = true;
        start();
    }
}

void PioTriggerController::stopPreview() {
    m_running = false;
}

void PioTriggerController::run() {
    qDebug() << "PioTrigger: Preview thread started at" << m_previewFps << "fps.";

    while (m_running) {
        std::lock_guard<std::mutex> lock(m_fifoMutex);
        
        // 动态计算当前需要的硬件帧周期 (Frame Interval)
        uint32_t frameIntervalUs = 1000000 / m_previewFps.load();
        
        if (m_captureRequested.load()) {
            // 无缝插入闪光拍摄帧
            if (m_preFireCallback) {
                m_preFireCallback();
            }
            
            uint32_t exposureUs = m_captureExposureUs.load();
            uint32_t offsetUs = m_captureFlashDelayUs.load();
            
            // 强制脉宽 >= 15us，避免同步错位
            uint32_t pioExposureUs = (exposureUs > 29) ? (exposureUs - 14) : 15;
            
            // 保证留白时间 > 0，维持周期稳定。拍摄模式下总耗时 = offsetUs + pioExposureUs + blankingUs
            uint32_t totalActive = pioExposureUs + offsetUs;
            uint32_t blankingUs = (frameIntervalUs > totalActive) ? (frameIntervalUs - totalActive) : 100;

            qDebug() << "PioTrigger: Seamless Capture Pulse! Target Exp:" << exposureUs 
                     << "us | PIO Pulse:" << pioExposureUs 
                     << "us | Flash pre-delay:" << offsetUs << "us | Blanking:" << blankingUs << "us";

            // 非阻塞入队，避免触发底层 PIO 驱动 put 操作超时
            auto safe_pio_put = [&](uint32_t data) {
                while (pio_sm_is_tx_fifo_full(m_pio, m_sm_trigger)) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
                pio_sm_put(m_pio, m_sm_trigger, data);
            };

            // 推送参数: [mode=1] [offsetUs-1] [pioExposureUs-1] [blankingUs-1]
            safe_pio_put(1u);
            safe_pio_put(std::max(1u, offsetUs) - 1);
            safe_pio_put(std::max(1u, pioExposureUs) - 1);
            safe_pio_put(std::max(1u, blankingUs) - 1);

            // 等待物理曝光完成
            // 轮询等待曝光完成，避免内核驱动超时
            while (pio_sm_is_rx_fifo_empty(m_pio, m_sm_trigger)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            pio_sm_get(m_pio, m_sm_trigger);
            
            // 记录曝光结束时间戳
            struct timespec _ts;
            clock_gettime(CLOCK_BOOTTIME, &_ts);
            m_lastFireTimestampNs.store(
                (int64_t)_ts.tv_sec * 1000000000LL + (int64_t)_ts.tv_nsec,
                std::memory_order_release);
            
            // 硬件确认曝光完成，通知 MainWindow 可以停止看门狗
            emit exposureCompleted();
            
            m_captureRequested = false;
        } else {
            // 预览模式
            uint32_t exposureUs = static_cast<uint32_t>(m_previewExposureUs.load());
            
            // 核心公式补偿：Pulse Width = Target Exposure - 14.26us (取整 14us)，保底 15us 以免丢帧
            uint32_t pioExposureUs = (exposureUs > 29) ? (exposureUs - 14) : 15;
            
            // 设定最小 100us Blanking，确保全局快门数据完整读出
            if (pioExposureUs + 100 >= frameIntervalUs) {
                pioExposureUs = frameIntervalUs - 100;
            }
            uint32_t blankingUs = frameIntervalUs - pioExposureUs;

            // 非阻塞入队，避免触发底层 PIO 驱动 put 操作超时
            auto safe_pio_put = [&](uint32_t data) {
                while (pio_sm_is_tx_fifo_full(m_pio, m_sm_trigger)) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
                pio_sm_put(m_pio, m_sm_trigger, data);
            };

            // 推送参数: [mode=0] [pioExposureUs-1] [blankingUs-1]
            safe_pio_put(0u);
            safe_pio_put(std::max(1u, pioExposureUs) - 1);
            safe_pio_put(std::max(1u, blankingUs) - 1);
            
            // 预览模式无 push block，禁止调用 get_blocking
        }
    }

    qDebug() << "PioTrigger: Preview thread stopped.";
}

void PioTriggerController::triggerCapturePulse(int exposureUs, int flashPreDelayUs, std::function<void()> preFireCallback) {
    // 异步无缝插入请求，不需要暂停预览线程！
    m_captureExposureUs = exposureUs;
    m_captureFlashDelayUs = flashPreDelayUs;
    m_preFireCallback = preFireCallback;
    m_lastFireTimestampNs.store(0, std::memory_order_release); // 重置时间戳，防止匹配到上一次的历史记录
    m_captureRequested = true;
    
    // 异步插入闪光拍摄指令，保持底层传感器数据流及内部时钟连续稳定。
}
