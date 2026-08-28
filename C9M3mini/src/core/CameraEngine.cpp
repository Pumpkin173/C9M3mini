#include <libcamera/libcamera.h>
#include <libcamera/formats.h>
#include <libcamera/control_ids.h>
#include <sys/mman.h>
#include <iostream>
#include <memory>
#include "CameraEngine.h"
#include <QDebug>

using namespace libcamera;

CameraEngine::CameraEngine(QObject* parent) : QObject(parent) {
    streamViewfinder = nullptr;
    streamRaw = nullptr;
    currentEv = 0.0f;
    currentMeteringMode = 0;
}

CameraEngine::~CameraEngine() {
    stop();
}

void CameraEngine::setMeteringMode(int mode) {
    if (mode < 0 || mode > 2) return;
    currentMeteringMode = mode;
}

void CameraEngine::setExposureCompensation(float ev) {
    if (ev > 5.0f) ev = 5.0f;
    if (ev < -5.0f) ev = -5.0f;
    currentEv = ev;
}

void CameraEngine::setDriveMode(int mode) {
    m_driveMode = mode;
}

void CameraEngine::setManualExposure(int shutterUs, int iso) {
    m_manualShutterUs = shutterUs;
    m_manualIso = iso;
}

void CameraEngine::updateLuma(int avgY, float frameIso) {
    m_avgY = avgY;
    m_lastFrameIso = frameIso;
}

void CameraEngine::capturePhoto(int skipFrames) {
    m_skipFrames = skipFrames;
    m_captureRequested = true;
}

void CameraEngine::setFps(float fps) {
    m_targetFps = fps;
}

void CameraEngine::setAwbMode(bool isAuto) {
    m_autoAwb = isAuto;
}

void CameraEngine::setColorTemperature(int tempK) {
    if (tempK < 2500) tempK = 2500;
    if (tempK > 9500) tempK = 9500;
    m_colorTemp = tempK;
}

