#include "MainWindow.h"
#include <chrono>
#include <QVBoxLayout>
#include <vector>
#include <QPainter>
#include <QPainterPath>
#include <QDateTime>
#include <QtConcurrent>
#include <QFuture>
#include <QBuffer>
#include "../core/DngTemplate.h"
#include "../core/PhotoDngTemplate.h"
#include <QDir>
#include <QFile>
#include <QDirIterator>
#include <QDebug>
#include <cmath>
#include <algorithm>
#include <gpiod.h>
#include <QProcess>
#include <QMessageBox>
#include <QSettings>


MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    // 设置窗口背景颜色
    this->resize(640, 480);
    this->setStyleSheet("background-color: black;");

    // 创建中心部件
    QWidget* centralWidget = new QWidget(this);
    setCentralWidget(centralWidget);

    QVBoxLayout* layout = new QVBoxLayout(centralWidget);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // 取景器 (Viewfinder)
    viewfinder = new QLabel(this);
    viewfinder->setAlignment(Qt::AlignCenter);
    // 确保 viewfinder 能接收鼠标事件（虽然我们是在 MainWndow 处理，但防万一）
    viewfinder->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    viewfinder->setText("Waiting for Sensor...");
    viewfinder->setStyleSheet("color: white; font-size: 24px;");
    viewfinder->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    layout->addWidget(viewfinder);

    // 初始化 EV 弹窗
    evPopup = new EvControlWidget(this); // MainWindow 的子控件，浮在上面
    evPopup->hide(); // 默认隐藏

    // 连接信号：当标尺变动时 -> 设置相机
    connect(evPopup, &EvControlWidget::evValueChanged, this, &MainWindow::onEvChanged);

    // 初始化 测光模式 弹窗
    meterPopup = new MeteringControlWidget(this);
    meterPopup->hide();
    // 连接信号：弹窗被点击 -> 切换模式
    connect(meterPopup, &MeteringControlWidget::modeSelected, this, &MainWindow::onMeteringChanged);
    
    // 初始化 AWB 弹窗
    awbPopup = new AwbControlWidget(this);
    awbPopup->hide();
    connect(awbPopup, &AwbControlWidget::awbChanged, this, &MainWindow::onAwbChanged);

    isoPopup = new IsoControlWidget(this);
    isoPopup->hide();
    connect(isoPopup, &IsoControlWidget::isoChanged, this, &MainWindow::onIsoChanged);

    shutterPopup = new ShutterControlWidget(this);
    shutterPopup->hide();
    connect(shutterPopup, &ShutterControlWidget::shutterChanged, this, &MainWindow::onShutterChanged);

    peakingPopup = new PeakingControlWidget(this);
    peakingPopup->hide();
    connect(peakingPopup, &PeakingControlWidget::peakingChanged, this, &MainWindow::onPeakingChanged);

    // 初始化引擎
    engine = new CameraEngine(this);

    // 初始化硬件管理器
    hwManager = new HardwareManager(this);
    
    // 初始化 PIO 触发控制器 (负责 IMX296 外部触发和闪光同步)
    pioController = new PioTriggerController(this);
    m_currentVideoFps = 24.0f; // 初始视频帧率默认 24
    if (pioController->init()) {
        pioController->startPreview(getTargetFps(), m_manualShutterUs);
    } else {
        qWarning() << "MainWindow: PioTriggerController init failed (will use fallback).";
        pioController->startPreview(getTargetFps(), m_manualShutterUs);
    }

    // 拍摄看门狗：5 秒超时自动复位
    m_captureWatchdog = new QTimer(this);
    m_captureWatchdog->setSingleShot(true);
    connect(m_captureWatchdog, &QTimer::timeout, this, [this]() {
        qWarning() << "MainWindow: Capture watchdog triggered! Forcing cleanup.";
        m_isCapturing = false;
        m_captureArmed = false;
        m_photoCaptureSkipFrames = -1;
    });

    // 连接 PIO 硬件曝光完成信号（调试确认 + 未来可扩展）
    if (pioController) {
        connect(pioController, &PioTriggerController::exposureCompleted, this, [this]() {
            qDebug() << "MainWindow: PIO hardware confirmed exposure complete.";
        }, Qt::QueuedConnection);
    }

    // 读取持久化闪光延迟配置
    {
        QSettings settings("C9M3", "Camera");
        m_flashPreDelay = settings.value("flashPreDelay", 1200).toInt();
        qDebug() << "MainWindow: Loaded flashPreDelay =" << m_flashPreDelay << "us from QSettings.";
    }

    connect(hwManager, &HardwareManager::rotaryEncoderScroll, this, &MainWindow::onRotaryEncoderScroll);

    // (硬件轮询线程在下方所有信号连接完毕后再启动)

    // 注意：ZSL JPEG 内存捕获依然使用 engine::photoTaken 信号处理


    // 初始化相册模块
    m_albumWidget = new AlbumWidget(this);
    m_albumWidget->hide(); // 默认隐藏

    // 收到关闭信号 -> 隐藏相册
    connect(m_albumWidget, &AlbumWidget::closed, this, [=]() {
        m_albumWidget->hide();
        // 如果之前暂停了相机引擎，这里可以 resume
        });

    // --- 初始化设置菜单 ---
    m_settingsMenu = new SettingsMenu(this);
    m_settingsMenu->hide();
    connect(m_settingsMenu, &SettingsMenu::closed, this, [=]() {
        m_settingsMenu->hide();
    });
    connect(m_settingsMenu, &SettingsMenu::gridLineModeChanged, this, &MainWindow::onGridLineModeChanged);
    connect(m_settingsMenu, &SettingsMenu::flashCalibrationRequested, this, &MainWindow::startFlashCalibration);
    connect(m_settingsMenu, &SettingsMenu::manualFlashCalibrationRequested, this, &MainWindow::startManualFlashCalibration);

    // 闪光校准覆盖层 (默认隐藏)
    m_calibOverlay = new QLabel(this);
    m_calibOverlay->setAlignment(Qt::AlignCenter);
    m_calibOverlay->setStyleSheet(
        "QLabel {"
        "  background-color: rgba(0, 0, 0, 170);"
        "  color: white;"
        "  font-family: 'WenQuanYi Zen Hei', 'SimHei', Arial;"
        "  font-size: 28px;"
        "  font-weight: bold;"
        "  border-radius: 16px;"
        "}"
    );
    m_calibOverlay->setGeometry(80, 150, 480, 180);
    m_calibOverlay->hide();

    // 初始化手动校准 UI
    m_manualCalibWidget = new QWidget(this);
    m_manualCalibWidget->resize(600, 100);
    // 屏幕下方居中
    m_manualCalibWidget->setStyleSheet("background-color: rgba(30, 30, 30, 200); border-radius: 20px;");
    
    QHBoxLayout* manualLayout = new QHBoxLayout(m_manualCalibWidget);
    manualLayout->setContentsMargins(20, 10, 20, 10);
    manualLayout->setSpacing(30);

    m_btnManualCalibMinus = new QPushButton("-", m_manualCalibWidget);
    m_btnManualCalibPlus = new QPushButton("+", m_manualCalibWidget);
    m_lblManualCalibDelay = new QLabel("1200 us", m_manualCalibWidget);

    QString btnStyle = "QPushButton { background-color: rgba(60,60,60,200); color: white; font-size: 40px; border-radius: 10px; }"
                       "QPushButton:pressed { background-color: rgb(255,140,0); }";
    m_btnManualCalibMinus->setStyleSheet(btnStyle);
    m_btnManualCalibPlus->setStyleSheet(btnStyle);
    m_btnManualCalibMinus->setFixedSize(80, 80);
    m_btnManualCalibPlus->setFixedSize(80, 80);

    // 支持长按自动重复
    m_btnManualCalibMinus->setAutoRepeat(true);
    m_btnManualCalibMinus->setAutoRepeatDelay(400);
    m_btnManualCalibMinus->setAutoRepeatInterval(30); // 快速跳变
    m_btnManualCalibPlus->setAutoRepeat(true);
    m_btnManualCalibPlus->setAutoRepeatDelay(400);
    m_btnManualCalibPlus->setAutoRepeatInterval(30);

    m_lblManualCalibDelay->setStyleSheet("color: rgb(255,140,0); font-size: 36px; font-weight: bold; background: transparent;");
    m_lblManualCalibDelay->setAlignment(Qt::AlignCenter);
    
    // 让 Label 支持点击 (使用 EventFilter)
    m_lblManualCalibDelay->installEventFilter(this);

    manualLayout->addWidget(m_btnManualCalibMinus);
    manualLayout->addWidget(m_lblManualCalibDelay, 1);
    manualLayout->addWidget(m_btnManualCalibPlus);

    m_manualCalibWidget->hide();

    connect(m_btnManualCalibMinus, &QPushButton::clicked, this, &MainWindow::onManualCalibMinus);
    connect(m_btnManualCalibPlus, &QPushButton::clicked, this, &MainWindow::onManualCalibPlus);

    // 校准节拍定时器
    m_calibTimer = new QTimer(this);
    m_calibTimer->setSingleShot(false);
    connect(m_calibTimer, &QTimer::timeout, this, &MainWindow::onCalibrationTick);

    // 初始化回放按钮
    btnPlayback = new StrokeButton(QString::fromUtf8("▶"), this);
    btnPlayback->setFixedSize(80, 60);
    QFont playFont("Arial", 30);
    btnPlayback->setFont(playFont);
    btnPlayback->setTextOffset(3 ,-4);

    // 点击按钮 -> 打开相册
    connect(btnPlayback, &QPushButton::clicked, this, [=]() {
        // 设为跟主窗口一样大，覆盖上去
        m_albumWidget->setGeometry(0, 0, this->width(), this->height());
        m_albumWidget->openAlbum();
        });

    // 确保按钮默认不显示，等 updateFrame 定位
    btnPlayback->hide();

    // 当引擎发图 -> 调用 updateFrame 更新画面
    connect(engine, &CameraEngine::frameCaptured, this, &MainWindow::updateFrame);

    // 初始化视频模块 (自动适应系统内存)
    m_videoRecorder = new VideoRecorder("/media/pumpkin/FILM/Video", this);

    // 直接在硬件回调线程中同步推帧，避免信号延迟
    // stride: libcamera 每行字节数（含对齐填充），透传给 pushFrame 按行解除填充
    connect(engine, &CameraEngine::rawFrameCaptured, this,
    [=](const uint8_t* rawData, size_t dataSize, uint32_t stride,
        uint16_t rGain, uint16_t bGain, double shutterUs, double gain,
        int64_t sensorTimestampNs) {

        // 缓存最新帧的白平衡增益
        m_lastRGain = rGain / 256.0f;
        m_lastBGain = bGain / 256.0f;

        // 亮度快速采样 (无锁)
        // 提示: RAW 数据结构为小端 10-bit (存储为 16-bit)，需以 uint16_t 解析
        const uint16_t* raw16 = reinterpret_cast<const uint16_t*>(rawData);
        size_t pixelCount = dataSize / 2;
        uint64_t lumaSum = 0;
        uint32_t sampleCount = 0;
        for (uint32_t i = 0; i < pixelCount; i += 64) {
            lumaSum += raw16[i];
            sampleCount++;
        }
        float currentLuma = (float)lumaSum / sampleCount;

        m_calibLastRawLuma.store(currentLuma, std::memory_order_relaxed);
        if (m_calibState != CALIB_OFF) {
            float maxL = m_calibMaxLuma.load(std::memory_order_relaxed);
            if (currentLuma > maxL)
                m_calibMaxLuma.store(currentLuma, std::memory_order_relaxed);
        }

        // 视频录制流水线
        if (m_isVideoMode && m_videoRecorder->isRecording()) {
            WhiteBalance wb = { rGain, bGain };
            m_videoRecorder->pushFrame(rawData, dataSize, stride, wb, shutterUs, gain);
            return;
        }

        int skip = m_photoCaptureSkipFrames.load();
        if (!m_isVideoMode && skip == 0 && m_photoFormat == Photo_DNG) {
            // DNG 单帧捕获 (无闪光/校准模式)
            m_photoCaptureSkipFrames = -1;

            constexpr uint32_t kWidth  = 1456;
            constexpr uint32_t kHeight = 1088;
            constexpr uint32_t kRowBytes = kWidth * 2;
            QByteArray cleanRaw;
            cleanRaw.reserve(kRowBytes * kHeight);
            for (uint32_t y = 0; y < kHeight; ++y)
                cleanRaw.append(reinterpret_cast<const char*>(rawData + y * stride), kRowBytes);

            QMutexLocker locker(&m_captureMutex);
            m_capturedRawData = std::move(cleanRaw);
            m_rawCaptured = true;
            locker.unlock();

            QMetaObject::invokeMethod(this, [this]() { checkAndSaveSingleDng(); }, Qt::QueuedConnection);
        } else if (!m_isVideoMode && m_captureArmed && m_photoFormat == Photo_DNG) {
            //  闪光 DNG 硬件时间戳精准匹配 (Timestamp Matcher)
            int64_t fireNs = pioController ? pioController->lastFireTimestampNs() : 0;
            int64_t diffNs = sensorTimestampNs - fireNs;
            if (diffNs > -5000000 && diffNs < 50000000) { // 容忍 -5ms 到 +50ms
                constexpr uint32_t kWidth  = 1456;
                constexpr uint32_t kHeight = 1088;
                constexpr uint32_t kRowBytes = kWidth * 2;
                QByteArray cleanRaw;
                cleanRaw.reserve(kRowBytes * kHeight);
                for (uint32_t y = 0; y < kHeight; ++y)
                    cleanRaw.append(reinterpret_cast<const char*>(rawData + y * stride), kRowBytes);

                QMutexLocker locker(&m_captureMutex);
                m_capturedRawData = std::move(cleanRaw);
                m_rawCaptured = true;
                locker.unlock();

                qDebug() << "MainWindow: Flash RAW Matched! Diff =" << diffNs / 1000000.0 << "ms";
                QMetaObject::invokeMethod(this, [this]() { checkAndSaveSingleDng(); }, Qt::QueuedConnection);
            }
        }
    }, Qt::DirectConnection);


    // ZSL JPEG 内存捕获（用于常规单张拍照）
    connect(engine, &CameraEngine::photoTaken, this, [=](QImage image) {
        if (m_isVideoMode) return;
        
        // 后台压缩 QImage 为 JPEG 字节流
        QtConcurrent::run([=, img = std::move(image)]() {
            QByteArray jpgBytes;
            QBuffer buffer(&jpgBytes);
            buffer.open(QIODevice::WriteOnly);
            img.save(&buffer, "JPG", 95);
            
            QMutexLocker locker(&m_captureMutex);
            m_capturedJpgData = std::move(jpgBytes);
            m_jpgCaptured = true;
            locker.unlock();
            
            // 尝试触发组装落盘
            QMetaObject::invokeMethod(this, [this]() { checkAndSaveSingleDng(); }, Qt::QueuedConnection);
        });
    }, Qt::DirectConnection);

    // DNG+Flash JPEG 时间戳精准提取
    connect(engine, &CameraEngine::frameCaptured, this, [=](QImage image, double shutter, double gain, float ev, int64_t sensorTimestampNs) {
        if (m_isVideoMode) return;
        
        if (m_flashEnabled && m_captureArmed) {
            int64_t fireNs = pioController ? pioController->lastFireTimestampNs() : 0;
            int64_t diffNs = sensorTimestampNs - fireNs;
            if (diffNs > -5000000 && diffNs < 50000000) {
                // 后台压缩 QImage 为 JPEG 字节流
                QtConcurrent::run([=, img = std::move(image)]() {
                    QByteArray jpgBytes;
                    QBuffer buffer(&jpgBytes);
                    buffer.open(QIODevice::WriteOnly);
                    img.save(&buffer, "JPG", 95);
                    
                    QMutexLocker locker(&m_captureMutex);
                    m_capturedJpgData = std::move(jpgBytes);
                    m_jpgCaptured = true;
                    locker.unlock();
                    
                    qDebug() << "MainWindow: Flash JPG Matched! Diff =" << diffNs / 1000000.0 << "ms";
                    QMetaObject::invokeMethod(this, [this]() { checkAndSaveSingleDng(); }, Qt::QueuedConnection);
                });
            }
        }
    }, Qt::DirectConnection);


    // 全分辨率 YUV 视频流管道
    connect(engine, &CameraEngine::videoFrameCaptured, this, [=](const uint8_t* yuvData, size_t dataSize) {
        if (m_isVideoMode && m_videoRecorder->isRecording()) {
            m_videoRecorder->pushYuvFrame(yuvData, dataSize);
        }
    }, Qt::DirectConnection);

    // 将 CameraEngine 广播的实际 YUV 流参数同步给录像器
    // CameraEngine::start() 在 camera->configure() 后立即发出此信号（早于任何录制开始）
    // VideoRecorder 用这些值构造正确的 FFmpeg 命令（使用真实 stride 而非硬编码分辨率）
    connect(engine, &CameraEngine::videoStreamConfigured,
            m_videoRecorder, &VideoRecorder::setVideoStreamInfo);

    m_videoHud = new VideoHudWidget(this);
    // 覆盖整个中心窗口，并置于最顶层
    m_videoHud->setGeometry(0, 0, 640, 480);
    m_videoHud->setCodecText(m_videoRecorder->currentCodecName());
    m_videoHud->hide(); // 默认隐藏，假设初始为拍照模式

    // 连接硬件信号
    // 模式切换开关 (AW9523 P0_7)
    connect(hwManager, &HardwareManager::modeSwitched, this, &MainWindow::onModeSwitched);
    // 驱动模式转盘 (AW9523 P0_0~P0_3)
    connect(hwManager, &HardwareManager::driveModeChanged, this, &MainWindow::onDriveModeChanged);
    // REC 按键 (AW9523 P1_7 + GPIO25) —— 承担视频录制与照片快门功能
    connect(hwManager, &HardwareManager::videoRecordPressed, this, [=]() {
        qDebug() << "MainWindow: videoRecordPressed received | m_isVideoMode =" << m_isVideoMode
                 << "| isRecording =" << m_videoRecorder->isRecording();
        if (m_isVideoMode) { // 视频模式：开启/停止录制
            if (m_videoRecorder->isRecording()) {
                m_videoRecorder->stopRecording();
            }
            else {
                m_videoRecorder->startRecording();
            }
        } else { // 照片模式：作为快门触发拍照
            onStillCapture();
        }
    });

    // 相册键 (AW9523 P0_5)
    connect(hwManager, &HardwareManager::albumButtonPressed, this, [=]() {
        // 如果正在录制，忽略回放请求
        if (m_isVideoMode && m_videoRecorder->isRecording()) {
            qDebug() << "MainWindow: Recording in progress. Ignore album playback.";
            return;
        }

        if (m_isVideoMode) {
            // 视频模式：每次打开都重新扫描（确保新录制文件出现），再次按下则关闭
            if (m_albumWidget->isVisible()) {
                m_albumWidget->hide();
            } else {
                m_albumWidget->setGeometry(0, 0, this->width(), this->height());
                m_albumWidget->openVideoAlbum(); // 内部每次都重新扫描
            }
        } else {
            // 拍照模式：动态开关图片相册
            if (m_albumWidget->isVisible()) {
                m_albumWidget->hide();
            } else {
                m_albumWidget->setGeometry(0, 0, this->width(), this->height());
                m_albumWidget->openAlbum();
            }
        }
    });

    // Fn 键 (AW9523 P0_6) → 退出 mpv 播放 / 按住显示内存池容量
    connect(hwManager, &HardwareManager::fnPressed, m_albumWidget, &AlbumWidget::stopPlayback);
    connect(hwManager, &HardwareManager::fnPressed, this, [=]() {
        if (!m_videoHud) return;
        float dngGb      = m_videoRecorder->getDngPoolSizeGb();
        float proxyMb    = m_videoRecorder->getProxyPoolSizeMb();
        int   proxySlots = m_videoRecorder->getProxyPoolSlots();
        QString msg = QString("DNG Pool: %1 GB\nProxy: %2 slots / %3 MB")
                        .arg(dngGb,    0, 'f', 2)
                        .arg(proxySlots)
                        .arg(proxyMb,  0, 'f', 0);
        m_videoHud->showToast(msg, 0); // 0 = 持久显示，松开后 fnReleased 隐藏
    });
    // 松开 Fn → 立即隐藏内存池 Toast
    connect(hwManager, &HardwareManager::fnReleased, this, [=]() {
        if (m_videoHud) m_videoHud->hideToast();
    });

    // Del 键 (AW9523 P0_4) → 删除当前相册图片/视频
    connect(hwManager, &HardwareManager::deletePressed, m_albumWidget, &AlbumWidget::onDeletePressed);

    // 摇杆居中 (GPIO 16) → 确认删除 / 播放视频
    connect(hwManager, &HardwareManager::joystickCenterPressed, m_albumWidget, &AlbumWidget::onJoystickCenterPressed);

    // 摇杆左右 (GPIO 5, 26) → 翻页
    connect(hwManager, &HardwareManager::joystickLeftPressed, m_albumWidget, &AlbumWidget::onJoystickLeftPressed);
    connect(hwManager, &HardwareManager::joystickRightPressed, m_albumWidget, &AlbumWidget::onJoystickRightPressed);

    // 设置菜单的摇杆控制
    connect(hwManager, &HardwareManager::joystickUpPressed, m_settingsMenu, &SettingsMenu::onJoystickUp);
    connect(hwManager, &HardwareManager::joystickDownPressed, m_settingsMenu, &SettingsMenu::onJoystickDown);
    connect(hwManager, &HardwareManager::joystickLeftPressed, m_settingsMenu, &SettingsMenu::onJoystickLeft);
    connect(hwManager, &HardwareManager::joystickRightPressed, m_settingsMenu, &SettingsMenu::onJoystickRight);
    connect(hwManager, &HardwareManager::joystickCenterPressed, m_settingsMenu, &SettingsMenu::onJoystickCenter);

    // MainWindow 自身的摇杆路由 (供 FPS 微调等)
    connect(hwManager, &HardwareManager::joystickUpPressed, this, &MainWindow::onJoystickUp);
    connect(hwManager, &HardwareManager::joystickDownPressed, this, &MainWindow::onJoystickDown);
    connect(hwManager, &HardwareManager::joystickLeftPressed, this, &MainWindow::onJoystickLeft);
    connect(hwManager, &HardwareManager::joystickRightPressed, this, &MainWindow::onJoystickRight);
    connect(hwManager, &HardwareManager::joystickCenterPressed, this, &MainWindow::onJoystickCenter);

    // 长按 Del + Menu 3 秒退出应用
    connect(hwManager, &HardwareManager::exitAppRequested, this, &MainWindow::close);

    // Menu 键 (AW9523 P1_4) 
    connect(hwManager, &HardwareManager::menuPressed, this, &MainWindow::onMenuPressed);


    //  连接视频录制器与索尼风格 HUD
    connect(m_videoRecorder, &VideoRecorder::recordingStarted, m_videoHud, [=]() {
        m_videoHud->setRecordingState(true);
        hwManager->setTallyLed(true);
        });
    connect(m_videoRecorder, &VideoRecorder::recordingStopped, m_videoHud, [=]() {
        m_videoHud->setRecordingState(false);
        hwManager->setTallyLed(false);
        });
    connect(m_videoHud, &VideoHudWidget::codecSwitchRequested, m_videoRecorder, &VideoRecorder::cycleCodec);
    connect(m_videoRecorder, &VideoRecorder::codecChanged, m_videoHud, &VideoHudWidget::setCodecText);
    connect(m_videoHud, &VideoHudWidget::fpsSwitchRequested, this, [=](float newFps) {
        if (m_videoRecorder->isRecording()) {
            m_videoHud->showToast("录制中无法切换帧率", 1500);
            return;
        }
        m_currentVideoFps = newFps;
        engine->setFps(newFps);
        m_videoRecorder->setFps(newFps);
        if (pioController && m_isVideoMode) {
            pioController->startPreview(m_currentVideoFps, m_manualShutterUs);
        }
    });
    connect(m_videoHud, &VideoHudWidget::showVariableFpsUiRequested, this, [=]() {
        if (m_videoRecorder->isRecording()) {
            m_videoHud->showToast("录制中无法切换帧率", 1500);
            return;
        }
        showVariableFpsUi();
    });
    connect(m_settingsMenu, &SettingsMenu::variableFpsToggled, m_videoHud, &VideoHudWidget::setVariableFpsMode);
    
    // 连接内存盘不足等录制错误提示
    connect(m_videoRecorder, &VideoRecorder::recordingError, this, [=](QString msg) {
        if (m_videoHud) {
            m_videoHud->showToast(msg, 2500);
        }
    });

    QTimer::singleShot(0, this, [this]() {
        hwManager->start();
        engine->start();
    });
}

