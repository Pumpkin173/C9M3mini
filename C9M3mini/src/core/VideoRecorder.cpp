#include "VideoRecorder.h"
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <cstdio>
#include <iostream>
#include <QDir>
#include "DngTemplate.h"
#include <chrono>
#include <csignal>

VideoRecorder::VideoRecorder(const std::string& output_dir, QObject* parent)
    : QObject(parent),
    m_isRecording(false),
    m_currentCodec(CODEC_CINEMADNG),
    m_recordedSeconds(0),
    m_poolSizeGigabytes(0),
    m_outputDir(output_dir)
{
    // 忽略 SIGPIPE 信号，防止在使用 pipe 传输数据时（例如向 ffmpeg 推流），因对端进程异常退出导致主程序崩溃
    signal(SIGPIPE, SIG_IGN);

    // 读取系统物理总内存
    long pages = sysconf(_SC_PHYS_PAGES);
    long page_size = sysconf(_SC_PAGE_SIZE);
    long totalRamMB = (pages * page_size) / (1024 * 1024);

    if (totalRamMB >= 7000) {
        m_poolSizeGigabytes = 6.0f;
        m_proxyNumSlots = 250; 
    } else if (totalRamMB >= 3500) {
        m_poolSizeGigabytes = 2.5f;
        m_proxyNumSlots = 180; 
    } else if (totalRamMB >= 1500) {
        m_poolSizeGigabytes = 1.0f;
        m_proxyNumSlots = 120; // 2秒缓冲，足以应对冷启动，且绝对安全不 OOM
    } else {
        m_poolSizeGigabytes = 0.3f;
        m_proxyNumSlots = 60;
    }

    qDebug() << "VideoRecorder: Detected RAM =" << totalRamMB << "MB. Auto-assigned pools - RAW:" 
             << m_poolSizeGigabytes << "GB, Proxy:" << m_proxyNumSlots << "slots";

    // 静态一次性分配，强制使用 MAP_POPULATE
    allocateMemoryPool();
    
    // 初始化并缓存已打补丁的 DNG 模板 (去除了之前错误的 extern 声明，确保使用正确的内部链接模板)
    std::memcpy(m_dngHeader, DNG_PREFIX, DNG_PREFIX_SIZE);
    uint32_t magic = 0xDEADBEEF;
    for (int j = 0; j < DNG_PREFIX_SIZE - 3; ++j) {
        if (std::memcmp(m_dngHeader + j, &magic, 4) == 0) {
            uint32_t zero = 0;
            std::memcpy(m_dngHeader + j, &zero, 4);
            break;
        }
    }
    
    // 启动后台跨文件系统搬运线程
    m_copyThread = std::thread(&VideoRecorder::copyThreadWorker, this);

    //  cDNG 代理冷启动失败问题
    std::thread([]() {
        FILE* p = popen("ffmpeg -y -thread_queue_size 8 -analyzeduration 0 -probesize 32 "
                        "-f rawvideo -pix_fmt yuv420p -s 1456x1088 -framerate 24 -i - "
                        "-vf scale=iw/2:ih/2:flags=fast_bilinear -c:v libx264 -preset ultrafast -timecode 00:00:00:00 "
                        "/dev/shm/warmup.mp4 >/dev/null 2>&1", "we");
        if (p) {
            size_t frameSize = 1456 * 1088 * 3 / 2;
            std::vector<uint8_t> blankFrame(frameSize, 0); // 默认填充0 (黑色，U/V 分量可能是偏绿，但不重要)
            fwrite(blankFrame.data(), 1, frameSize, p);
            pclose(p);
            std::remove("/dev/shm/warmup.mp4");
        }
    }).detach();
}

VideoRecorder::~VideoRecorder() {
    if (m_isRecording || isFlushing()) {
        m_isRecording = false;
        m_isEngineRunning.store(false, std::memory_order_release);
        m_writeIdx.notify_one();
        m_proxyWriteIdx.notify_one();
    }
    if (m_ioThread.joinable()) {
        m_ioThread.join();
    }
    if (m_proxyThread.joinable()) {
        m_proxyThread.join();
    }
    
    // 停止并等待后台拷贝线程
    m_stopCopyThread.store(true);
    m_copyCv.notify_all();
    if (m_copyThread.joinable()) {
        m_copyThread.join();
    }
    
    if (m_memoryPool != MAP_FAILED && m_memoryPool != nullptr) {
        munmap(m_memoryPool, m_poolSize);
    }
    if (m_proxyPool != MAP_FAILED && m_proxyPool != nullptr) {
        munmap(m_proxyPool, m_proxyPoolSize);
    }
}