void CameraEngine::start() {
    cm = std::make_unique<CameraManager>();
    cm->start();

    if (cm->cameras().empty()) {
        qDebug() << "Engine: 未找到摄像头！";
        return;
    }
    camera = cm->cameras()[0];
    camera->acquire();

    auto area = camera->properties().get(properties::PixelArrayActiveAreas);
    if (area && !area->empty()) {
        sensorSize = (*area)[0].size();
        qDebug() << "Sensor Size:" << sensorSize.width << "x" << sensorSize.height;
    } else {
        sensorSize = { 2028, 1520 };
    }

    // 配置三流：Viewfinder + VideoRecording + Raw
    std::unique_ptr<CameraConfiguration> config = camera->generateConfiguration({ StreamRole::Viewfinder, StreamRole::VideoRecording, StreamRole::Raw });
    if (!config || config->empty()) {
        qCritical() << "Failed to generate camera configuration!";
        return;
    }

    // 配置 Viewfinder 流
    StreamConfiguration& vfConfig = config->at(0);
    vfConfig.size = { 640, 480 };
    vfConfig.pixelFormat = libcamera::formats::BGR888;
    
    // 配置 VideoRecording 流
    if (config->size() > 1) {
        StreamConfiguration& videoConfig = config->at(1);
        videoConfig.size = sensorSize;
        videoConfig.pixelFormat = libcamera::formats::YUV420;
    }

    // 配置 RAW 流
    if (config->size() > 2) {
        StreamConfiguration& rawConfig = config->at(2);
        rawConfig.size = sensorSize;
        rawConfig.pixelFormat = libcamera::formats::SBGGR16;
    }

    config->validate();
    camera->configure(config.get());
    streamViewfinder = config->at(0).stream();
    streamVideo = config->at(1).stream();
    streamRaw = config->size() > 2 ? config->at(2).stream() : nullptr;

    // 实际配置打印
    qDebug() << "Engine: VF stream ->" << config->at(0).size.width << "x" << config->at(0).size.height
             << config->at(0).pixelFormat.toString().c_str();
    qDebug() << "Engine: VIDEO stream ->" << config->at(1).size.width << "x" << config->at(1).size.height
             << config->at(1).pixelFormat.toString().c_str()
             << "| stride:" << config->at(1).stride;

    // 广播视频流参数给录像器
    // FFmpeg 需要用这里的 stride（非 width）作为 rawvideo 行宽，否则行对齐填充会造成花屏
    {
        int vw = config->at(1).size.width;
        int vh = config->at(1).size.height;
        int vs = static_cast<int>(config->at(1).stride); 
        emit videoStreamConfigured(vw, vh, vs);
    }

    if (streamRaw) {
        const StreamConfiguration& actualRaw = config->at(2);
        size_t activePixelBytes = (size_t)actualRaw.size.width * actualRaw.size.height * 2;
        qDebug() << "Engine: RAW stream ->" << actualRaw.size.width << "x" << actualRaw.size.height
                 << actualRaw.pixelFormat.toString().c_str()
                 << "| stride:" << actualRaw.stride
                 << "| padding per row:" << (actualRaw.stride - actualRaw.size.width * 2)
                 << "| active pixel bytes:" << activePixelBytes;
    }

    allocator = std::make_unique<FrameBufferAllocator>(camera);
    allocator->allocate(streamViewfinder);
    allocator->allocate(streamVideo);
    if (streamRaw) {
        allocator->allocate(streamRaw);
    }

    // 内存映射并构建 Request
    int reqCount = (int)allocator->buffers(streamViewfinder).size();
    int videoCount = (int)allocator->buffers(streamVideo).size();
    int rawCount = streamRaw ? (int)allocator->buffers(streamRaw).size() : 0;
    qDebug() << "Engine: VF buffers =" << reqCount << ", VIDEO buffers =" << videoCount << ", RAW buffers =" << rawCount;

    // 计算实际可用的最大 Request 数量。
    int totalRequests = reqCount;
    if (streamVideo) totalRequests = std::min(totalRequests, videoCount);
    if (streamRaw)   totalRequests = std::min(totalRequests, rawCount);
    qDebug() << "Engine: Total synchronized requests =" << totalRequests;

    for (int i = 0; i < totalRequests; ++i) {
        std::unique_ptr<Request> request = camera->createRequest();
        if (!request) continue;

        // Mmap Viewfinder
        const auto& vf_buf = allocator->buffers(streamViewfinder)[i];
        if (!vf_buf || vf_buf->planes().empty()) {
            qWarning() << "Engine: VF buffer" << i << "无效，跳过";
            continue;
        }
        const FrameBuffer::Plane& vf_plane = vf_buf->planes()[0];
        void* vf_mem = mmap(NULL, vf_plane.length, PROT_READ, MAP_SHARED, vf_plane.fd.get(), 0);
        if (vf_mem == MAP_FAILED) {
            qWarning() << "Engine: VF buffer" << i << "mmap 失败";
            continue;
        }
        mappedBuffers[vf_buf.get()] = vf_mem;
        request->addBuffer(streamViewfinder, vf_buf.get());


        // Mmap VideoRecording
        if (i < videoCount) {
            const auto& vid_buf = allocator->buffers(streamVideo)[i];
            if (vid_buf && !vid_buf->planes().empty()) {
                size_t totalVidSize = 0;
                for (const auto& p : vid_buf->planes()) totalVidSize += p.length;

                const FrameBuffer::Plane& vid_plane = vid_buf->planes()[0];
                void* vid_mem = mmap(NULL, totalVidSize, PROT_READ, MAP_SHARED, vid_plane.fd.get(), 0);
                if (vid_mem != MAP_FAILED) {
                    mappedBuffers[vid_buf.get()] = vid_mem;
                    mappedSizes[vid_buf.get()]   = totalVidSize;
                    request->addBuffer(streamVideo, vid_buf.get());
                } else {
                    qWarning() << "Engine: VIDEO buffer" << i << "mmap 失败";
                }
            }
        }

        // Mmap RAW
        if (streamRaw && i < rawCount) {
            const auto& raw_buf = allocator->buffers(streamRaw)[i];
            if (raw_buf && !raw_buf->planes().empty()) {
                const FrameBuffer::Plane& raw_plane = raw_buf->planes()[0];
                void* raw_mem = mmap(NULL, raw_plane.length, PROT_READ, MAP_SHARED, raw_plane.fd.get(), 0);
                if (raw_mem != MAP_FAILED) {
                    mappedBuffers[raw_buf.get()] = raw_mem;
                    request->addBuffer(streamRaw, raw_buf.get());
                } else {
                    qWarning() << "Engine: RAW buffer" << i << "mmap 失败，此帧不录制 RAW";
                }
            } else {
                qWarning() << "Engine: RAW buffer" << i << "无效，跳过 RAW";
            }
        }

        requests.push_back(std::move(request));
    }

    // 绑定回调并启动
    camera->requestCompleted.connect(this, &CameraEngine::requestComplete);
    camera->start();

    // 发送初始请求
    for (std::unique_ptr<Request>& req : requests) {
        camera->queueRequest(req.get());
    }
}