MainWindow::~MainWindow() {
}

// EV槽函数
void MainWindow::onEvChanged(float newEv) {
    engine->setExposureCompensation(newEv);
    evPopup->setCurrentEv(newEv);
}

// 测光槽函数
void MainWindow::onMeteringChanged(int newMode) {
    currentMeteringMode = (MeteringMode)newMode;
    engine->setMeteringMode(newMode);
}

// AWB 槽函数
void MainWindow::onAwbChanged(bool isAuto, int tempK) {
    lastIsAutoAwb = isAuto;
    lastColorTemp = tempK;
    engine->setAwbMode(isAuto);
    engine->setColorTemperature(tempK);
    awbPopup->setCurrentMode(isAuto, tempK);
}

void MainWindow::onGridLineModeChanged(int mode) {
    m_gridLineMode = mode;
}

// 点击释放处理 (用于滑动手势检测)
void MainWindow::mouseReleaseEvent(QMouseEvent* event) {
    QPoint releasePos = event->pos();
    int dx = releasePos.x() - m_touchStartPos.x();
    int dy = releasePos.y() - m_touchStartPos.y();
    
    // 判断是否为有效的水平滑动 (X偏移超过100，Y偏移不超过80)
    if (qAbs(dx) > 100 && qAbs(dy) < 80) {
        if (dx < 0 && !m_uiHidden) {
            // 左划：隐藏
            m_uiHidden = true;
            // 隐藏正在显示的弹窗
            evPopup->hide();
            meterPopup->hide();
            isoPopup->hide();
            shutterPopup->hide();
            peakingPopup->hide();
            awbPopup->hide();
        } else if (dx > 0 && m_uiHidden) {
            // 右划：显示
            m_uiHidden = false;
        }
    }
    
    QMainWindow::mouseReleaseEvent(event);
}