void VideoRecorder::allocateMemoryPool() {
    float sizeToMap = m_poolSizeGigabytes;
    size_t targetBytes = static_cast<size_t>(sizeToMap * 1024 * 1024 * 1024);
    m_numSlots = targetBytes / FRAME_SIZE;
    m_poolSize = m_numSlots * FRAME_SIZE;

    // 强制使用 MAP_POPULATE 提前分配物理内存页，避免在录制过程中触发缺页中断 (Page Fault) 导致底层数据流超时异常。
    int flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE;

    qDebug() << "VideoRecorder: Allocating fixed" << sizeToMap << "GB pool ("
             << m_numSlots << "slots)...";

    m_memoryPool = static_cast<uint8_t*>(mmap(
        nullptr, m_poolSize,
        PROT_READ | PROT_WRITE,
        flags,
        -1, 0
    ));

    if (m_memoryPool == MAP_FAILED) {
        m_memoryPool = nullptr;
        qFatal("FATAL: Failed to mmap memory pool!");
    }

    // Advise the kernel to utilize Transparent Huge Pages (THP) to reduce TLB miss rates.
    madvise(m_memoryPool, m_poolSize, MADV_HUGEPAGE);
    // Mark as MADV_DONTFORK to prevent expensive Copy-On-Write operations during fork/popen calls.
    madvise(m_memoryPool, m_poolSize, MADV_DONTFORK);
    qDebug() << "VideoRecorder: Memory Pool ready.";

    // Proxy buffer pool allocation (static allocation to persist across recorder cycles).
    if (m_proxyPool == nullptr) {
        m_proxyPoolSize = m_proxyNumSlots * MAX_YUV_SIZE;
        qDebug() << "VideoRecorder: Allocating" << (m_proxyPoolSize / 1024 / 1024) << "MB Proxy Pool...";
        m_proxyPool = static_cast<uint8_t*>(mmap(
            nullptr, m_proxyPoolSize, PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0));
        if (m_proxyPool == MAP_FAILED) {
            qWarning() << "VideoRecorder: Failed to mmap proxy pool! Memory constrained?";
            m_proxyPool = nullptr;
        } else {
            madvise(m_proxyPool, m_proxyPoolSize, MADV_HUGEPAGE);
            madvise(m_proxyPool, m_proxyPoolSize, MADV_DONTFORK);
        }
    }
}

QString VideoRecorder::currentCodecName() const {
    switch (m_currentCodec) {
    case CODEC_CINEMADNG: return "cDNG";
    case CODEC_H264:      return "H.264";
    case CODEC_H265:      return "H.265";
    default:              return "Unknown";
    }
}

void VideoRecorder::setCodec(VideoCodec codec) {
    if (m_isRecording || isFlushing()) {
        qWarning() << "Cannot change codec while recording or flushing!";
        return;
    }
    m_currentCodec = codec;
    emit codecChanged(currentCodecName());
}

void VideoRecorder::cycleCodec() {
    int next = (static_cast<int>(m_currentCodec) + 1) % 3;
    setCodec(static_cast<VideoCodec>(next));
}

void VideoRecorder::setFps(float fps) {
    m_targetFps = fps;
}

void VideoRecorder::setVideoStreamInfo(int width, int height, int stride) {
    if (m_isRecording) {
        qWarning() << "VideoRecorder: Cannot update stream info while recording!";
        return;
    }
    m_videoWidth  = width;
    m_videoHeight = height;
    m_videoStride = stride;
    qDebug() << "VideoRecorder: Video stream info set:"
             << width << "x" << height
             << "| Y-stride:" << stride
             << "| padding per row:" << (stride - width);
}

QString VideoRecorder::getRecordingTimecode() const {
    // 真正的专业电影机 Free Run (Time of Day) 时间码
    float fps = m_targetFps.load(std::memory_order_relaxed);
    if (fps < 1.0f) fps = 24.0f;
    int fps_int = qRound(fps);

    QTime time = QTime::currentTime();
    int frames = (time.msec() * fps_int) / 1000;
    
    return time.toString("HH:mm:ss") + QString(":%1").arg(frames, 2, 10, QChar('0'));
}

float VideoRecorder::getMemoryPoolStatus() const {
    if (m_numSlots == 0) return 1.0f;
    size_t current_write = m_writeIdx.load(std::memory_order_relaxed);
    size_t current_read = m_readIdx.load(std::memory_order_acquire);
    size_t usedSlots = (current_write + m_numSlots - current_read) % m_numSlots;
    return 1.0f - (float)usedSlots / (float)m_numSlots;
}

#include <sys/statvfs.h>
QString VideoRecorder::getActiveStoragePath() {
    QDir mediaDir("/media/pumpkin");
    QStringList entries = mediaDir.entryList(QStringList() << "FILM*", QDir::Dirs | QDir::NoDotAndDotDot, QDir::Time);
    if (!entries.isEmpty()) {
        return "/media/pumpkin/" + entries.first();
    }
    return "/media/pumpkin/FILM";
}

