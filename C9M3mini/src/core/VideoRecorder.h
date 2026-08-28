#pragma once

#include <QObject>
#include <QTimer>
#include <QTime>
#include <QString>
#include <QDebug>
#include <QProcess>

#include <atomic>
#include <thread>
#include <cstdint>
#include <cstddef>
#include <string>
#include <cstdio>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <utility>
#include <vector>

// 动态白平衡热补丁结构体
struct WhiteBalance {
    uint16_t r_coeff;
    uint16_t b_coeff;
};

class VideoRecorder : public QObject {
    Q_OBJECT
public:
    enum VideoCodec {
        CODEC_CINEMADNG = 0,
        CODEC_H264,
        CODEC_H265
    };

    // 内存池常量定义
    static constexpr size_t DNG_PREFIX_SIZE = 1024;
    static constexpr size_t RAW_PIXEL_SIZE = 3168256;      // = 1456 * 1088 * 2 bytes
    static constexpr size_t DNG_SUFFIX_SIZE = 0;           // Suffix has been perfectly merged into prefix
    static constexpr size_t ACTUAL_DNG_SIZE = DNG_PREFIX_SIZE + RAW_PIXEL_SIZE + DNG_SUFFIX_SIZE;
    
    
    static constexpr size_t FRAME_SIZE = (ACTUAL_DNG_SIZE + 65535) & ~65535;

    // 代理 YUV 缓冲池
    static constexpr size_t ACTUAL_YUV_MAX_SIZE = (1456 * 1088 * 3 / 2) + sizeof(size_t);
    static constexpr size_t MAX_YUV_SIZE = 2621440; // 2.5 MB (2.5 * 1024 * 1024)


    // IMX296 传感器尺寸（DNG 模板与此一致）
    static constexpr uint32_t SENSOR_WIDTH  = 1456;
    static constexpr uint32_t SENSOR_HEIGHT = 1088;
    static constexpr uint32_t BYTES_PER_PIXEL = 2;         // SBGGR16: 10-bit 存储在 16-bit
    static constexpr uint32_t PIXEL_BYTES_PER_ROW = SENSOR_WIDTH * BYTES_PER_PIXEL; // 2912

    // 自动检测固定大小缓冲池
    explicit VideoRecorder(const std::string& output_dir, QObject* parent = nullptr);
    ~VideoRecorder() override;

    // 禁用拷贝与移动语义，确保内存池及线程状态安全
    VideoRecorder(const VideoRecorder&) = delete;
    VideoRecorder& operator=(const VideoRecorder&) = delete;

    bool isRecording() const { return m_isRecording; }
    VideoCodec currentCodec() const { return m_currentCodec; }
    QString currentCodecName() const;

public slots:
    void startRecording();
    void stopRecording();

    void setCodec(VideoCodec codec);
    void cycleCodec();
    void setFps(float fps);

    // 接收 CameraEngine 广播的实际视频流配置（validate 后可能与请求值不同）
    // stride: Y 平面每行字节数（含对齐填充），用于生成正确的 FFmpeg 命令
    void setVideoStreamInfo(int width, int height, int stride);

    QString getRecordingTimecode() const;
    float getMemoryPoolStatus() const;
    QString getRemainingStorageGB() const;
    static QString getActiveStoragePath();
    bool isFlushing() const { 
        return (!m_isRecording) && (m_ioFlushing.load(std::memory_order_relaxed) || m_proxyFlushing.load(std::memory_order_relaxed)); 
    }

    // 获取内存池大小，供 UI 显示
    float getDngPoolSizeGb() const { return m_poolSizeGigabytes; }
    int getProxyPoolSlots() const { return m_proxyNumSlots; }
    float getProxyPoolSizeMb() const { return (m_proxyNumSlots * MAX_YUV_SIZE) / (1024.0f * 1024.0f); }
    int64_t getLastProxyDropTimeMs() const { return m_lastProxyDropTimeMs.load(std::memory_order_relaxed); }