// 点击处理
void MainWindow::mousePressEvent(QMouseEvent* event) {
    // 记录点击起始点，供 mouseReleaseEvent 使用
    m_touchStartPos = event->pos();
    QPoint clickPos = event->pos();
    if (!m_isVideoMode) {
        QRect hitFormat = m_photoFormatRect.adjusted(-15, -15, 15, 15);
        if (hitFormat.contains(clickPos)) {
            onPhotoFormatCycled();
            return;
        }
    }
    if (awbIndicatorRect.contains(clickPos)) {
        evPopup->hide();
        meterPopup->hide();
        isoPopup->hide();
        shutterPopup->hide();
        peakingPopup->hide();
        if (awbPopup->isHidden()) {
            // align with evPopup basically
            int popupX = (evBoxRect.center().x() - awbPopup->width() / 2) - 200; 
            popupX = qBound(10, popupX, width() - awbPopup->width() - 10);
            int popupY = evBoxRect.top() - awbPopup->height() - 10;
            
            // 如果 evBoxRect 尚未初始化，给出默认坐标
            if (evBoxRect.isNull()) {
                popupX = (width() - awbPopup->width()) / 2;
                popupY = height() - 35 - awbPopup->height() - 10;
            }

            awbPopup->move(popupX, popupY);
            awbPopup->setCurrentMode(lastIsAutoAwb, lastColorTemp);
            awbPopup->show();
            awbPopup->raise();
        } else {
            awbPopup->hide();
        }
        return;
    }

    if (evBoxRect.contains(clickPos)) {
        meterPopup->hide();
        awbPopup->hide();
        isoPopup->hide();
        shutterPopup->hide();
        peakingPopup->hide();
        // 只有在非 M，或者非 S 档时才允许使用 EV pop
        if (evPopup->isHidden()) {
            // 弹窗位置：对齐在 EV 按钮的正上方
            int popupX = (evBoxRect.center().x() - evPopup->width() / 2) - 200;
            popupX = qBound(10, popupX, width() - evPopup->width() - 10);
            int popupY = evBoxRect.top() - evPopup->height() - 10;
            evPopup->move(popupX, popupY);
            evPopup->setCurrentEv(lastEv);
            evPopup->show();
            evPopup->raise();
        }
        else {
            evPopup->hide();
        }
        return;
    }
    
    // 检查是否点击了 [测光指示图标] (右下角) -> 弹出 测光选择
    if (meterIndicatorRect.contains(clickPos)) {
        evPopup->hide();
        awbPopup->hide();
        isoPopup->hide();
        shutterPopup->hide();
        peakingPopup->hide();

        if (meterPopup->isHidden()) {
            int popupX = (width() - meterPopup->width()) / 2;
            int popupY = evBoxRect.top() - meterPopup->height() - 10;

            meterPopup->move(popupX, popupY);
            meterPopup->setSelection((int)currentMeteringMode); // 确保高亮当前项
            meterPopup->show();
            meterPopup->raise();
        }
        else {
            meterPopup->hide();
        }
        QMainWindow::mousePressEvent(event);
        return;
    }

    // 检查是否点击了 [ISO] 框或者 [S] 快门框
    if (currentMode == Mode_M || currentMode == Mode_S) { // 只有M或S档允许弹出
        if (isoBoxRect.contains(clickPos)) {
            evPopup->hide(); meterPopup->hide(); awbPopup->hide(); shutterPopup->hide(); peakingPopup->hide();
            if (isoPopup->isHidden()) {
                int popupX = qBound(10, isoBoxRect.center().x() - isoPopup->width() / 2, width() - isoPopup->width() - 10);
                int popupY = isoBoxRect.top() - isoPopup->height() - 10;
                isoPopup->move(popupX, popupY);
                isoPopup->setIso(m_manualIso);
                isoPopup->show();
                isoPopup->raise();
            } else {
                isoPopup->hide();
            }
            return;
        }

        if (shutterBoxRect.contains(clickPos)) {
            evPopup->hide(); meterPopup->hide(); awbPopup->hide(); isoPopup->hide(); peakingPopup->hide();
            if (shutterPopup->isHidden()) {
                int popupX = qBound(10, shutterBoxRect.center().x() - shutterPopup->width() / 2, width() - shutterPopup->width() - 10);
                int popupY = shutterBoxRect.top() - shutterPopup->height() - 10;
                shutterPopup->move(popupX, popupY);
                if (m_isVideoMode) {
                    shutterPopup->setMaxShutter(1000000 / m_currentVideoFps);
                } else {
                    shutterPopup->setMaxShutter(100000000);
                }
                shutterPopup->setShutter(m_manualShutterUs);
                shutterPopup->show();
                shutterPopup->raise();
            } else {
                shutterPopup->hide();
            }
            return;
        }
    }

    // 点击其他空白区域 -> 关闭所有弹窗
    bool clickOnEv      = evPopup->isVisible()      && evPopup->geometry().contains(clickPos);
    bool clickOnMeter   = meterPopup->isVisible()   && meterPopup->geometry().contains(clickPos);
    bool clickOnAwb     = awbPopup->isVisible()     && awbPopup->geometry().contains(clickPos);
    bool clickOnIso     = isoPopup->isVisible()     && isoPopup->geometry().contains(clickPos);
    bool clickOnShutter = shutterPopup->isVisible() && shutterPopup->geometry().contains(clickPos);
    bool clickOnPeaking = peakingPopup->isVisible() && peakingPopup->geometry().contains(clickPos);

    // 检查是否点击了峰值对焦图标
    if (peakingIndicatorRect.contains(clickPos)) {
        evPopup->hide(); meterPopup->hide(); awbPopup->hide();
        isoPopup->hide(); shutterPopup->hide();
        if (peakingPopup->isHidden()) {
            int popupX = qBound(10, peakingIndicatorRect.left(), width() - peakingPopup->width() - 10);
            int popupY = peakingIndicatorRect.top() - peakingPopup->height() - 10;
            if (popupY < 5) popupY = peakingIndicatorRect.bottom() + 10; // 防止超出屏幕上边缘
            peakingPopup->move(popupX, popupY);
            peakingPopup->setSelection(m_peakingEnabled, m_peakingColor, m_peakingLevel);
            peakingPopup->show();
            peakingPopup->raise();
        } else {
            peakingPopup->hide();
        }
        return;
    }

    // 检查是否点击了闪光灯图标 (切换开/关)
    if (flashIndicatorRect.contains(clickPos)) {
        m_flashEnabled = !m_flashEnabled;
        qDebug() << "MainWindow: Flash" << (m_flashEnabled ? "ON" : "OFF");
        // 闪光灯与硬件触发引脚已移交 PioTriggerController，不再需要手动拉低
        if (!m_flashEnabled) {
            m_flashGpioActive.store(false);
        }
        return;
    }

    if (!clickOnEv && !clickOnMeter && !clickOnAwb && !clickOnIso && !clickOnShutter && !clickOnPeaking) {
        evPopup->hide();
        meterPopup->hide();
        awbPopup->hide();
        isoPopup->hide();
        shutterPopup->hide();
        peakingPopup->hide();
    }

    // 保持父类处理
    QMainWindow::mousePressEvent(event);
}

// 峰值对焦图标前向声明（定义位于文件末尾）
static void drawPeakingIcon(QPainter& p, int x, int y, bool enabled, int color);
static void drawFlashIcon(QPainter& p, int x, int y, bool enabled);

// 绘制P、M、S模式图标
void drawModeIndicator(QPainter& p, int x, int y, MainWindow::ShootMode mode) {
    QString text;
    switch (mode) {
    case MainWindow::Mode_Auto:
        text = "P";
        break;
    case MainWindow::Mode_S:
        text = "S";
        break;
    case MainWindow::Mode_M:
        text = "M";
        break;
    case MainWindow::Mode_Empty1: text = "C1"; break;
    case MainWindow::Mode_Empty2: text = "C2"; break;
    case MainWindow::Mode_Empty3: text = "C3"; break;
    case MainWindow::Mode_Empty4: text = "C4"; break;
    case MainWindow::Mode_Empty5: text = "C5"; break;
    case MainWindow::Mode_Empty6: text = "C6"; break;
    case MainWindow::Mode_Empty7: text = "C7"; break;
    default:
        text = "U";
        break;
    }

    int fontSize = (text.length() > 1) ? 18 : 36;
    QFont modeFont("Arial", fontSize);
    modeFont.setBold(true);
    p.setFont(modeFont);
    int size = 50;
    QFontMetrics fm(modeFont);
    int textWidth = fm.horizontalAdvance(text);
    int textX = x + (size - textWidth) / 2;
    int textY = y + (size + fm.ascent() - fm.descent()) / 2;
    QPainterPath path;
    path.addText(textX, textY, modeFont, text);
    p.setPen(QPen(Qt::black, 1));
    p.setBrush(Qt::white);
    p.drawPath(path);
}