QString VideoRecorder::getRemainingStorageGB() const {
    struct statvfs stat;
    // 直接检查根挂载点，因为刚格式化的卡里可能还没有 Video 文件夹
    QString baseDir = getActiveStoragePath();
    if (statvfs(baseDir.toStdString().c_str(), &stat) != 0) {
        return "N/A";
    }
    double free_bytes = (double)stat.f_bavail * stat.f_frsize;
    int free_gb = qRound(free_bytes / (1024.0 * 1024.0 * 1024.0));
    if (free_gb > 999) {
        return QString("%1T").arg(free_gb / 1000.0, 0, 'f', 1);
    }
    return QString("%1G").arg(free_gb);
}

void VideoRecorder::startRecording() {
    if (m_isRecording) return;
    if (isFlushing()) {
        qDebug() << "VideoRecorder: Cannot start recording yet. Threads still flushing.";
        return;
    }

    if (m_ioThread.joinable()) {
        m_ioThread.join(); // 清理已经被彻底标志完成的游离线程句柄
    }
    if (m_proxyThread.joinable()) {
        m_proxyThread.join();
    }

    qDebug() << "VideoRecorder: Starting record with codec:" << currentCodecName();

    // 在启动后台线程前重置读写指针，防止新旧数据冲突。
    m_writeIdx.store(0, std::memory_order_relaxed);
    m_readIdx.store(0, std::memory_order_relaxed);

    // 自动创建带有时间戳的独立输出目录 (遵循 CinemaDNG 规范要求按序列分文件夹)
    QString dynamicVideoDir = getActiveStoragePath() + "/Video";
    QString sessionName = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
    m_currentSessionDir = dynamicVideoDir.toStdString() + "/" + sessionName.toStdString();
    QDir dir(QString::fromStdString(m_currentSessionDir));
    
    if (!dir.exists()) {
        if (dir.mkpath(".")) {
            qDebug() << "VideoRecorder: Created output directory:" << dir.absolutePath();
        } else {
            qCritical() << "VideoRecorder: FATAL - Cannot create output dir:" << dir.absolutePath();
            return;
        }
    }

    if (m_currentCodec == CODEC_CINEMADNG) {
        // 检查 /dev/shm 的剩余空间，如果过小则拒绝启动，防止 OOM 
        struct statvfs shmStat;
        if (statvfs("/dev/shm", &shmStat) == 0) {
            double free_mb = (double)shmStat.f_bavail * shmStat.f_frsize / (1024.0 * 1024.0);
            if (free_mb < 50.0) {
                QString errorMsg = QString("内存盘剩余空间不足 (%1 MB)，请等待后台拷贝完成。").arg(free_mb, 0, 'f', 1);
                qCritical() << "VideoRecorder:" << errorMsg;
                emit recordingError(errorMsg);
                return;
            }
        }
    }

    // 获取实际尺寸，用于推流 FFmpeg
    int w      = (m_videoWidth  > 0) ? m_videoWidth  : 1456;
    int h      = (m_videoHeight > 0) ? m_videoHeight : 1088;
    int stride = (m_videoStride > 0) ? m_videoStride : w;
    
    // 使用实际宽度 w 推流 FFmpeg
    QString sizeArg = QString("%1x%2").arg(w).arg(h);
    QString fps_str = QString::number(m_targetFps.load(std::memory_order_relaxed), 'f', 6);
    
    QString startTimecode = getRecordingTimecode();
    QString timecodeArg = "";
    if (m_targetFps.load(std::memory_order_relaxed) <= 30.0f) {
        timecodeArg = QString("-timecode %1").arg(startTimecode);
    }

    if (m_currentCodec == CODEC_CINEMADNG) {
        
        QString proxyFileName = QString("/dev/shm/video_%1_proxy.mp4").arg(sessionName);
        QString cmd = QString("ffmpeg -y -thread_queue_size 8 -analyzeduration 0 -probesize 32 "
                              "-f rawvideo -pix_fmt yuv420p -s %1 -framerate %2 -i - "
                              "-vf scale=iw/2:ih/2:flags=fast_bilinear -c:v libx264 -preset ultrafast %3 \"%4\" >/dev/null 2>&1")
                          .arg(sizeArg, fps_str, timecodeArg, proxyFileName);

        qDebug() << "VideoRecorder: Starting background Proxy FFmpeg on RAM Disk:" << cmd;
        m_ffmpegPipe = popen(cmd.toStdString().c_str(), "we");
        if (!m_ffmpegPipe) {
             qWarning() << "VideoRecorder: Proxy failed to start! Continuing with DNG-only...";
        }
        m_forceAbort.store(false, std::memory_order_release);
        m_isEngineRunning.store(true, std::memory_order_release);
        m_ioFlushing.store(true, std::memory_order_release);
        m_proxyFlushing.store(true, std::memory_order_release);
        m_ioThread = std::thread(&VideoRecorder::ioThreadWorker, this);

        m_proxyWriteIdx.store(0, std::memory_order_relaxed);
        m_proxyReadIdx.store(0, std::memory_order_relaxed);
        m_proxyThread = std::thread(&VideoRecorder::proxyThreadWorker, this);
    } else {
        // 普通 H.264 / H.265 录制
        QString ext = (m_currentCodec == CODEC_H265) ? ".h265.mp4" : ".h264.mp4";
        QString outputFileName = QString::fromStdString(m_currentSessionDir) + "/video_" + sessionName + ext;
        QString codecName = (m_currentCodec == CODEC_H264) ? "libx264" : "libx265";
        
        QString cmd = QString("ffmpeg -y -thread_queue_size 8 -analyzeduration 0 -probesize 32 "
                              "-f rawvideo -pix_fmt yuv420p -s %1 -framerate %2 -i - ")
                          .arg(sizeArg, fps_str);
        cmd += QString("-c:v %1 -preset ultrafast %2 \"%3\" >/dev/null 2>&1")
                   .arg(codecName, timecodeArg, outputFileName);

        qDebug() << "VideoRecorder: Starting ffmpeg with popen:" << cmd;
        m_ffmpegPipe = popen(cmd.toStdString().c_str(), "we");
        if (!m_ffmpegPipe) {
            qCritical() << "VideoRecorder: Failed to popen ffmpeg process for H.264/H.265!";
            m_isRecording = false;
            return;
        }

        m_isEngineRunning.store(true, std::memory_order_release);
        m_ioFlushing.store(true, std::memory_order_release);
        m_proxyFlushing.store(false, std::memory_order_release);
        m_ioThread = std::thread(&VideoRecorder::ioThreadWorker, this);
    }

    // 更新 Qt UI 状态
    m_isRecording = true;
    m_recordedSeconds = 0;
    m_frameCounter = 0;
    m_captureCounter = 0;
    m_lastReportedPercentage = 1.0f;
    emit timecodeUpdated(startTimecode);
    emit memoryPoolStatusUpdated(1.0f);
    // 底层帧回调推入时同步更新。
    emit recordingStarted();
    qDebug() << "VideoRecorder: Recording started. Output dir:" << dir.absolutePath();
}