    // 直接推入 Libcamera 原始 RAW 数据和白平衡参数 (CinemaDNG)
    // stride: libcamera 每行实际字节数（包含对齐填充）
    bool pushFrame(const uint8_t* raw_pixels, size_t data_size, uint32_t stride, const WhiteBalance& wb, double shutterUs = 0, double gain = 1.0);

    // 推送 YUV420 像素给 FFmpeg 用于压缩 H.264/H.265
    bool pushYuvFrame(const uint8_t* yuv_pixels, size_t data_size);

signals:
    void recordingStarted();
    void recordingStopped();
    void timecodeUpdated(QString timeStr);
    void codecChanged(QString codecName);
    void memoryPoolStatusUpdated(float remainingPercentage);

    
    // 录制错误（例如内存盘空间不足）
    void recordingError(QString message);

private:
    // 内存与底层资源分配
    void allocateMemoryPool(); // 固定分配，使用 MAP_POPULATE
    void ioThreadWorker();
    void proxyThreadWorker();
    void copyThreadWorker();
    inline void patchWhiteBalance(uint8_t* suffix_ptr, const WhiteBalance& wb) noexcept;

    // Qt 状态变量
    std::atomic<bool> m_isRecording{false};
    std::atomic<VideoCodec> m_currentCodec{CODEC_CINEMADNG};
    int m_recordedSeconds;

    // 底层录制引擎变量
    std::string m_outputDir;
    std::string m_currentSessionDir;
    float m_poolSizeGigabytes;
    size_t m_numSlots;
    size_t m_poolSize;
    uint8_t* m_memoryPool = nullptr;
    std::thread m_ioThread;
    
    // 独立的 Proxy 缓冲池与线程 (针对 cDNG 代理模式)
    uint8_t* m_proxyPool = nullptr;
    size_t m_proxyPoolSize = 0;
    size_t m_proxyNumSlots = 50; // 50槽 * 4MB = 200MB
    std::thread m_proxyThread;
    alignas(64) std::atomic<size_t> m_proxyWriteIdx{ 0 };
    alignas(64) std::atomic<size_t> m_proxyReadIdx{ 0 };

    std::atomic<FILE*> m_ffmpegPipe{nullptr};
    std::vector<uint8_t> m_proxyCleanBuffer;

    // 无锁环形队列索引 (alignas(64) 避免缓存行伪共享 False Sharing)
    alignas(64) std::atomic<size_t> m_writeIdx{ 0 }; // 生产者 (CameraEngine) 写入位置
    alignas(64) std::atomic<size_t> m_readIdx{ 0 };  // 消费者 (I/O Thread) 读取位置
    alignas(64) std::atomic<bool> m_isEngineRunning{ false };
    alignas(64) std::atomic<bool> m_forceAbort{ false };
    uint32_t m_frameCounter{ 0 };
    std::atomic<uint32_t> m_captureCounter{ 0 };
    std::atomic<float> m_targetFps{ 24.0f };
    std::atomic<bool> m_ioFlushing{ false };
    std::atomic<bool> m_proxyFlushing{ false };
    float m_lastReportedPercentage{ 1.0f };
    alignas(64) std::atomic<int64_t> m_lastProxyDropTimeMs{ 0 };
    
    // 缓存已打补丁的 DNG 文件头
    uint8_t m_dngHeader[DNG_PREFIX_SIZE];

    // 代理视频跨文件系统移动的后台队列
    std::mutex m_copyMutex;
    std::condition_variable m_copyCv;
    std::queue<std::pair<QString, QString>> m_copyQueue;
    std::thread m_copyThread;
    std::atomic<bool> m_stopCopyThread{false};

    // 实际的 YUV 视频流参数（由 CameraEngine::videoStreamConfigured 信号填入）
    int m_videoWidth{ 0 };    // 有效像素列数
    int m_videoHeight{ 0 };   // 有效像素行数
    int m_videoStride{ 0 };   // Y 平面每行字节数（可能大于 m_videoWidth，含行填充）
};