//绘制快门速度、感光度标签
QRect drawParamBox(QPainter& p, int x, int y, QString title, QString value) {
    int boxWidth = 160;
    int boxHeight = 32;
    int radius = 3;
    QRect rect(x, y, boxWidth, boxHeight);
    p.setBrush(QColor(0, 0, 0, 0));
    p.setPen(QPen(Qt::black, 2));
    p.drawRoundedRect(rect, radius, radius);
    p.setPen(QPen(Qt::white, 1));
    p.drawRoundedRect(rect, radius, radius);

    // 绘制小标题 (如 "ISO") 带描边
    {
        QFont titleFont("Arial", 20);
        titleFont.setBold(true);
        p.setFont(titleFont);
        QFontMetrics fm(titleFont);
        int titleW = fm.horizontalAdvance(title);
        int titleX = x + 25 - titleW / 2;
        int titleY = y + 0 + fm.ascent();
        QPainterPath titlePath;
        titlePath.addText(titleX, titleY, titleFont, title);
        p.setPen(QPen(Qt::black, 1)); 
        p.setBrush(Qt::white);
        p.drawPath(titlePath);
    }

    // 绘制数值 (如 "1/100") 带描边
    {
        int fontSize = 25;
        QFont valFont("Arial", fontSize);
        valFont.setBold(true);
        QFontMetrics fm(valFont);
        
        while (fm.horizontalAdvance(value) > 95 && fontSize > 14) {
            fontSize--;
            valFont.setPointSize(fontSize);
            fm = QFontMetrics(valFont);
        }

        p.setFont(valFont);
        int textWidth = fm.horizontalAdvance(value);
        int textX = x + 30 + (boxWidth - textWidth) / 2;
        if (textX + textWidth > x + boxWidth - 5) {
            textX = x + boxWidth - textWidth - 5;
        }

        int textY = y + 7 + (20 + fm.ascent() - fm.descent()) / 2;

        QPainterPath valPath;
        valPath.addText(textX, textY, valFont, value);
        p.setPen(QPen(Qt::black, 1));
        if (title == "EV" && value != "+0.0") {
            p.setBrush(QColor(255, 204, 0)); 
        }
        else {
            p.setBrush(Qt::white);
        }

        p.drawPath(valPath);
    }

    return rect;
}

//绘制测光图标
static void drawMeteringIcon(QPainter& p, int x, int y, MainWindow::MeteringMode mode) {
    int w = 80;
    int h = 60;
    QRect rect(x, y, w, h);
    p.setBrush(QColor(0, 0, 0, 0));
    p.setPen(QPen(Qt::black, 3));
    p.drawRoundedRect(rect, 8, 8);
    p.setPen(QPen(Qt::white, 2));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(rect, 8, 8);
    QPainterPath path;
    int cx = x + w / 2; 
    int cy = y + h / 2; 
    switch (mode) {
    case MainWindow::Meter_Matrix:
    {
        int r = 16;
        path.addRect(cx - r, cy - r, 2 * r, 2 * r);
        path.moveTo(cx, cy - r); path.lineTo(cx, cy + r);
        path.moveTo(cx - r, cy); path.lineTo(cx + r, cy);
    }
    break;

    case MainWindow::Meter_Center:
    {
        path.addEllipse(QPoint(cx, cy), 12, 12);
        path.moveTo(cx - 20, cy - 10);
        path.quadTo(cx - 24, cy, cx - 20, cy + 10);
        path.moveTo(cx + 20, cy - 10);
        path.quadTo(cx + 24, cy, cx + 20, cy + 10);
    }
    break;

    case MainWindow::Meter_Spot:
    {
        path.addEllipse(QPoint(cx, cy), 6, 6);
        int r = 16;
        int len = 6;
        // 四个角的瞄准线
        path.moveTo(cx - r, cy - r); path.lineTo(cx - r + len, cy - r); // 左上横
        path.moveTo(cx - r, cy - r); path.lineTo(cx - r, cy - r + len); // 左上竖

        path.moveTo(cx + r, cy - r); path.lineTo(cx + r - len, cy - r); // 右上横
        path.moveTo(cx + r, cy - r); path.lineTo(cx + r, cy - r + len); // 右上竖

        path.moveTo(cx - r, cy + r); path.lineTo(cx - r + len, cy + r); // 左下横
        path.moveTo(cx - r, cy + r); path.lineTo(cx - r, cy + r - len); // 左下竖

        path.moveTo(cx + r, cy + r); path.lineTo(cx + r - len, cy + r); // 右下横
        path.moveTo(cx + r, cy + r); path.lineTo(cx + r, cy + r - len); // 右下竖
    }
    break;
    }

    p.setPen(QPen(Qt::black, 4));
    p.setBrush(mode == MainWindow::Meter_Spot ? QBrush(Qt::white) : QBrush(Qt::NoBrush));
    p.drawPath(path);
    p.setPen(QPen(Qt::white, 2));
    p.setBrush(mode == MainWindow::Meter_Spot ? QBrush(Qt::white) : QBrush(Qt::NoBrush));
    p.drawPath(path);
}

// 绘制AWB图标
static void drawAwbIcon(QPainter& p, int x, int y, bool isAuto, int colorTemp) {
    int w = 80;
    int h = 60;
    QRect rect(x, y, w, h);

    p.setBrush(QColor(0, 0, 0, 0));

    p.setPen(QPen(Qt::black, 3));
    p.drawRoundedRect(rect, 8, 8);

    p.setPen(QPen(Qt::white, 2));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(rect, 8, 8);

    QFont fontTop("Arial", 18, QFont::Bold);
    p.setFont(fontTop);
    QString textTop = "AWB";
    QFontMetrics fmTop(fontTop);
    int topX = x + (w - fmTop.horizontalAdvance(textTop)) / 2;
    int topY = y + 25; 

    QPainterPath pathTop;
    pathTop.addText(topX, topY, fontTop, textTop);

    p.setPen(QPen(Qt::black, 1));
    p.setBrush(Qt::white);
    p.drawPath(pathTop);

    QFont fontBot("Arial", 16, QFont::Bold);
    p.setFont(fontBot);
    QString textBot = isAuto ? "AUTO" : QString("%1K").arg(colorTemp);
    QFontMetrics fmBot(fontBot);
    int botX = x + (w - fmBot.horizontalAdvance(textBot)) / 2;
    int botY = y + 48; 

    QPainterPath pathBot;
    pathBot.addText(botX, botY, fontBot, textBot);

    p.setPen(QPen(Qt::black, 1));
    p.setBrush(Qt::white);
    p.drawPath(pathBot);
}

// 绘制屏幕中心的测光指示框
static void drawMeteringGuide(QPainter& p, int screenW, int screenH, MainWindow::MeteringMode mode) {
    if (mode == MainWindow::Meter_Matrix) return;

    int cx = screenW / 2;
    int cy = screenH / 2;

    QPen penBlack(Qt::black, 3);
    QPen penWhite(Qt::white, 1);
    p.setBrush(Qt::NoBrush);

    if (mode == MainWindow::Meter_Spot) {
        int radius = 20;
        p.setPen(penBlack);
        p.drawEllipse(QPoint(cx, cy), radius, radius);
        p.setPen(penWhite);
        p.drawEllipse(QPoint(cx, cy), radius, radius);
    }
    else if (mode == MainWindow::Meter_Center) {
        // 绘制中央重点
        int w = screenW * 0.25;
        int h = screenH * 0.25;
        QRect rect(cx - w / 2, cy - h / 2, w, h);

        int r = 20;
        QPainterPath path;
        path.moveTo(rect.left() + r, rect.top());
        path.quadTo(rect.topLeft(), QPoint(rect.left(), rect.top() + r));
        path.lineTo(rect.left(), rect.bottom() - r);
        path.quadTo(rect.bottomLeft(), QPoint(rect.left() + r, rect.bottom()));

        path.moveTo(rect.right() - r, rect.top());
        path.quadTo(rect.topRight(), QPoint(rect.right(), rect.top() + r));
        path.lineTo(rect.right(), rect.bottom() - r);
        path.quadTo(rect.bottomRight(), QPoint(rect.right() - r, rect.bottom()));

        p.setPen(penBlack);
        p.drawPath(path);
        p.setPen(penWhite);
        p.drawPath(path);
    }
}
// 计算并绘制直方图, 返回计算得到的平均亮度 (0-255)
int drawHistogram(QPainter& p, int x, int y, const QImage& img, MainWindow::MeteringMode mode) {
    int width = 120;  
    int height = 60;   
    int radius = 8;
    p.setBrush(QColor(0, 0, 0, 100));
    p.setPen(QPen(QColor(255, 255, 255, 150), 1));
    p.drawRoundedRect(x, y, width, height, radius, radius);
    if (img.isNull()) return 118;
    int bins[256] = { 0 };
    int maxCount = 0;
    long long totalY = 0;
    int pixelCount = 0;

    // 不遍历每一个像素，跳样采样
    int step = 4;

    const uchar* bits = img.constBits();
    int bytesPerLine = img.bytesPerLine();
    int depth = img.depth() / 8;

    // 确定测光区域
    int meterX1 = 0, meterY1 = 0, meterX2 = img.width() - 1, meterY2 = img.height() - 1;
    if (mode == MainWindow::Meter_Center) {
        // 中央重点：取画面中间的 25% 面积 (长宽各 1/2)
        int cx = img.width() / 2;
        int cy = img.height() / 2;
        int w = img.width() / 2;
        int h = img.height() / 2;
        meterX1 = cx - w / 2;
        meterX2 = cx + w / 2;
        meterY1 = cy - h / 2;
        meterY2 = cy + h / 2;
    } else if (mode == MainWindow::Meter_Spot) {
        // 点测光：取画面中间极小区域 (比如 40x40)
        int cx = img.width() / 2;
        int cy = img.height() / 2;
        meterX1 = std::max(0, cx - 20);
        meterX2 = std::min(img.width() - 1, cx + 20);
        meterY1 = std::max(0, cy - 20);
        meterY2 = std::min(img.height() - 1, cy + 20);
    }

    for (int r = 0; r < img.height(); r += step) {
        const uchar* line = bits + r * bytesPerLine;
        for (int c = 0; c < img.width(); c += step) {
            int offset = c * depth;
            int b = line[offset];
            int g = line[offset + 1];
            int r_val = line[offset + 2];
            int yVal = (r_val * 77 + g * 150 + b * 29) >> 8;
            if (yVal < 0) yVal = 0;
            if (yVal > 255) yVal = 255;
            bins[yVal]++;
            if (c >= meterX1 && c <= meterX2 && r >= meterY1 && r <= meterY2) {
                totalY += yVal;
                pixelCount++;
            }
        }
    }

    for (int i = 1; i < 255; ++i) {
        if (bins[i] > maxCount) {
            maxCount = bins[i];
        }
    }

    // 绘制波形图
    if (maxCount == 0) maxCount = 1; // 防止除以0
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(255, 255, 255, 200));
    QPainterPath path;
    path.moveTo(x, y + height - 5);
    for (int i = 0; i < width; ++i) {
        int startBin = (i * 256) / width;
        int endBin = ((i + 1) * 256) / width;
        int val = 0;
        for (int b = startBin; b < endBin; ++b) {
            if (bins[b] > val) {
                val = bins[b];
            }
        }
        int barHeight = (val * (height - 10)) / maxCount;
        if (barHeight > height - 10) {
            barHeight = height - 10; 
        }
        path.lineTo(x + i, y + height - 5 - barHeight);
    }
    path.lineTo(x + width, y + height - 5);
    path.lineTo(x, y + height - 5);

    p.drawPath(path);

    int avgY = pixelCount > 0 ? (int)(totalY / pixelCount) : 118;
    return avgY;
}