void VideoRecorder::stopRecording() {
    if (!m_isRecording) return;

    qDebug() << "VideoRecorder: Stopping record. Flushing buffers asynchronously...";
    m_isRecording = false;

    // 对于所有编码格式，停止录制引擎并唤醒可能在沉睡的 I/O 线程
    // 异步管道关闭都在 ioThreadWorker 自然结束后进行，避免阻塞主线程
    m_isEngineRunning.store(false, std::memory_order_release);
    m_writeIdx.notify_one();
    m_proxyWriteIdx.notify_one();
    qDebug() << "VideoRecorder:" << currentCodecName() << "Engine stopped. IO Thread will handle flushing.";

    // 若发生异常中止（如缓存池已满），直接置位 m_forceAbort 以中断 I/O 队列，确保后台线程安全退出
    // 避免在此处调用 system 等派生进程的操作，以防止瞬间的 CPU 资源抢占干扰底层相机硬件的数据流。
    if (m_forceAbort.load(std::memory_order_acquire)) {
        qCritical() << "VideoRecorder: Force abort detected! Pipelines will exit immediately.";
    }

    emit recordingStopped();
}

inline void VideoRecorder::patchWhiteBalance(uint8_t* prefix_ptr, const WhiteBalance& wb) noexcept {
    uint8_t* as_shot_ptr = prefix_ptr + DNG_AS_SHOT_NEUTRAL_SUFFIX_OFFSET;
    // AsShotNeutral 是 3 个 Rational（每个 Rational = uint32_t 分子 + uint32_t 分母 = 8 字节）
    // 总体 24 字节。结构： {R_num, R_den}, {G_num, G_den}, {B_num, B_den}
    // r_coeff 和 b_coeff 是基准为 256 的整型增益 (gain = coeff / 256.0)
    // 根据 DNG 规范，AsShotNeutral 应当是中性色的线性场景 RGB 值，即白平衡增益的倒数 (1 / gain)
    // 所以 R 的 AsShotNeutral = 256 / r_coeff, B 的 AsShotNeutral = 256 / b_coeff
    
    uint32_t r_num = 256;
    // 白平衡数据防零保护：防止底层框架在特定情况下返回无效的白平衡增益（如 0）。
    // 写入分母为 0 的 Rational 数据会破坏 DNG/TIFF 文件结构，导致后期处理软件（如 DaVinci Resolve）解析失败。
    uint32_t r_den = (wb.r_coeff == 0) ? 256 : wb.r_coeff;
    
    uint32_t g_num = 1;
    uint32_t g_den = 1;
    
    uint32_t b_num = 256;
    uint32_t b_den = (wb.b_coeff == 0) ? 256 : wb.b_coeff;
    
    std::memcpy(as_shot_ptr,      &r_num, 4);
    std::memcpy(as_shot_ptr + 4,  &r_den, 4);
    std::memcpy(as_shot_ptr + 8,  &g_num, 4);
    std::memcpy(as_shot_ptr + 12, &g_den, 4);
    std::memcpy(as_shot_ptr + 16, &b_num, 4);
    std::memcpy(as_shot_ptr + 20, &b_den, 4);
}