void CameraEngine::setMeteringPoint(float x, float y) {
    if (sensorSize.width == 0 || sensorSize.height == 0) return;

    int cx = static_cast<int>(x * sensorSize.width);
    int cy = static_cast<int>(y * sensorSize.height);

    int w = sensorSize.width / 10;
    int h = sensorSize.height / 10;
    int x1 = std::max(0, cx - w / 2);
    int y1 = std::max(0, cy - h / 2);

    libcamera::Rectangle rect(x1, y1, w, h);
    meteringWindows.clear();
    meteringWindows.push_back(rect);
}

template <typename T>
void CameraEngine::trySetControl(ControlList& controls, const std::string& name, const T& value) {
    const ControlId* foundId = nullptr;
    for (const auto& [id, info] : camera->controls()) {
        if (id->name() == name) {
            foundId = id;
            break;
        }
    }
    if (foundId) {
        const auto* typedId = reinterpret_cast<const Control<T>*>(foundId);
        controls.set(*typedId, value);
    }
}

void CameraEngine::stop() {
    if (camera) {
        camera->stop();

        // 在 release 前清理 Request，避免 libcamera 内部报错
        requests.clear();

        // 释放 Mmap 的内存，否则内核将保持 V4L2 设备占用，导致 rpicam-still 无法获取相机
        for (auto const& [buf, mem] : mappedBuffers) {
            size_t size = 0;
            if (mappedSizes.count(buf)) {
                size = mappedSizes[buf];
            } else {
                for (const auto& plane : buf->planes()) {
                    size += plane.length;
                }
            }
            if (mem && mem != MAP_FAILED) {
                munmap(mem, size);
            }
        }
        mappedBuffers.clear();
        mappedSizes.clear();
        allocator.reset();
        camera->release();
        camera.reset();
    }
    if (cm) {
        cm->stop();
        cm.reset();
    }
}