void MainWindow::onModeSwitched(bool isVideo) {
    m_isVideoMode = isVideo;

    if (pioController) {
        pioController->startPreview(getTargetFps(), m_manualShutterUs);
    }

    if (m_isVideoMode) {
        // 进入视频模式：UI 切换
        btnPlayback->hide();
        m_videoHud->show();
        m_videoHud->raise();
    }
    else {
        // 切回拍照模式
        if (m_videoRecorder->isRecording()) {
            m_videoRecorder->stopRecording(); // 停止录像
        }
        m_videoHud->hide();
        btnPlayback->show();
    }
}

void MainWindow::onDriveModeChanged(int modeIndex) {
    if (modeIndex >= 0 && modeIndex <= 9) {
        currentMode = static_cast<ShootMode>(modeIndex);
        engine->setDriveMode(modeIndex);
        
        if (currentMode != Mode_M && currentMode != Mode_S) {
            isoPopup->hide();
            shutterPopup->hide();
        }
    }
}

void MainWindow::onIsoChanged(int iso) {
    m_manualIso = iso;
    engine->setManualExposure(m_manualShutterUs, m_manualIso);
}

void MainWindow::onShutterChanged(int shutterUs) {
    m_manualShutterUs = shutterUs;
    engine->setManualExposure(m_manualShutterUs, m_manualIso);
    if (pioController) {
        pioController->startPreview(getTargetFps(), m_manualShutterUs);
    }
}

void MainWindow::showVariableFpsUi() {
    if (!m_variableFpsWidget) {
        m_variableFpsWidget = new VariableFpsWidget(this);
        connect(m_variableFpsWidget, &VariableFpsWidget::stepRequested, this, &MainWindow::changeVariableFps);
    }
    
    // Center it
    m_variableFpsWidget->move((width() - m_variableFpsWidget->width()) / 2,
                              (height() - m_variableFpsWidget->height()) / 2);
    m_variableFpsWidget->setFps(m_currentVideoFps);
    m_variableFpsWidget->show();
    m_variableFpsWidget->raise();
}

void MainWindow::changeVariableFps(float delta) {
    if (m_videoRecorder->isRecording()) return;

    float newFps = m_currentVideoFps + delta;
    if (newFps < 1.0f) newFps = 1.0f;
    if (newFps > 60.0f) newFps = 60.0f;
    
    m_currentVideoFps = newFps;
    if (m_variableFpsWidget) m_variableFpsWidget->setFps(m_currentVideoFps);
    
    // 更新右上角 HUD 显示的帧率
    if (m_videoHud) {
        if (m_currentVideoFps == (int)m_currentVideoFps) {
            m_videoHud->setResFpsText(QString("%1p").arg((int)m_currentVideoFps));
        } else {
            m_videoHud->setResFpsText(QString("%1p").arg(m_currentVideoFps, 0, 'f', 3));
        }
    }
    
    engine->setFps(m_currentVideoFps);
    m_videoRecorder->setFps(m_currentVideoFps);
    if (pioController) {
        pioController->startPreview(m_currentVideoFps, m_manualShutterUs);
    }
}

void MainWindow::onJoystickUp() {
    if (m_variableFpsWidget && m_variableFpsWidget->isVisible()) {
        changeVariableFps(1.0f);
    }
}

void MainWindow::onJoystickDown() {
    if (m_variableFpsWidget && m_variableFpsWidget->isVisible()) {
        changeVariableFps(-1.0f);
    }
}

void MainWindow::onJoystickLeft() {
    if (m_variableFpsWidget && m_variableFpsWidget->isVisible()) {
        changeVariableFps(-0.1f);
    }
}

void MainWindow::onJoystickRight() {
    if (m_variableFpsWidget && m_variableFpsWidget->isVisible()) {
        changeVariableFps(0.1f);
    }
}

void MainWindow::onJoystickCenter() {
    if (m_variableFpsWidget && m_variableFpsWidget->isVisible()) {
        m_variableFpsWidget->hide();
    }
}

void MainWindow::onRotaryEncoderScroll(int delta, bool switchPressed) {
    if (m_variableFpsWidget && m_variableFpsWidget->isVisible()) {
        m_variableFpsWidget->setFineTuneMode(switchPressed);
        float step = switchPressed ? 0.001f : 0.01f;
        changeVariableFps(delta * step);
        return;
    }
    
    // 旋钛控制快门/ISO
    if (switchPressed) {
        isoPopup->changeIso(delta);
    } else {
        if (m_isVideoMode) {
            shutterPopup->setMaxShutter(1000000 / m_currentVideoFps);
        } else {
            shutterPopup->setMaxShutter(100000000);
        }
        shutterPopup->changeShutter(delta);
    }
}

//更新画面
void MainWindow::updateFrame(QImage image, double shutterTimeUs, double analogGain, float targetEv)  {
    m_lastShutterUs = shutterTimeUs;
    m_lastIso = (int)(analogGain * 100);
    
    // 保存当前 EV 值供弹窗使用
    lastEv = targetEv;

    // 如果开启了峰值对焦，先在低分辨率 (640x480) 原图上处理
    QImage processImg = image;
    if (m_peakingEnabled) {
        if (processImg.format() != QImage::Format_RGB888) {
            processImg = processImg.convertToFormat(QImage::Format_RGB888);
        } else {
            processImg = processImg.copy();
        }
        applyFocusPeaking(processImg, m_peakingColor, m_peakingLevel);
    }

    // 将处理后（或原图）缩放到屏幕大小
    Qt::TransformationMode scaleMode = (m_isVideoMode && m_videoRecorder->isRecording()) 
                                        ? Qt::FastTransformation 
                                        : Qt::SmoothTransformation;

    QImage scaledImg = processImg.scaled(
        viewfinder->size(),
        Qt::KeepAspectRatio,
        scaleMode
    );

    // 转换为 QPixmap（仅一次 CPU->GPU 上传）并开始绘制 HUD
    QPixmap pixmap = QPixmap::fromImage(scaledImg);
    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing); // 抗锯齿，让圆角和文字平滑

    // 绘制参考线 
    if (m_gridLineMode != 0) {
        p.setPen(QPen(QColor(255, 255, 255, 128), 1)); // 半透明白线
        int w = pixmap.width();
        int h = pixmap.height();
        
        if (m_gridLineMode == 1 || m_gridLineMode == 3) {
            // Rule of 3rds
            p.drawLine(w / 3, 0, w / 3, h);
            p.drawLine(2 * w / 3, 0, 2 * w / 3, h);
            p.drawLine(0, h / 3, w, h / 3);
            p.drawLine(0, 2 * h / 3, w, 2 * h / 3);
        }
        if (m_gridLineMode == 2) {
            // Square (1:1 crop lines)
            int squareW = h; 
            int offsetX = (w - squareW) / 2;
            p.drawLine(offsetX, 0, offsetX, h);
            p.drawLine(offsetX + squareW, 0, offsetX + squareW, h);
        }
        if (m_gridLineMode == 3) {
            // Center Cross
            int cx = w / 2;
            int cy = h / 2;
            int crossLen = 20;
            p.drawLine(cx - crossLen, cy, cx + crossLen, cy);
            p.drawLine(cx, cy - crossLen, cx, cy + crossLen);
        }
    }

    // 数据格式化
    int iso = (int)(analogGain * 100);
    QString isoStr = QString::number(iso);
    QString shutterStr;
    if (shutterTimeUs <= 0) {
        shutterStr = "AUTO";
    }
    else if (shutterTimeUs >= 1000000) {
        shutterStr = QString("%1\"").arg(shutterTimeUs / 1000000.0, 0, 'f', 1);
    }
    else {
        // 快门倒数显示
        int den = qRound(1000000.0 / shutterTimeUs);
        shutterStr = QString("1/%1").arg(den);
    }
    
    // M/S 档位优先显示设定参数
    if (currentMode == Mode_M || currentMode == Mode_S) {
        if (m_manualIso > 0) isoStr = QString::number(m_manualIso);
        else isoStr = QString::number((int)(analogGain * 100));

        static const std::vector<int> sList = {
            30, 31, 40, 50, 62, 78, 100, 125, 156, 200, 250, 312, 400, 500, 625, 800, 1000,
            1250, 1562, 2000, 2500, 3125, 4000, 5000, 6250, 8000, 10000, 12500, 16666,
            20000, 25000, 33333, 40000, 50000, 66666, 76923, 100000, 125000, 166666,
            200000, 250000, 333333, 400000, 500000, 600000, 800000, 1000000, 1300000,
            1600000, 2000000, 2500000, 3200000, 4000000, 5000000, 6000000, 8000000,
            10000000, 13000000, 15500000
        };
        static const std::vector<QString> sStrs = {
            "1/33000", "1/32000", "1/25000", "1/20000", "1/16000", "1/12800", "1/10000", "1/8000",
            "1/6400", "1/5000", "1/4000", "1/3200", "1/2500", "1/2000", "1/1600",
            "1/1250", "1/1000", "1/800", "1/640", "1/500", "1/400", "1/320", "1/250",
            "1/200", "1/160", "1/125", "1/100", "1/80", "1/60", "1/50", "1/40",
            "1/30", "1/25", "1/20", "1/15", "1/13", "1/10", "1/8", "1/6", "1/5",
            "1/4", "1/3", "0.4\"", "0.5\"", "0.6\"", "0.8\"", "1\"", "1.3\"",
            "1.6\"", "2\"", "2.5\"", "3.2\"", "4\"", "5\"", "6\"", "8\"",
            "10\"", "13\"", "15.5\""
        };
        
        auto it = std::find(sList.begin(), sList.end(), m_manualShutterUs);
        if (it != sList.end()) {
            shutterStr = sStrs[std::distance(sList.begin(), it)];
        } else {
            if (m_manualShutterUs < 1000000) {
                shutterStr = QString("1/%1").arg(1000000 / m_manualShutterUs);
            } else {
                shutterStr = QString("%1\"").arg((float)m_manualShutterUs / 1000000.0f, 0, 'f', 1);
            }
        }
    }

    QString evStr = QString::asprintf("%+.1f", targetEv);

    int startY = pixmap.height() - 35;
    int centerX = pixmap.width() / 2;
    int gap = 8;
    int boxW = 160;
    int startX = centerX - 60 - (3 * boxW + 2 * gap) / 2;

    int iconSize = 36;
    int iconGap = 15;
    int totalIconW = 3 * iconSize + 2 * iconGap;
    int iconsY = startY - 10 - iconSize;
    int iconsStartX = (pixmap.width() - totalIconW) / 2;

    int viewX = (viewfinder->width() - pixmap.width()) / 2;
    int viewY = (viewfinder->height() - pixmap.height()) / 2;
    QRect sRectOnPixmap = drawParamBox(p, startX, startY, "S", shutterStr);
    shutterBoxRect = sRectOnPixmap.translated(viewX, viewY);
    QRect isoRectOnPixmap = drawParamBox(p, startX + boxW + gap, startY, "ISO", isoStr);
    isoBoxRect = isoRectOnPixmap.translated(viewX, viewY);

    //直方图并获取平均亮度
    int histW = 120;
    int histH = 60;
    int histX = pixmap.width() - histW - 4;
    int histY = pixmap.height() - histH - 2;
    int avgY = drawHistogram(p, histX, histY, image, currentMeteringMode);

    engine->updateLuma(avgY, analogGain * 100.0f);

    // M模式 + 非 Auto ISO 的真正手动档，使用 Histogram 计算出来的 EV 作为测光表
    if (currentMode == Mode_M && m_manualIso != 0) {
        float evOffset = 2.2f * std::log2(avgY / 118.0f);
        if (evOffset > 3.0f) evOffset = 3.0f;
        if (evOffset < -3.0f) evOffset = -3.0f;
        evStr = QString::asprintf("%+.1f", evOffset);
    }

    //画 EV
    QRect rectOnPixmap = drawParamBox(p, startX + 2 * boxW + 2 * gap, startY, "EV", evStr);
    evBoxRect = rectOnPixmap.translated(viewX, viewY);

    // 代理丢帧警告
    if (m_isVideoMode && m_videoRecorder->isRecording() && m_videoRecorder->currentCodec() == VideoRecorder::CODEC_CINEMADNG) {
        auto now = std::chrono::steady_clock::now().time_since_epoch();
        int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
        if (nowMs - m_videoRecorder->getLastProxyDropTimeMs() < 1000) { // 显示1秒钟
            QFont warnFont("Arial", 16, QFont::Bold);
            p.setFont(warnFont);
            QString warnText = "代理丢帧";
            QFontMetrics fm(warnFont);
            int txtW = fm.horizontalAdvance(warnText);
            int paramsCenterX = startX + (3 * boxW + 2 * gap) / 2;
            int txtX = paramsCenterX - txtW / 2;
            int txtY = startY - 10;
            QPainterPath path;
            path.addText(txtX, txtY, warnFont, warnText);
            p.setPen(QPen(Qt::black, 1));
            p.setBrush(QColor(255, 204, 0));
            p.drawPath(path);
        }
    }

    //绘制拍摄模式指示标签
    drawModeIndicator(p, 2, 2, currentMode);
    int iconX = pixmap.width() - 83;
    int iconY = pixmap.height() - 60 - 2 - 20 - 300;

    int peakIconX = iconX;
    int peakIconY = iconY + 60 + 8;
    int flashIconX = peakIconX;
    int flashIconY = peakIconY + 60 + 8;
    int awbIconX = 3; 
    int awbIconY = iconY; 

    if (!m_uiHidden) {
        drawMeteringIcon(p, iconX, iconY, currentMeteringMode);
        drawMeteringGuide(p, pixmap.width(), pixmap.height(), currentMeteringMode);
        QRect meterRect(iconX, iconY, 80, 60);
        meterIndicatorRect = meterRect.translated(viewX, viewY);

        // 峰值对焦图标
        drawPeakingIcon(p, peakIconX, peakIconY, m_peakingEnabled, m_peakingColor);
        peakingIndicatorRect = QRect(peakIconX, peakIconY, 80, 60).translated(viewX, viewY);

        // 闪光灯图标
        if (!m_isVideoMode) {
            drawFlashIcon(p, flashIconX, flashIconY, m_flashEnabled);
            flashIndicatorRect = QRect(flashIconX, flashIconY, 80, 60).translated(viewX, viewY);
        } else {
            flashIndicatorRect = QRect();
        }

        // 绘制 AWB 图标
        drawAwbIcon(p, awbIconX, awbIconY, lastIsAutoAwb, lastColorTemp);
        QRect awbRect(awbIconX, awbIconY, 80, 60);
        awbIndicatorRect = awbRect.translated(viewX, viewY);
    } else {
        meterIndicatorRect = QRect();
        peakingIndicatorRect = QRect();
        flashIndicatorRect = QRect();
        awbIndicatorRect = QRect();
    }
    int pbX = pixmap.width() - 80 - 3;
    int pbY = pixmap.height() - 60 - 10;
    pbY = pixmap.height() - 60 - 10 - 70;
    btnPlayback->move(pbX + viewX, pbY + viewY);
    if (!m_isVideoMode && !m_uiHidden) {
        if (m_albumWidget->isHidden()) {
            btnPlayback->show();
        }
        else {
            btnPlayback->hide();
        }
    } else {
        btnPlayback->hide();
    }

    if (!m_isVideoMode) {
        drawPhotoFormatLabel(p, viewX, viewY);
    }
    p.end();

    if (m_isVideoMode) {
        static uint32_t storageTick = 0;
        static QString cachedStorage = "128G";
        if (storageTick++ % 60 == 0) {
            cachedStorage = m_videoRecorder->getRemainingStorageGB();
        }
        m_videoHud->setStorageText(cachedStorage);

        if (m_videoRecorder->isRecording()) {
            m_videoHud->setRecordingState(true);
        } else {
            m_videoHud->setRecordingState(false);
        }
        m_videoHud->setTimecode(m_videoRecorder->getRecordingTimecode());
        m_videoHud->setMemoryPoolStatus(m_videoRecorder->getMemoryPoolStatus());
    }

    // 显示最终画面
    viewfinder->setPixmap(pixmap);
}