bool VideoRecorder::pushFrame(const uint8_t* raw_pixels, size_t data_size, uint32_t stride, const WhiteBalance& wb, double shutterUs, double gain) {
    if (!m_isRecording) return false;
    if (m_currentCodec != CODEC_CINEMADNG) {
        // H264/H265 是通过 pushRgbFrame 由外界处理
        return false;
    }

    size_t current_write = m_writeIdx.load(std::memory_order_relaxed);
    size_t current_read = m_readIdx.load(std::memory_order_acquire);
    
    // 计算剩余空间百分比并动态通知 UI (低频触发机制以防洪水事件)
    size_t usedSlots = (current_write + m_numSlots - current_read) % m_numSlots;
    float currentPercentage = 1.0f - (float)usedSlots / (float)m_numSlots;
    if (std::abs(currentPercentage - m_lastReportedPercentage) >= (1.0f / 128.0f)) {
        m_lastReportedPercentage = currentPercentage;
        emit memoryPoolStatusUpdated(currentPercentage);
    }

    size_t next_write = (current_write + 1) % m_numSlots;

    // memory_order_acquire 保证看到消费者最新的读指针位置
    if (next_write == current_read) {
        bool expected = true;
        if (m_isEngineRunning.compare_exchange_strong(expected, false)) {
            // 不再使用 m_forceAbort，让 ioThread 自然把池子里的剩余数据刷完！
            qCritical() << "ERROR: Ring buffer FULL! SD Card too slow. Stopping recording automatically.";
            emit recordingError("cDNG池满");
            // 使用 Qt 的 invokeMethod 跨线程安全调用 stopRecording
            QMetaObject::invokeMethod(this, "stopRecording", Qt::QueuedConnection);
        }
        return false;
    }

    uint8_t* slot_base = m_memoryPool + (current_write * FRAME_SIZE);

    // 实时填充 DNG 模板
    std::memcpy(slot_base, m_dngHeader, DNG_PREFIX_SIZE);

    // 剔除行对齐填充，仅拷贝有效像素
    uint8_t* dst = slot_base + DNG_PREFIX_SIZE;
    const uint8_t* src = raw_pixels;
    if (stride == PIXEL_BYTES_PER_ROW) {
        std::memcpy(dst, src, RAW_PIXEL_SIZE);
    } else {
        for (uint32_t row = 0; row < SENSOR_HEIGHT; ++row) {
            std::memcpy(dst, src, PIXEL_BYTES_PER_ROW);
            dst += PIXEL_BYTES_PER_ROW;
            src += stride;
        }
    }

    // 锁定首帧曝光和白平衡，防止序列解析闪烁
    static uint32_t lockedShutterNum = 0;
    static uint32_t lockedIsoVal = 0;
    static uint16_t lockedRCoeff = 256;
    static uint16_t lockedBCoeff = 256;

    if (m_captureCounter.load(std::memory_order_relaxed) == 0 || lockedShutterNum == 0) {
        lockedShutterNum = (uint32_t)shutterUs;
        lockedIsoVal = (uint32_t)(gain * 100);
        
        lockedRCoeff = (wb.r_coeff == 0) ? 256 : wb.r_coeff;
        lockedBCoeff = (wb.b_coeff == 0) ? 256 : wb.b_coeff;
    }

    // 打上白平衡和曝光参数热补丁
    WhiteBalance lockedWb = { lockedRCoeff, lockedBCoeff };
    patchWhiteBalance(slot_base, lockedWb);

    #ifdef DNG_EXPOSURE_TIME_OFFSET
    uint32_t shutterDen = 1000000;
    memcpy(slot_base + DNG_EXPOSURE_TIME_OFFSET, &lockedShutterNum, 4);
    memcpy(slot_base + DNG_EXPOSURE_TIME_OFFSET + 4, &shutterDen, 4);
    #endif

    #ifdef DNG_ISO_OFFSET
    memcpy(slot_base + DNG_ISO_OFFSET, &lockedIsoVal, 4);
    #endif

    // memory_order_release 确保数据全部写完后，再更新索引让消费者可见
    m_writeIdx.store(next_write, std::memory_order_release);

    // 唤醒底层的 futex 等待
    m_writeIdx.notify_one();

    // 用于主线程同步抓取时间码
    m_captureCounter.fetch_add(1, std::memory_order_relaxed);

    // 每 60 帧打印一次，确认推帧链路正常
    static uint32_t dbgFrameCount = 0;
    if (++dbgFrameCount % 60 == 0) {
        qDebug() << "VideoRecorder: pushFrame OK, total pushed =" << dbgFrameCount
                 << "| ring write =" << next_write
                 << "| pool remaining =" << (currentPercentage * 100.0f) << "%";
    }

    return true;
}

