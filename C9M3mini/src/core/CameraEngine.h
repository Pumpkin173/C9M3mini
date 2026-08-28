#pragma once

#include <QObject>
#include <QImage>
#include <vector>
#include <map>
#include <memory>
#include <atomic>

#pragma push_macro("signals")
#pragma push_macro("slots")
#pragma push_macro("emit")
#undef signals
#undef slots
#undef emit
#include <libcamera/libcamera.h>
#pragma pop_macro("signals")
#pragma pop_macro("slots")
#pragma pop_macro("emit")

class CameraEngine : public QObject {
    Q_OBJECT
public:
    explicit CameraEngine(QObject* parent = nullptr);
    ~CameraEngine() override;

    // UI 控制接口
    void setExposureCompensation(float ev);
    void setMeteringMode(int mode);
    void setMeteringPoint(float x, float y);
    void capturePhoto(int skipFrames = 0);
    void setFps(float fps);
    void setTargetFps(float fps) { m_targetFps.store(fps, std::memory_order_relaxed); }
    
    void setAwbMode(bool isAuto);
    void setColorTemperature(int tempK);

    void setDriveMode(int mode);
    void setManualExposure(int shutterUs, int iso);
    void updateLuma(int avgY, float frameIso);

    void start();
    void stop();

signals:
    // 发送给 UI 的预览流 (QImage)
    void frameCaptured(QImage image, double shutterUs, double gain, float ev, int64_t sensorTimestampNs);
    void photoTaken(QImage image);
    void rawFrameCaptured(const uint8_t* rawData, size_t dataSize, uint32_t stride,
                          uint16_t rGain, uint16_t bGain, double shutterUs, double gain,
                          int64_t sensorTimestampNs);

    void videoFrameCaptured(const uint8_t* yuvData, size_t dataSize);
    void videoStreamConfigured(int width, int height, int stride);

private:
    void requestComplete(libcamera::Request* request);
    
    template <typename T>
    void trySetControl(libcamera::ControlList& controls, const std::string& name, const T& value);

    // libcamera 核心对象
    std::unique_ptr<libcamera::CameraManager> cm;
    std::shared_ptr<libcamera::Camera> camera;
    libcamera::Stream* streamViewfinder;
    libcamera::Stream* streamRaw;
    libcamera::Stream* streamVideo;

    std::unique_ptr<libcamera::FrameBufferAllocator> allocator;
    std::vector<std::unique_ptr<libcamera::Request>> requests;
    std::map<const libcamera::FrameBuffer*, void*>   mappedBuffers;
    std::map<const libcamera::FrameBuffer*, size_t>  mappedSizes;

    libcamera::Size sensorSize;
    std::vector<libcamera::Rectangle> meteringWindows;

    std::atomic<float> currentEv;
    std::atomic<int>   currentMeteringMode;
    std::atomic<int>   m_skipFrames{0};
    std::atomic<bool>  m_captureRequested{false};
    std::atomic<float> m_targetFps{24.0f};
    std::atomic<bool>  m_autoAwb{true};
    std::atomic<int>   m_colorTemp{5500};
    
    std::atomic<int>   m_driveMode{0};
    std::atomic<int>   m_manualShutterUs{16666};
    std::atomic<int>   m_manualIso{0};
    std::atomic<int>   m_avgY{118};
    std::atomic<float> m_lastFrameIso{100.0f};
    std::atomic<float> m_dynamicIso{100.0f};
};