// 峰值对焦图标绘制
static void drawPeakingIcon(QPainter& p, int x, int y, bool enabled, int color) {
    int w = 80, h = 60;
    QRect rect(x, y, w, h);

    // 轮廓框
    p.setBrush(QColor(0, 0, 0, 0));
    p.setPen(QPen(Qt::black, 3));
    p.drawRoundedRect(rect, 8, 8);
    p.setPen(QPen(Qt::white, 2));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(rect, 8, 8);

    // "PEAK" 标题文字
    QFont fTop("Arial", 18, QFont::Bold);
    p.setFont(fTop);
    QFontMetrics fmT(fTop);
    QString topTxt = "PEAK";
    int topX = x + (w - fmT.horizontalAdvance(topTxt)) / 2;
    int topY = y + 25;
    QPainterPath ppTop;
    ppTop.addText(topX, topY, fTop, topTxt);
    p.setPen(QPen(Qt::black, 1));
    p.setBrush(Qt::white);
    p.drawPath(ppTop);

    // 底部状态指示：开启时展示当前颜色圆点，关闭时白色 "OFF"
    if (enabled) {
        QColor dotColor;
        switch (color) {
            case 0: dotColor = QColor(255, 60, 60);  break; // 红
            case 2: dotColor = QColor(80, 160, 255); break; // 蓝
            default: dotColor = QColor(60, 220, 80); break; // 绿
        }
        p.setBrush(dotColor);
        p.setPen(QPen(Qt::black, 1.5));
        p.drawEllipse(x + w / 2 - 9, y + 36, 18, 13);
    } else {
        QFont fBot("Arial", 16, QFont::Bold);
        p.setFont(fBot);
        QFontMetrics fmB(fBot);
        QString botTxt = "OFF";
        int botX = x + (w - fmB.horizontalAdvance(botTxt)) / 2;
        int botY = y + 50;
        QPainterPath ppBot;
        ppBot.addText(botX, botY, fBot, botTxt);
        p.setPen(QPen(Qt::black, 1));
        p.setBrush(Qt::white);
        p.drawPath(ppBot);
    }
}

// 闪光灯图标绘制
static void drawFlashIcon(QPainter& p, int x, int y, bool enabled) {
    int w = 80, h = 60;
    QRect rect(x, y, w, h);

    // 轮廓框
    p.setBrush(QColor(0, 0, 0, 0));
    p.setPen(QPen(Qt::black, 3));
    p.drawRoundedRect(rect, 8, 8);
    p.setPen(QPen(Qt::white, 2));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(rect, 8, 8);

    // 闪电图标
    int cx = x + w / 2;
    int cy = y + h / 2 - 3;
    QPainterPath bolt;
    bolt.moveTo(cx + 2,  cy - 14);
    bolt.lineTo(cx - 6,  cy + 2);
    bolt.lineTo(cx - 1,  cy + 2);
    bolt.lineTo(cx - 4,  cy + 14);
    bolt.lineTo(cx + 6,  cy - 2);
    bolt.lineTo(cx + 1,  cy - 2);
    bolt.closeSubpath();

    QColor boltColor = enabled ? QColor(255, 204, 0) : Qt::white;
    p.setPen(QPen(Qt::black, 2));
    p.setBrush(boltColor);
    p.drawPath(bolt);
    if (enabled) {
        p.setBrush(QColor(255, 204, 0));
        p.setPen(QPen(Qt::black, 1.5));
        p.drawEllipse(x + w / 2 - 9, y + 40, 18, 13);
    } else {
        QFont fBot("Arial", 16, QFont::Bold);
        p.setFont(fBot);
        QFontMetrics fmB(fBot);
        QString botTxt = "OFF";
        int botX = x + (w - fmB.horizontalAdvance(botTxt)) / 2;
        int botY = y + 54;
        QPainterPath ppBot;
        ppBot.addText(botX, botY, fBot, botTxt);
        p.setPen(QPen(Qt::black, 1));
        p.setBrush(Qt::white);
        p.drawPath(ppBot);
    }
}

// 极速滚动行缓冲峰值对焦算法
void MainWindow::applyFocusPeaking(QImage& img, int color, int level) {
    if (img.format() != QImage::Format_RGB888) {
        img = img.convertToFormat(QImage::Format_RGB888);
    }
    const int w = img.width();
    const int h = img.height();

    // 恢复 3x3 Sobel 的标准阈值
    int threshold;
    switch (level) {
        case 0:  threshold = 80; break; // LOW
        case 2:  threshold = 28; break; // HIGH
        default: threshold = 50; break; // MID
    }

    uint8_t pr, pg, pb;
    switch (color) {
        case 0: pr=255; pg=50;  pb=50;  break;
        case 2: pr=70;  pg=150; pb=255; break;
        default:pr=50;  pg=240; pb=80;  break;
    }

    int bytesPerLine = img.bytesPerLine();
    uint8_t* bits = img.bits();

    m_peakingFrameCount++;

    // 时域降频：每 3 帧进行一次全量边缘计算
    if (m_peakingFrameCount % 3 == 0) {
        m_peakingPoints.clear();
        if (m_peakingPoints.capacity() < 10000) {
            m_peakingPoints.reserve(10000);
        }

        // 使用 3 行滚动缓冲
        thread_local std::vector<uint8_t> ring_buf;
        ring_buf.resize(w * 3);
        uint8_t* L_prev = ring_buf.data();
        uint8_t* L_curr = ring_buf.data() + w;
        uint8_t* L_next = ring_buf.data() + w * 2;

        // 计算一整行的真实亮度
        auto calcLumaRow = [&](uint8_t* dst, int y) {
            const uint8_t* src = bits + y * bytesPerLine;
            for (int x = 0; x < w; ++x) {
                int off = x * 3;
                dst[x] = (src[off]*77 + src[off+1]*150 + src[off+2]*29) >> 8;
            }
        };

        if (h >= 2) {
            calcLumaRow(L_prev, 0);
            calcLumaRow(L_curr, 1);
        }

        for (int y = 1; y < h - 1; ++y) {
            calcLumaRow(L_next, y + 1);

            if (y % 2 != 0) {
                for (int x = 1; x < w - 1; x += 2) {
                    int gx = -L_prev[x - 1] + L_prev[x + 1]
                             - 2 * L_curr[x - 1] + 2 * L_curr[x + 1]
                             - L_next[x - 1] + L_next[x + 1];
                             
                    int gy = -L_prev[x - 1] - 2 * L_prev[x] - L_prev[x + 1]
                             + L_next[x - 1] + 2 * L_next[x] + L_next[x + 1];
                             
                    int g = (std::abs(gx) + std::abs(gy)) >> 3;

                    if (g >= threshold) {
                        m_peakingPoints.push_back({(int16_t)x, (int16_t)y});
                    }
                }
            }

            // 滚动行指针，复用内存
            uint8_t* temp = L_prev;
            L_prev = L_curr;
            L_curr = L_next;
            L_next = temp;
        }
    }

    // 在当前图像上绘制缓存的点阵
    for (const auto& pt : m_peakingPoints) {
        if (pt.y < h && pt.x < w) {
            int off = pt.y * bytesPerLine + pt.x * 3;
            bits[off]   = pr;
            bits[off+1] = pg;
            bits[off+2] = pb;
        }
    }
}