void VideoRecorder::ioThreadWorker() {
    std::vector<uint8_t> cleanBuffer;

    while ((m_isEngineRunning.load(std::memory_order_acquire) ||
        m_readIdx.load(std::memory_order_relaxed) != m_writeIdx.load(std::memory_order_relaxed)) &&
        !m_forceAbort.load(std::memory_order_acquire)) {

        size_t current_read = m_readIdx.load(std::memory_order_relaxed);
        size_t current_write = m_writeIdx.load(std::memory_order_acquire);

        if (current_read == current_write) {
            if (m_isEngineRunning.load(std::memory_order_acquire)) {
                // 不使用 m_writeIdx.wait()，因为 C++20 wait 内部会循环检查旧值，导致单纯 notify 无法唤醒退出线程
                std::this_thread::sleep_for(std::chrono::milliseconds(4));
            }
            continue;
        }

        uint8_t* slot_base = m_memoryPool + (current_read * FRAME_SIZE);

        if (m_currentCodec == CODEC_CINEMADNG) {
            char filename[256];
            snprintf(filename, sizeof(filename), "%s/frame_%06u.dng", m_currentSessionDir.c_str(), ++m_frameCounter);

            int fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd != -1) {
                write(fd, slot_base, ACTUAL_DNG_SIZE);
                
                fdatasync(fd);
                posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
                
                close(fd);
            } else {
                qCritical() << "ERROR: Failed to open file for writing:" << filename;
            }
            // 数据落盘后使用 madvise 回收物理页
            madvise(slot_base, FRAME_SIZE, MADV_DONTNEED);
        } else {
            // H.264 / H.265 向 FFmpeg 管道写入 YUV 帧
            size_t data_size = *reinterpret_cast<size_t*>(slot_base);
            const uint8_t* yuv_pixels = slot_base + sizeof(size_t);

            if (m_ffmpegPipe) {
                uint32_t w = (m_videoWidth > 0) ? m_videoWidth : 1456;
                uint32_t h = (m_videoHeight > 0) ? m_videoHeight : 1088;
                uint32_t stride = (m_videoStride > 0) ? m_videoStride : w;

                if (stride == w || stride == 0) {
                    size_t expected_size = w * h * 3 / 2;
                    size_t write_size = (data_size > expected_size) ? expected_size : data_size;
                    fwrite(yuv_pixels, 1, write_size, m_ffmpegPipe);
                } else {
                    size_t expected_size = w * h * 3 / 2;
                    if (cleanBuffer.size() < expected_size) {
                        cleanBuffer.resize(expected_size);
                    }
                    
                    uint8_t* src = const_cast<uint8_t*>(yuv_pixels);
                    uint8_t* dst = cleanBuffer.data();
                    
                    for (uint32_t y = 0; y < h; ++y) {
                        std::memcpy(dst, src, w);
                        src += stride;
                        dst += w;
                    }
                    
                    for (uint32_t y = 0; y < h / 2; ++y) {
                        std::memcpy(dst, src, w / 2);
                        src += stride / 2;
                        dst += w / 2;
                    }
                    
                    for (uint32_t y = 0; y < h / 2; ++y) {
                        std::memcpy(dst, src, w / 2);
                        src += stride / 2;
                        dst += w / 2;
                    }
                    
                    fwrite(cleanBuffer.data(), 1, expected_size, m_ffmpegPipe);
                }
            }
        }

        // 释放该 Slot 给 Producer
        m_readIdx.store((current_read + 1) % m_numSlots, std::memory_order_release);
    }
    
    if (m_currentCodec != CODEC_CINEMADNG) {
        FILE* pipe = m_ffmpegPipe.exchange(nullptr, std::memory_order_acquire);
        if (pipe) {
            qDebug() << "VideoRecorder: Closing ffmpeg pipe (waiting for moov atom...)";
            pclose(pipe);
            qDebug() << "VideoRecorder: ffmpeg process exited. moov atom sealed.";
        }
    }

    // 落盘完全结束
    m_ioFlushing.store(false, std::memory_order_release);
    qDebug() << "VideoRecorder: ioThread flush completely finished.";
}