void CameraEngine::requestComplete(Request* request) {
    if (request->status() == Request::RequestCancelled) return;

    // 处理 Viewfinder 图像
    const FrameBuffer* vf_buf = request->findBuffer(streamViewfinder);
    void* vf_data = mappedBuffers[vf_buf];
    const StreamConfiguration& vf_cfg = streamViewfinder->configuration();
    
    // Qt 与 libcamera (DRM) 的 RGB/BGR 命名在端序上通常是反的，这会导致红蓝通道反转
    QImage::Format qfmt = QImage::Format_RGB888;
    if (vf_cfg.pixelFormat == libcamera::formats::RGB888) {
        qfmt = QImage::Format_BGR888;
    } else if (vf_cfg.pixelFormat == libcamera::formats::BGR888) {
        qfmt = QImage::Format_RGB888;
    }
    
    QImage image((uchar*)vf_data, vf_cfg.size.width, vf_cfg.size.height, vf_cfg.stride, qfmt);

    const ControlList& metadata = request->metadata();
    double shutter = (double)metadata.get(controls::ExposureTime).value_or(0);
    double gain = (double)metadata.get(controls::AnalogueGain).value_or(1.0f);

    int64_t vfSensorTs = 0;
    auto vfTsOpt = metadata.get(controls::SensorTimestamp);
    if (vfTsOpt) vfSensorTs = *vfTsOpt;

    // 处理 Viewfinder 的全彩预览图像，提前发射 frameCaptured，
    // 确保 MainWindow 中的 s_currentBurstJpg 总是比 rawFrameCaptured 更早更新
    emit frameCaptured(image, shutter, gain, currentEv.load(), vfSensorTs);

    // 处理 RAW 图像并注入到录像器引擎
    if (streamRaw) {
        const FrameBuffer* raw_buf = request->findBuffer(streamRaw);
        // findBuffer 可能返回 nullptr（当该 Request 没有绑定 RAW 缓冲区时）
        if (raw_buf && mappedBuffers.count(raw_buf) && !raw_buf->planes().empty()) {
            void* raw_data = mappedBuffers[raw_buf];
            size_t raw_size = raw_buf->planes()[0].length;

            // 获取实际 stride，传递给 VideoRecorder 用于行对齐去除
            uint32_t raw_stride = static_cast<uint32_t>(streamRaw->configuration().stride);

            // 打印首帧的 stride 和有效像素尺寸
            static bool sizeLogged = false;
            if (!sizeLogged) {
                size_t activePixelBytes = (size_t)streamRaw->configuration().size.width
                                         * streamRaw->configuration().size.height * 2;
                qDebug() << "Engine: First RAW frame: buffer =" << raw_size
                         << "bytes | stride =" << raw_stride
                         << "| active pixels =" << activePixelBytes
                         << "bytes (DNG template expects 3168256)";
                if (activePixelBytes != 3168256) {
                    qWarning() << "Engine: WARNING! Active pixel area MISMATCH with DNG template!";
                }
                sizeLogged = true;
            }

            uint16_t rGain = 512, bGain = 512;
            auto colorGains = metadata.get(controls::ColourGains);
            if (colorGains) {
                rGain = static_cast<uint16_t>((*colorGains)[0] * 256.0f);
                bGain = static_cast<uint16_t>((*colorGains)[1] * 256.0f);
            }

            // 将 stride 一并传递，让 VideoRecorder 按行解除填充
            // 提取 SensorTimestamp（libcamera CLOCK_BOOTTIME 纳秒），供闪光帧精准匹配
            int64_t sensorTs = 0;
            auto sensorTsOpt = metadata.get(controls::SensorTimestamp);
            if (sensorTsOpt) sensorTs = *sensorTsOpt;

            emit rawFrameCaptured(static_cast<const uint8_t*>(raw_data), raw_size, raw_stride, rGain, bGain, shutter, gain, sensorTs);
        }
    }

    // 处理 VideoRecording 高清视频流 / Proxy 低清视频流
    if (streamVideo) {
        const FrameBuffer* vid_buf = request->findBuffer(streamVideo);
        if (vid_buf && mappedBuffers.count(vid_buf) && !vid_buf->planes().empty()) {
            void* vid_data = mappedBuffers[vid_buf];
            size_t vid_size = mappedSizes.count(vid_buf)
                              ? mappedSizes[vid_buf]
                              : [&]{ size_t s=0; for(const auto& p: vid_buf->planes()) s+=p.length; return s; }();
            emit videoFrameCaptured(static_cast<const uint8_t*>(vid_data), vid_size);
        }
    }

    if (m_captureRequested.load()) {
        if (m_skipFrames.load() > 0) {
            m_skipFrames--;
            qDebug() << "Engine: Skipping frame for ZSL flash sync, remaining skips:" << m_skipFrames.load();
        } else {
            m_captureRequested = false;
            emit photoTaken(image.copy());
            qDebug() << "Engine: Photo captured!";
        }
    }

    request->reuse(Request::ReuseBuffers);
    ControlList& controls = request->controls();

    bool isCapture = m_captureRequested.load();
    int dMode = m_driveMode.load();

    if (dMode == 1 || dMode == 2) { // 1=S, 2=M
        controls.set(controls::AeEnable, false);
        
        int reqShutter = m_manualShutterUs.load();
        int reqIso = m_manualIso.load();

        // 当在 Auto ISO 时，启用 AutoISO 算法
        if (reqIso == 0) {
            float evBias = std::pow(2.0f, currentEv.load());
            float targetY = 118.0f * evBias;
            float currentY = m_avgY.load();
            if (currentY < 1.0f) currentY = 1.0f;

            // 死区 (Deadband) 处理
            float tolerance = 8.0f;
            if (std::abs(currentY - targetY) < tolerance) {
                reqIso = (int)m_dynamicIso.load();
            } else {
                // 超出死区，计算新的增益
                float targetLinear = std::pow(targetY / 255.0f, 2.2f);
                float currentLinear = std::pow(currentY / 255.0f, 2.2f);
                float gainFactor = targetLinear / currentLinear;

                float frameIso = m_lastFrameIso.load();
                if (!isCapture && reqShutter > 100000) {
                    float ratio = (float)reqShutter / 100000.0f;
                    frameIso /= ratio; 
                }
                if (frameIso < 1.0f) frameIso = 1.0f;

                float idealIso = frameIso * gainFactor;
                
                if (idealIso < 100.0f) idealIso = 100.0f;
                if (idealIso > 12800.0f) idealIso = 12800.0f; 

                float smoothedIso = m_dynamicIso.load();
                smoothedIso = 0.9f * smoothedIso + 0.1f * idealIso;
                m_dynamicIso.store(smoothedIso);
                reqIso = (int)smoothedIso;
            }
        }

        // 模拟长曝光亮度：由于预览帧率最低限制为 10fps（即 100000us）
        // 压低虚拟快门到 33333us (30fps)，避免 VBLANK 溢出断言
        if (reqShutter > 33333) {
            float ratio = (float)reqShutter / 33333.0f;
            reqShutter = 33333;
            // 在预览模式下，通过提高模拟增益 (ISO) 来模拟长曝光效果。
            // 实际拍摄时，长曝光由底层 PIO 硬件直接控制 XHS 引脚实现，并使用真实的 ISO 设置。
            if (!isCapture) {
                reqIso = (int)(reqIso * ratio);
            }
        }

        controls.set(controls::ExposureTime, reqShutter);
        controls.set(controls::AnalogueGain, (float)reqIso / 100.0f);

    } else { // Auto Mode 等
        controls.set(controls::AeEnable, true);
        controls.set(controls::ExposureValue, currentEv.load());
    }

    bool autoAwb = m_autoAwb.load();
    controls.set(controls::AwbEnable, autoAwb);
    if (!autoAwb) {
        controls.set(controls::ColourTemperature, m_colorTemp.load());
    }

    // 动态帧率控制
    float targetFps = m_targetFps.load(std::memory_order_relaxed);
    if (targetFps > 0) {
        int64_t frameTimeUs = static_cast<int64_t>(1000000.0f / targetFps);
        int64_t maxLimitUs = frameTimeUs;
        int64_t manualShutter = m_manualShutterUs.load();
        if (manualShutter > maxLimitUs) {
            maxLimitUs = manualShutter + 1000000LL;
        }
        
        int64_t limits[2] = { 100LL, maxLimitUs };
        controls.set(controls::FrameDurationLimits, libcamera::Span<const int64_t, 2>(limits));
    }

    int uiMode = currentMeteringMode.load();
    int32_t hwMode = 2; 

    switch (uiMode) {
    case 0: hwMode = 2; break;
    case 1: hwMode = 0; break;
    case 2: hwMode = 1; break;
    }
    
    controls.set(controls::AeMeteringMode, hwMode);
    camera->queueRequest(request);
}