// 峰值对焦状态回调槽
void MainWindow::onPeakingChanged(bool enabled, int color, int level) {
    m_peakingEnabled = enabled;
    m_peakingColor   = color;
    m_peakingLevel   = level;
}

// 拍照格式标签绘制
void MainWindow::drawPhotoFormatLabel(QPainter& p, int viewX, int viewY) {
    QString formatText = (m_photoFormat == Photo_DNG) ? "DNG" : "JPEG";
    QColor fillColor = m_isCapturing ? QColor(255, 165, 0) : Qt::white;
    QFont font("Arial", 20, QFont::Bold);
    p.setFont(font);
    QFontMetrics fm(font);
    int textX = 58;
    int textY = 3;
    QPainterPath path;
    path.addText(textX, textY + fm.ascent(), font, formatText);
    p.setPen(QPen(Qt::black, 1));
    p.setBrush(fillColor);
    p.drawPath(path);
    m_photoFormatRect = QRect(textX, textY,
                              fm.horizontalAdvance(formatText),
                              fm.height())
                        .translated(viewX, viewY);
}

// 拍照格式切换槽：DNG <-> JPEG 循环
void MainWindow::onPhotoFormatCycled() {
    m_photoFormat = (m_photoFormat == Photo_DNG) ? Photo_JPEG : Photo_DNG;
    qDebug() << "MainWindow: Photo format ->"
             << (m_photoFormat == Photo_DNG ? "DNG (rpicam-still -e dng)" : "JPEG (rpicam-still -e jpg)");
}

// 拍照槽
void MainWindow::onStillCapture() {
    if (m_isCapturing) {
        qDebug() << "MainWindow: onStillCapture() skipped - capture in progress";
        return;
    }
    if (m_isVideoMode) return;

    m_isCapturing = true;
    m_photoTimestamp = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");

    // 闪光灯与外部硬件触发控制
    auto prepareCaptureState = [this]() {
        {
            QMutexLocker locker(&m_captureMutex);
            m_rawCaptured = false;
            m_jpgCaptured = false;
            m_capturedRawData.clear();
            m_capturedJpgData.clear();
        }

        if (m_flashEnabled) {
            // 时间戳匹配模式
            m_captureArmed = true;
            m_photoCaptureSkipFrames = -1;
            qDebug() << "MainWindow: Timestamp Matcher ARMED for Flash capture.";
        } else if (m_photoFormat == Photo_DNG) {
            m_captureArmed = false;
            m_photoCaptureSkipFrames = 0;
        } else {
            m_captureArmed = false;
            m_photoCaptureSkipFrames = -1;
        }

        // 非闪光模式：不管是 DNG 还是 JPEG，都需要 ZSL 提取一份 QImage 来保存 JPEG
        if (!m_flashEnabled) {
            engine->capturePhoto(0);
            qDebug() << "MainWindow: ZSL capture triggered for non-flash photo.";
        }
    };


    if (pioController) {
        int flashPreDelay = m_flashEnabled ? std::max(2, m_flashPreDelay) : 0;
        pioController->triggerCapturePulse(m_manualShutterUs, flashPreDelay, prepareCaptureState);
        if (m_captureWatchdog) m_captureWatchdog->start(5000);
    } else {
        prepareCaptureState();
    }
}

void MainWindow::checkAndSaveSingleDng() {
    QMutexLocker locker(&m_captureMutex);
    
    if (m_photoFormat == Photo_DNG) {
        if (!m_rawCaptured || !m_jpgCaptured) return;
    } else {
        if (!m_jpgCaptured) return;
    }
    
    QByteArray rawToSave = std::move(m_capturedRawData);
    QByteArray jpgToSave = std::move(m_capturedJpgData);
    m_rawCaptured = false;
    m_jpgCaptured = false;
    m_captureArmed = false;
    QString timestamp = m_photoTimestamp;
    bool isDng = (m_photoFormat == Photo_DNG);
    float rGain = m_lastRGain;
    float bGain = m_lastBGain;
    double shutterUs = m_lastShutterUs;
    uint32_t iso = m_lastIso;
    locker.unlock();

    // 在后台线程执行实际拼装写入
    QtConcurrent::run([=, raw = std::move(rawToSave), jpg = std::move(jpgToSave)]() {
        QString savePath = VideoRecorder::getActiveStoragePath() + "/Photo";
        QDir().mkpath(savePath);
        
        if (isDng) {
            QString dngPath = QString("%1/IMG_%2.dng").arg(savePath, timestamp);
            QFile f(dngPath);
            if (f.open(QIODevice::WriteOnly)) {
                QByteArray prefix(reinterpret_cast<const char*>(PHOTO_DNG_PREFIX), PHOTO_DNG_PREFIX_SIZE);
                
                uint32_t magic = 0xDEADBEEF;
                int magicIdx = -1;
                for (int i = 0; i < prefix.size() - 3; ++i) {
                    if (std::memcmp(prefix.data() + i, &magic, 4) == 0) {
                        magicIdx = i;
                        break;
                    }
                }
                
                if (magicIdx != -1) {
                    uint32_t jpgSize = jpg.size();
                    std::memcpy(prefix.data() + magicIdx, &jpgSize, 4);
                } else {
                    qWarning() << "MainWindow: 0xDEADBEEF magic not found in DNG_PREFIX!";
                }
                
                // 动态注入 AWB 以消除偏色
                uint32_t rNum = 10000;
                uint32_t rDen = (uint32_t)(rGain * 10000.0f);
                uint32_t gNum = 10000;
                uint32_t gDen = 10000;
                uint32_t bNum = 10000;
                uint32_t bDen = (uint32_t)(bGain * 10000.0f);
                
                uint8_t* as_shot_ptr = reinterpret_cast<uint8_t*>(prefix.data()) + PHOTO_DNG_AS_SHOT_NEUTRAL_SUFFIX_OFFSET;
                memcpy(as_shot_ptr,      &rNum, 4); memcpy(as_shot_ptr + 4,  &rDen, 4);
                memcpy(as_shot_ptr + 8,  &gNum, 4); memcpy(as_shot_ptr + 12, &gDen, 4);
                memcpy(as_shot_ptr + 16, &bNum, 4); memcpy(as_shot_ptr + 20, &bDen, 4);
                
                #ifdef PHOTO_DNG_EXPOSURE_TIME_OFFSET
                uint32_t shutterNum = (uint32_t)shutterUs;
                uint32_t shutterDen = 1000000;
                memcpy(prefix.data() + PHOTO_DNG_EXPOSURE_TIME_OFFSET, &shutterNum, 4);
                memcpy(prefix.data() + PHOTO_DNG_EXPOSURE_TIME_OFFSET + 4, &shutterDen, 4);
                #endif

                #ifdef PHOTO_DNG_ISO_OFFSET
                uint32_t isoVal = iso;
                memcpy(prefix.data() + PHOTO_DNG_ISO_OFFSET, &isoVal, 4);
                #endif
                
                // 单文件合并写入：头部 + RAW + JPEG
                f.write(prefix);
                f.write(raw);
                f.write(jpg);
                f.close();
                qDebug() << "MainWindow: ZSL Embedded DNG saved ->" << dngPath << "| JPG size:" << jpg.size();
            }
        } else {
            // 纯 JPEG 模式
            QString jpgPath = QString("%1/IMG_%2.jpg").arg(savePath, timestamp);
            QFile f(jpgPath);
            if (f.open(QIODevice::WriteOnly)) {
                f.write(jpg);
                f.close();
                qDebug() << "MainWindow: ZSL JPG saved ->" << jpgPath;
            }
        }
        
        // 恢复快门状态
        QMetaObject::invokeMethod(this, [=]() {
            m_isCapturing = false;
        });
    });
}

void MainWindow::onMenuPressed() {
    if (m_settingsMenu->isVisible()) {
        m_settingsMenu->closeMenu();
    } else {
        // 先关闭可能打开的其他弹窗和相册
        evPopup->hide();
        meterPopup->hide();
        awbPopup->hide();
        isoPopup->hide();
        shutterPopup->hide();
        peakingPopup->hide();
        if (m_albumWidget->isVisible()) {
            m_albumWidget->hide();
        }

        m_settingsMenu->setGeometry(0, 0, this->width(), this->height());
        m_settingsMenu->openMenu();
        m_settingsMenu->raise();
    }
}

// 闪光延时自动校准
void MainWindow::startFlashCalibration() {
    if (m_calibState != CALIB_OFF) return;

    if (!m_flashEnabled) {
        m_calibOverlay->setText(QString::fromUtf8("请先开启闪光灯！\n点击闪电图标开启闪光。"));
        m_calibOverlay->show();
        m_calibOverlay->raise();
        QTimer::singleShot(3000, this, [this]() { m_calibOverlay->hide(); });
        return;
    }
    if (!pioController) {
        m_calibOverlay->setText("无硬件触发控制器\n无法校准。");
        m_calibOverlay->show();
        m_calibOverlay->raise();
        QTimer::singleShot(3000, this, [this]() { m_calibOverlay->hide(); });
        return;
    }
    if (m_isVideoMode) {
        m_calibOverlay->setText(QString::fromUtf8("请切换到拍照模式\n再开始校准。"));
        m_calibOverlay->show();
        m_calibOverlay->raise();
        QTimer::singleShot(3000, this, [this]() { m_calibOverlay->hide(); });
        return;
    }

    // 初始化校准状态
    m_calibState        = CALIB_BASELINE_WAIT;
    m_calibSavedIso     = m_manualIso;
    m_calibWaitTicks    = 0;
    int w = 700;
    int h = 300;
    m_calibOverlay->setGeometry((width() - w) / 2, (height() - h) / 2, w, h);

    m_calibInitDelay    = m_flashPreDelay; 
    m_calibTrialDelay   = m_calibInitDelay;
    m_calibSuccessCount = 0;
    m_calibFailedCount  = 0;
    m_calibTotalTries   = 0;
    engine->setManualExposure(m_manualShutterUs, 160);
    m_calibMaxLuma.store(0.0f, std::memory_order_relaxed);
    m_calibOverlay->setText(QString::fromUtf8("闪光校准初始化...\n锁定环境亮度中 (ISO 160)\n快门: 1/%1 s")
        .arg(m_manualShutterUs > 0 ? 1000000 / m_manualShutterUs : 0));
    m_calibOverlay->show();
    m_calibOverlay->raise();

    // 启动节拍定时器：每 200ms 触发一次
    m_calibTimer->start(200);

    qDebug() << "MainWindow: Flash calibration STARTED.";
}