void VideoRecorder::proxyThreadWorker() {
    while ((m_isEngineRunning.load(std::memory_order_acquire) ||
           m_proxyReadIdx.load(std::memory_order_relaxed) != m_proxyWriteIdx.load(std::memory_order_relaxed)) &&
           !m_forceAbort.load(std::memory_order_acquire)) {

        size_t current_read = m_proxyReadIdx.load(std::memory_order_relaxed);
        size_t current_write = m_proxyWriteIdx.load(std::memory_order_acquire);

        if (current_read == current_write) {
            if (m_isEngineRunning.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(4));
            }
            continue;
        }

        uint8_t* slot_base = m_proxyPool + (current_read * MAX_YUV_SIZE);
        size_t data_size = *reinterpret_cast<size_t*>(slot_base);
        const uint8_t* yuv_pixels = slot_base + sizeof(size_t);

        if (m_ffmpegPipe) {
            uint32_t w = (m_videoWidth > 0) ? m_videoWidth : 1456;
            uint32_t h = (m_videoHeight > 0) ? m_videoHeight : 1088;
            uint32_t stride = (m_videoStride > 0) ? m_videoStride : w;

            if (stride == w || stride == 0) {
                size_t expected_size = w * h * 3 / 2;
                size_t write_size = (data_size > expected_size) ? expected_size : data_size;
                fwrite(yuv_pixels, 1, write_size, m_ffmpegPipe);
            } else {
                size_t expected_size = w * h * 3 / 2;
                if (m_proxyCleanBuffer.size() < expected_size) {
                    m_proxyCleanBuffer.resize(expected_size);
                }
                
                uint8_t* src = const_cast<uint8_t*>(yuv_pixels);
                uint8_t* dst = m_proxyCleanBuffer.data();
                
                for (uint32_t y = 0; y < h; ++y) {
                    std::memcpy(dst, src, w);
                    src += stride;
                    dst += w;
                }
                
                for (uint32_t y = 0; y < h / 2; ++y) {
                    std::memcpy(dst, src, w / 2);
                    src += stride / 2;
                    dst += w / 2;
                }
                
                for (uint32_t y = 0; y < h / 2; ++y) {
                    std::memcpy(dst, src, w / 2);
                    src += stride / 2;
                    dst += w / 2;
                }
                
                fwrite(m_proxyCleanBuffer.data(), 1, expected_size, m_ffmpegPipe);
            }
        }
        
        // 释放代理缓冲区的物理页
        madvise(slot_base, MAX_YUV_SIZE, MADV_DONTNEED);
        
        m_proxyReadIdx.store((current_read + 1) % m_proxyNumSlots, std::memory_order_release);
    }

    // Proxy 线程收尾工作
    FILE* pipe = m_ffmpegPipe.exchange(nullptr, std::memory_order_acquire);
    if (pipe) {
        qDebug() << "VideoRecorder (ProxyThread): Closing proxy ffmpeg pipe (waiting for moov atom...)";
        pclose(pipe);
        qDebug() << "VideoRecorder (ProxyThread): Proxy ffmpeg process exited. moov atom sealed.";

        QString sessionName = QString::fromStdString(m_currentSessionDir).split('/').last();
        QString srcProxy = QString("/dev/shm/video_%1_proxy.mp4").arg(sessionName);
        QString dstProxy = QString::fromStdString(m_currentSessionDir) + "/video_" + sessionName + "_proxy.mp4";
        
        qDebug() << "VideoRecorder (ProxyThread): Enqueueing proxy transfer to background thread.";
        {
            std::lock_guard<std::mutex> lock(m_copyMutex);
            m_copyQueue.push({srcProxy, dstProxy});
        }
        m_copyCv.notify_one();
    }
    
    // Proxy 生成结束，解除 Proxy Flushing 状态，让 UI 恢复可录制状态
    m_proxyFlushing.store(false, std::memory_order_release);
    qDebug() << "VideoRecorder: proxyThread flush completely finished.";
}

void VideoRecorder::copyThreadWorker() {
    while (true) {
        std::pair<QString, QString> task;
        {
            std::unique_lock<std::mutex> lock(m_copyMutex);
            m_copyCv.wait(lock, [this]() { return !m_copyQueue.empty() || m_stopCopyThread.load(); });
            
            if (m_stopCopyThread.load() && m_copyQueue.empty()) {
                break;
            }
            task = m_copyQueue.front();
            m_copyQueue.pop();
        }
        
        qDebug() << "VideoRecorder (CopyThread): Moving Proxy from RAM to SD Card:" << task.first << "->" << task.second;
        if (QFile::exists(task.first)) {
            bool success = QFile::rename(task.first, task.second);
            if (!success) {
                qDebug() << "VideoRecorder (CopyThread): rename failed (cross-device link?), falling back to copy...";
                // 写入临时文件，防止相册提前读取到不完整的半截文件导致黑封面
                QString tmpDst = task.second + ".tmp";
                QFile::remove(tmpDst); 
                if (QFile::copy(task.first, tmpDst)) {
                    QFile::remove(task.second);
                    if (QFile::rename(tmpDst, task.second)) {
                        success = true;
                    } else {
                        qCritical() << "VideoRecorder (CopyThread): Proxy rename tmp FAILED!";
                        QFile::remove(tmpDst);
                    }
                } else {
                    qCritical() << "VideoRecorder (CopyThread): Proxy copy FAILED! (Storage full?)";
                }
                
                // 必须移除 /dev/shm 中的源文件，防止内存盘 OOM 漏油
                QFile::remove(task.first);
            }
            
            if (success) {
                qDebug() << "VideoRecorder (CopyThread): Proxy transfer completed successfully.";
            }
        } else {
            qWarning() << "VideoRecorder (CopyThread): Proxy output file not found in RAM disk:" << task.first;
        }
    }
    qDebug() << "VideoRecorder (CopyThread): Exiting copy worker.";
}

bool VideoRecorder::pushYuvFrame(const uint8_t* yuv_pixels, size_t data_size) {
    if (!m_isRecording) return false;

    if (m_currentCodec == CODEC_CINEMADNG) {
        // [Proxy Workflow] H.264 代理流旁路推入
        if (!m_ffmpegPipe || !m_proxyPool) return false;

        size_t current_write = m_proxyWriteIdx.load(std::memory_order_relaxed);
        size_t current_read = m_proxyReadIdx.load(std::memory_order_acquire);
        size_t next_write = (current_write + 1) % m_proxyNumSlots;

        if (next_write == current_read) {
            // 若代理缓冲池满，丢弃当前 YUV 帧以保证 RAW 数据的连续性
            qWarning() << "WARNING: Proxy buffer FULL! Dropping YUV frame to protect RAW stream.";
            auto now = std::chrono::steady_clock::now().time_since_epoch();
            m_lastProxyDropTimeMs.store(std::chrono::duration_cast<std::chrono::milliseconds>(now).count(), std::memory_order_relaxed);
            // 不再抛出 emit recordingError 也不再 stopRecording，直接静默丢帧
            return false;
        }

        uint8_t* slot_base = m_proxyPool + (current_write * MAX_YUV_SIZE);
        size_t* size_ptr = reinterpret_cast<size_t*>(slot_base);
        uint8_t* dst = slot_base + sizeof(size_t);

        size_t copy_size = (data_size > (MAX_YUV_SIZE - sizeof(size_t))) ? (MAX_YUV_SIZE - sizeof(size_t)) : data_size;
        *size_ptr = copy_size;
        std::memcpy(dst, yuv_pixels, copy_size);

        m_proxyWriteIdx.store(next_write, std::memory_order_release);
        m_proxyWriteIdx.notify_one();
        return true;
    }

    // H.264/H.265 主视频录制 YUV420 数据大小约 2.4MB (< FRAME_SIZE)，复用 6GB 环形缓冲池防丢帧
    size_t current_write = m_writeIdx.load(std::memory_order_relaxed);
    size_t current_read = m_readIdx.load(std::memory_order_acquire);
    
    // 计算剩余空间并通知 UI
    size_t usedSlots = (current_write + m_numSlots - current_read) % m_numSlots;
    float currentPercentage = 1.0f - (float)usedSlots / (float)m_numSlots;
    if (std::abs(currentPercentage - m_lastReportedPercentage) >= (1.0f / 128.0f)) {
        m_lastReportedPercentage = currentPercentage;
        emit memoryPoolStatusUpdated(currentPercentage);
    }

    size_t next_write = (current_write + 1) % m_numSlots;
    if (next_write == current_read) {
        bool expected = true;
        if (m_isEngineRunning.compare_exchange_strong(expected, false)) {
            qCritical() << "ERROR: YUV buffer FULL! Stopping recording automatically.";
            emit recordingError("缓存池已满\n存储速度不足\n已自动停止录制");
            QMetaObject::invokeMethod(this, "stopRecording", Qt::QueuedConnection);
        }
        return false;
    }

    uint8_t* slot_base = m_memoryPool + (current_write * FRAME_SIZE);
    // 在 slot 头部记录 data_size
    size_t* size_ptr = reinterpret_cast<size_t*>(slot_base);
    *size_ptr = data_size;
    std::memcpy(slot_base + sizeof(size_t), yuv_pixels, data_size);

    m_writeIdx.store(next_write, std::memory_order_release);
    m_writeIdx.notify_one();

    m_captureCounter.fetch_add(1, std::memory_order_relaxed);
    return true;
}