void MainWindow::onCalibrationTick() {
    if (m_calibState == CALIB_OFF) { m_calibTimer->stop(); return; }

    // 全局超时保护
    if (m_calibState >= CALIB_SEARCH_LEADING) {
        if (m_calibTotalTries >= 300) {
            finishCalibration(false);
            return;
        }
        m_calibTotalTries++;
    }

    // 等待 ISO 160 生效
    if (m_calibState == CALIB_BASELINE_WAIT) {
        m_calibWaitTicks++;
        if (m_calibWaitTicks >= 4) {
            m_calibState = CALIB_BASELINE_COLLECT;
            m_calibMaxLuma.store(0.0f, std::memory_order_relaxed);
        }
        return;
    }

    // 采集 1s 左右的环境亮度作为基准
    if (m_calibState == CALIB_BASELINE_COLLECT) {
        m_calibBaseLuma = m_calibMaxLuma.load(std::memory_order_relaxed);
        if (m_calibBaseLuma < 10.0f) m_calibBaseLuma = 200.0f; 
        
        qDebug() << "MainWindow: Calib baseline collected:" << m_calibBaseLuma;
        m_calibState = CALIB_SEARCH_LEADING;
        
        m_calibMaxLuma.store(0.0f, std::memory_order_relaxed);
        m_calibLeadingEdge = -1;
        m_calibTrailingEdge = -1;
        m_calibTrialDelay = 0; 
        pioController->triggerCapturePulse(m_manualShutterUs, m_calibTrialDelay, nullptr);
        m_calibLastTestedDelay = m_calibTrialDelay;
        return;
    }

    float luma = m_calibMaxLuma.load(std::memory_order_acquire);
    bool flashDetected = (luma > m_calibBaseLuma * 1.2f);
    int testedDelay = m_calibLastTestedDelay;

    qDebug() << "MainWindow: Calib tick #" << m_calibTotalTries
             << " state=" << m_calibState
             << " testedDelay=" << testedDelay
             << " maxLuma=" << (int)luma
             << " base=" << (int)m_calibBaseLuma
             << " detected=" << flashDetected;

    m_calibMaxLuma.store(0.0f, std::memory_order_relaxed);

    if (m_calibTrialDelay > 1000) {
        if (m_calibState == CALIB_SEARCH_TRAILING) {
            m_calibTrailingEdge = 1000;
            m_calibBestDelay = (m_calibLeadingEdge + m_calibTrailingEdge) / 2;
            m_calibTrialDelay = m_calibBestDelay;
            m_calibSuccessCount = 0;
            m_calibWaitTicks = 0;
            m_calibState = CALIB_COOLDOWN;
            qDebug() << "MainWindow: Max delay reached. Forcing trailing edge to 1000us. True Center:" << m_calibBestDelay;
            return;
        } else {
            finishCalibration(false);
            return;
        }
    }
    if (m_calibTrialDelay < 0) m_calibTrialDelay = 0;

    // 寻找前沿
    if (m_calibState == CALIB_SEARCH_LEADING) {
        if (flashDetected) {
            m_calibLeadingEdge = testedDelay;
            m_calibState = CALIB_SEARCH_TRAILING;
            m_calibTrialDelay = testedDelay + 10;
            qDebug() << "MainWindow: Leading edge found at" << m_calibLeadingEdge << "us";
        } else {
            m_calibTrialDelay = testedDelay + 10; 
            m_calibOverlay->setText(
                QString::fromUtf8("闪光校准 - 寻找快门窗口前沿...\n当前测试起振偏移: %1 us\n快门: 1/%2 s")
                    .arg(m_calibTrialDelay)
                    .arg(m_manualShutterUs > 0 ? 1000000 / m_manualShutterUs : 0));
        }
    }
    // 寻找后沿
    else if (m_calibState == CALIB_SEARCH_TRAILING) {
        if (flashDetected) {
            m_calibTrialDelay = testedDelay + 10;
            m_calibOverlay->setText(
                QString::fromUtf8("闪光校准 - 寻找快门窗口后沿...\n已确立前沿偏移: %1 us\n当前测试偏移: %2 us\n进度: 扫描中...")
                    .arg(m_calibLeadingEdge)
                    .arg(m_calibTrialDelay));
        } else {
            m_calibTrailingEdge = testedDelay - 10;
            m_calibBestDelay = (m_calibLeadingEdge + m_calibTrailingEdge) / 2;
            m_calibTrialDelay = m_calibBestDelay;
            qDebug() << "MainWindow: Trailing edge found at" << m_calibTrailingEdge << "us. True Center:" << m_calibBestDelay;
            
            m_calibSuccessCount = 0;
            m_calibWaitTicks = 0;
            m_calibState = CALIB_COOLDOWN;
            return;
        }
    }
    else if (m_calibState == CALIB_COOLDOWN) {
        m_calibWaitTicks++;
        if (m_calibWaitTicks >= 15) { // 冷却 3 秒，确保闪光灯电容完全回电
            m_calibState = CALIB_VERIFY;
            m_calibWaitTicks = 0; 
            m_calibTrialDelay = m_calibBestDelay; 
            pioController->triggerCapturePulse(m_manualShutterUs, m_calibTrialDelay, nullptr);
            m_calibLastTestedDelay = m_calibTrialDelay;
        } else {
            m_calibOverlay->setText(
                QString::fromUtf8("闪光校准 - 硬件冷却与储能中...\n剩余: %1 秒\n即将验证中心偏移: %2 us")
                    .arg(1.0 - (m_calibWaitTicks * 0.2), 0, 'f', 1)
                    .arg(m_calibBestDelay));
        }
        return;
    }
    // 稳定性验证
    else if (m_calibState == CALIB_VERIFY) {
        if (flashDetected) {
            m_calibSuccessCount++;
            if (m_calibSuccessCount >= 1) {
                finishCalibration(true);
                return;
            }
        }
        
        m_calibWaitTicks++;
        if (m_calibWaitTicks >= 10) { 
            finishCalibration(false);
            return;
        }

        m_calibOverlay->setText(
            QString::fromUtf8("闪光校准 - 稳定性验证\n起振偏移锁定: %1 us\n连续成功: %2/1 (等待: %3/10)")
                .arg(m_calibTrialDelay)
                .arg(m_calibSuccessCount)
                .arg(m_calibWaitTicks));
                
        m_calibTrialDelay = m_calibBestDelay;
    }
    pioController->triggerCapturePulse(m_manualShutterUs, m_calibTrialDelay, nullptr);
    m_calibLastTestedDelay = m_calibTrialDelay;
}

void MainWindow::finishCalibration(bool success) {
    m_calibTimer->stop();
    m_calibState = CALIB_OFF;

    engine->setManualExposure(m_manualShutterUs, m_calibSavedIso);

    if (success) {
        m_flashPreDelay = m_calibTrialDelay;

        {
            QSettings settings("C9M3", "Camera");
            settings.setValue("flashPreDelay", m_flashPreDelay);
            settings.sync();
        }

        qDebug() << "MainWindow: Flash calibration SUCCESS!"
                 << "flashPreDelay fixed at" << m_flashPreDelay
                 << "us (persisted to QSettings).";

        m_calibOverlay->setText(
            QString::fromUtf8("✔ 校准完成\n起振偏移已固定为 %1 us\n(%2 ms)")
                .arg(m_flashPreDelay)
                .arg(m_flashPreDelay / 1000.0, 0, 'f', 1));
        m_calibOverlay->setStyleSheet(
            "QLabel {"
            "  background-color: rgba(0, 70, 0, 210);"
            "  color: #00FF88;"
            "  font-family: 'WenQuanYi Zen Hei', 'SimHei', Arial;"
            "  font-size: 28px;"
            "  font-weight: bold;"
            "  border-radius: 16px;"
            "}"
        );
    } else {
        qDebug() << "MainWindow: Flash calibration FAILED.";
        m_calibOverlay->setText(QString::fromUtf8("❌ 校准失败\n未能找到稳定的起振偏移量"));
        m_calibOverlay->setStyleSheet(
            "QLabel {"
            "  background-color: rgba(100, 0, 0, 210);"
            "  color: #FF6666;"
            "  font-family: 'WenQuanYi Zen Hei', 'SimHei', Arial;"
            "  font-size: 28px;"
            "  font-weight: bold;"
            "  border-radius: 16px;"
            "}"
        );
    }

    m_calibOverlay->show();
    m_calibOverlay->raise();

    QTimer::singleShot(4000, this, [this]() {
        m_calibOverlay->hide();
        m_calibOverlay->setStyleSheet(
            "QLabel {"
            "  background-color: rgba(0, 0, 0, 170);"
            "  color: white;"
            "  font-family: 'WenQuanYi Zen Hei', 'SimHei', Arial;"
            "  font-size: 28px;"
            "  font-weight: bold;"
            "  border-radius: 16px;"
            "}"
        );
    });
}

// 手动闪光校准逻辑

void MainWindow::startManualFlashCalibration() {
    if (m_isVideoMode) {
        m_calibOverlay->setText(QString::fromUtf8("请切换到拍照模式\n再进行手动校准。"));
        m_calibOverlay->show();
        m_calibOverlay->raise();
        QTimer::singleShot(3000, this, [this]() { m_calibOverlay->hide(); });
        return;
    }

    if (m_calibState != CALIB_OFF) {
        m_calibTimer->stop();
        m_calibState = CALIB_OFF;
    }

    m_isManualCalibActive = true;
    
    int w = 600;
    int h = 100;
    int x = (width() - w) / 2;
    int y = height() - h - 140;
    m_manualCalibWidget->setGeometry(x, y, w, h);
    
    m_lblManualCalibDelay->setText(QString("%1 us").arg(m_flashPreDelay));
    
    m_manualCalibWidget->show();
    m_manualCalibWidget->raise();
    
    qDebug() << "MainWindow: Manual flash calibration started. current=" << m_flashPreDelay << "us";
}

void MainWindow::onManualCalibMinus() {
    if (!m_isManualCalibActive) return;
    
    int step = (m_flashPreDelay > 3000) ? 50000 : 2;
    m_flashPreDelay -= step;
    
    if (m_flashPreDelay < 0) m_flashPreDelay = 0;
    if (m_flashPreDelay == 3000 - 50000) m_flashPreDelay = 3000;
    
    m_lblManualCalibDelay->setText(QString("%1 us").arg(m_flashPreDelay));
}

void MainWindow::onManualCalibPlus() {
    if (!m_isManualCalibActive) return;
    
    int step = (m_flashPreDelay >= 3000) ? 50000 : 2;
    m_flashPreDelay += step;
    
    if (m_flashPreDelay > 1000000) m_flashPreDelay = 1000000;
    
    m_lblManualCalibDelay->setText(QString("%1 us").arg(m_flashPreDelay));
}

void MainWindow::finishManualCalibration() {
    if (!m_isManualCalibActive) return;
    
    m_isManualCalibActive = false;
    m_manualCalibWidget->hide();
    
    // 固化设置
    QSettings settings("C9M3", "Camera");
    settings.setValue("flashPreDelay", m_flashPreDelay);
    settings.sync();
    
    qDebug() << "MainWindow: Manual flash calibration finished. Saved:" << m_flashPreDelay << "us";
    
    m_calibOverlay->setText(QString::fromUtf8("手动校准完成\n起振偏移已保存: %1 us").arg(m_flashPreDelay));
    m_calibOverlay->setStyleSheet(
        "QLabel {"
        "  background-color: rgba(0, 70, 0, 210);"
        "  color: #00FF88;"
        "  font-family: 'WenQuanYi Zen Hei', 'SimHei', Arial;"
        "  font-size: 28px;"
        "  font-weight: bold;"
        "  border-radius: 16px;"
        "}"
    );
    m_calibOverlay->show();
    m_calibOverlay->raise();
    
    QTimer::singleShot(2000, this, [this]() {
        m_calibOverlay->hide();
        m_calibOverlay->setStyleSheet(
            "QLabel {"
            "  background-color: rgba(0, 0, 0, 170);"
            "  color: white;"
            "  font-family: 'WenQuanYi Zen Hei', 'SimHei', Arial;"
            "  font-size: 28px;"
            "  font-weight: bold;"
            "  border-radius: 16px;"
            "}"
        );
    });
}

bool MainWindow::eventFilter(QObject* obj, QEvent* event) {
    if (obj == m_lblManualCalibDelay) {
        if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::TouchBegin) {
            finishManualCalibration();
            return true;
        }
    }
    return QWidget::eventFilter(obj, event);
}
