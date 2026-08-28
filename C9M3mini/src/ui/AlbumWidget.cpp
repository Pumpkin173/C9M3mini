#include "AlbumWidget.h"
#include <QDir>
#include <QDirIterator>
#include <QPainter>
#include <QPainterPath>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QMediaPlayer>
#include <QAudioOutput>
#include <QGraphicsView>
#include <QGraphicsScene>
#include <QGraphicsVideoItem>
#include <QProcess>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QLinearGradient>
#include <QDateTime>
#include <QFile>
#include <QDebug>
#include <cmath>
#include "../core/VideoRecorder.h"

// 静态工具函数
static void parsePixFmt(const QString& pix_fmt, QString& chroma, int& bitDepth) {
    bitDepth = 8;
    if (pix_fmt.contains("10")) bitDepth = 10;
    else if (pix_fmt.contains("12")) bitDepth = 12;

    if (pix_fmt.startsWith("yuv420") || pix_fmt == "nv12" || pix_fmt == "nv21"
            || pix_fmt.startsWith("yuvj420"))        chroma = "4:2:0";
    else if (pix_fmt.startsWith("yuv422") || pix_fmt.startsWith("yuvj422")) chroma = "4:2:2";
    else if (pix_fmt.startsWith("yuv444") || pix_fmt.startsWith("yuvj444")) chroma = "4:4:4";
    else if (pix_fmt.startsWith("gray")   || pix_fmt.startsWith("mono"))    chroma = "4:0:0";
    else chroma = pix_fmt;
}

static QString friendlyCodec(const QString& n) {
    if (n == "h264")     return "H.264";
    if (n == "hevc")     return "H.265";
    if (n == "vp8")      return "VP8";
    if (n == "vp9")      return "VP9";
    if (n == "av1")      return "AV1";
    if (n == "rawvideo") return "RAW";
    if (n == "mpeg4")    return "MPEG-4";
    if (n == "mjpeg")    return "MJPEG";
    return n.toUpper();
}

static QString friendlyBitrate(qint64 bps) {
    if (bps <= 0) return "—";
    if (bps >= 1000000) return QString("%1 Mbps").arg(bps / 1000000.0, 0, 'f', 1);
    return QString("%1 Kbps").arg(bps / 1000);
}

static QString friendlyFps(const QString& r) {
    QStringList p = r.split('/');
    if (p.size() == 2) {
        double num = p[0].toDouble(), den = p[1].toDouble();
        if (den > 0) {
            double fps = num / den;
            static const double known[] = {23.976,24.0,25.0,29.97,30.0,
                                           47.952,48.0,50.0,59.94,60.0};
            for (double k : known)
                if (std::fabs(fps - k) < 0.1) return QString::number(k, 'g', 5);
            return QString::number(fps, 'f', 2);
        }
    }
    return r;
}

static QString thumbPath(const QFileInfo& fi) {
    QString key = QString::number(fi.lastModified().toSecsSinceEpoch()) + fi.fileName();
    return QDir::tempPath() + "/c9m3albthumb_" + QString::number(qHash(key), 16) + ".jpg";
}

//  构造 / 析构
AlbumWidget::AlbumWidget(QWidget* parent) : QWidget(parent) {
    setStyleSheet("background-color: black;");
    
    m_videoScene = new QGraphicsScene(this);
    m_videoView = new QGraphicsView(m_videoScene, this);
    m_videoView->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_videoView->setStyleSheet("background: transparent; border: none;");
    m_videoView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_videoView->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_videoView->hide();
    
    m_videoItem = new QGraphicsVideoItem;
    m_videoItem->setAspectRatioMode(Qt::KeepAspectRatio);
    m_videoScene->addItem(m_videoItem);

    m_audioOutput = new QAudioOutput(this);
    m_mediaPlayer = new QMediaPlayer(this);
    m_mediaPlayer->setAudioOutput(m_audioOutput);
    m_mediaPlayer->setVideoOutput(m_videoItem);
    
    // Initialize overlay UI component
    m_overlayWidget = new AlbumOverlayWidget(this);
    m_overlayWidget->hide();
    
    // Monitor media player state to handle UI transitions
    connect(m_mediaPlayer, &QMediaPlayer::playbackStateChanged, this, [=](QMediaPlayer::PlaybackState state) {
        if (state == QMediaPlayer::StoppedState) {
            m_videoView->hide();
            if (m_overlayWidget) m_overlayWidget->hide();
            update();
        }
    });

    // 绑定悬浮 UI 渲染回调
    m_overlayWidget->paintCallback = [this](QPainter& p) {
        if (m_currentIndex >= 0 && m_isVideoMode) {
            const VideoMeta& meta = m_metaCache[m_currentIndex];
            drawCinemaOverlay(p, meta, m_files[m_currentIndex]);
        }
    };
}

AlbumWidget::~AlbumWidget() {
    if (m_mediaPlayer && m_mediaPlayer->playbackState() == QMediaPlayer::PlayingState) {
        m_mediaPlayer->stop();
    }
    const auto procs = m_activeProcs;
    for (QProcess* p : procs) {
        if (p && p->state() != QProcess::NotRunning) {
            p->kill();
            p->waitForFinished(500);
        }
    }
}

//  公共接口
void AlbumWidget::openAlbum() {
    m_isVideoMode = false;
    QString photoDir = VideoRecorder::getActiveStoragePath() + "/Photo";
    QDir dir(photoDir);
    QStringList filters; filters << "IMG_*.dng" << "IMG_*.jpg";
    dir.setNameFilters(filters);
    dir.setSorting(QDir::Time | QDir::Reversed);
    
    QFileInfoList allFiles = dir.entryInfoList(filters, QDir::Files, QDir::Time);
    m_files.clear();
    
    // 文件去重策略
    QSet<QString> baseNames;
    for (const QFileInfo& fi : allFiles) {
        QString base = fi.completeBaseName();
        if (!baseNames.contains(base)) {
            m_files.append(fi);
            baseNames.insert(base);
        }
    }
    
    m_currentIndex = m_files.isEmpty() ? -1 : 0;
    m_currentImage = QImage();
    if (!m_files.isEmpty()) loadCurrentImage();
    show(); raise(); setFocus();
}

void AlbumWidget::openVideoAlbum() {
    m_isVideoMode = true;
    m_currentImage = QImage();

    // 每次打开都重新扫描，确保新录制文件出现
    QStringList filters;
    filters << "*.mp4" << "*.mkv" << "*.mov" << "*.h264" << "*.h265";
    
    QString videoDir = VideoRecorder::getActiveStoragePath() + "/Video";
    QDirIterator it(videoDir, filters, QDir::Files,
                    QDirIterator::Subdirectories);

    const auto procs = m_activeProcs;
    for (QProcess* p : procs) {
        if (p && p->state() != QProcess::NotRunning) {
            p->kill();
            p->waitForFinished(100);
        }
    }
    m_metaCache.clear();
    m_files.clear();

    while (it.hasNext()) {
        it.next();
        // 将代理文件重映射为父级序列会话
        QFileInfo fi = it.fileInfo();
        if (fi.fileName().endsWith("_proxy.mp4")) {
            m_files.append(QFileInfo(fi.dir().absolutePath()));
        } else {
            m_files.append(fi);
        }
    }

    // 扫描 /dev/shm 中的活跃代理（正在录制的 cDNG）
    QDirIterator itShm("/dev/shm", QStringList() << "*_proxy.mp4", QDir::Files);
    while (itShm.hasNext()) {
        itShm.next();
        QFileInfo fi = itShm.fileInfo();
        QString sessionName = fi.fileName(); 
        if (sessionName.startsWith("video_") && sessionName.endsWith("_proxy.mp4")) {
            sessionName = sessionName.mid(6, sessionName.length() - 16); 
            QString sessionDir = videoDir + "/" + sessionName;
            QFileInfo dirInfo(sessionDir);
            bool exists = false;
            for (const QFileInfo& existing : m_files) {
                if (existing.absoluteFilePath() == dirInfo.absoluteFilePath()) {
                    exists = true;
                    break;
                }
            }
            if (!exists && dirInfo.exists()) {
                m_files.append(dirInfo);
            }
        }
    }

    // 按修改时间降序（最新录制排 index 0）
    std::sort(m_files.begin(), m_files.end(), [](const QFileInfo& a, const QFileInfo& b) {
        return a.lastModified() > b.lastModified();
    });

    m_currentIndex = m_files.isEmpty() ? -1 : 0;

    show(); raise(); setFocus(); update();

    if (m_currentIndex >= 0) {
        requestVideoMeta(m_currentIndex);
        if (m_currentIndex + 1 < m_files.size())
            requestVideoMeta(m_currentIndex + 1);
    }
}

//  停止播放（绑定 Fn 实体键）
void AlbumWidget::stopPlayback() {
    if (m_mediaPlayer && m_mediaPlayer->playbackState() == QMediaPlayer::PlayingState) {
        m_mediaPlayer->stop();
    }
}

//  删除逻辑 (硬件按钮 + UI)

void AlbumWidget::onDeletePressed() {
    if (!isVisible() || m_files.isEmpty()) return;
    if (m_mediaPlayer && m_mediaPlayer->playbackState() == QMediaPlayer::PlayingState) return;

    if (!m_showDeleteConfirm) {
        m_showDeleteConfirm = true;
        update();
    }
}


void AlbumWidget::executeDelete() {
    if (m_currentIndex < 0 || m_currentIndex >= m_files.size()) return;

    QFileInfo fi = m_files[m_currentIndex];
    bool isDir = fi.isDir();
    QString filePath = fi.absoluteFilePath();

    if (isDir) {
        QDir dir(filePath);
        dir.removeRecursively();
        
        // 尝试清理可能残留的空会话目录
        QDir parentDir = dir;
        parentDir.cdUp();
        parentDir.rmdir(dir.dirName());

        QString sessionName = fi.fileName();
        QString shmPath = "/dev/shm/video_" + sessionName + "_proxy.mp4";
        if (QFile::exists(shmPath)) {
            QFile::remove(shmPath);
        }
    } else {
        QFile::remove(filePath);
        
        // 同步清理空的 H.264 会话目录
        QDir parentDir = fi.dir();
        if (parentDir.isEmpty()) {
            QDir grandParent = parentDir;
            grandParent.cdUp();
            grandParent.rmdir(parentDir.dirName());
        }
    }

    m_files.removeAt(m_currentIndex);
    m_metaCache.clear();
    m_showDeleteConfirm = false;

    if (m_files.isEmpty()) {
        m_currentIndex = -1;
        m_currentImage = QImage();
    } else {
        if (m_currentIndex >= m_files.size()) {
            m_currentIndex = m_files.size() - 1;
        }
    }

    if (m_isVideoMode) {
        if (m_currentIndex >= 0) navigateVideo(m_currentIndex);
    } else {
        if (m_currentIndex >= 0) loadCurrentImage();
    }
    update();
}


//  异步元数据 + 缩略图提取

void AlbumWidget::requestVideoMeta(int index) {
    if (index < 0 || index >= m_files.size()) return;
    if (m_metaCache.contains(index) &&
        (m_metaCache[index].loaded || m_metaCache[index].loading)) return;

    m_metaCache[index].loading = true;
    QFileInfo fi = m_files[index];
    
    // 如果该项是文件夹，说明是从 Proxy 提取的 DNG 会话
    QString filePath = fi.absoluteFilePath();
    bool isDngProxy = fi.isDir();
    if (isDngProxy) {
        QString sessionName = fi.fileName();
        filePath = fi.absoluteFilePath() + "/video_" + sessionName + "_proxy.mp4";
        if (!QFile::exists(filePath)) {
            QString shmPath = "/dev/shm/video_" + sessionName + "_proxy.mp4";
            if (QFile::exists(shmPath)) {
                filePath = shmPath;
            }
        }
    }

    QString cacheFile = thumbPath(fi);

    auto* ffprobe = new QProcess(this);
    m_activeProcs.append(ffprobe);

    connect(ffprobe, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [=](int exitCode, QProcess::ExitStatus) {

        VideoMeta& meta = m_metaCache[index];

        if (exitCode == 0) {
            QByteArray raw = ffprobe->readAllStandardOutput();
            QJsonObject root = QJsonDocument::fromJson(raw).object();
            
            QJsonObject formatObj = root["format"].toObject();
            if (formatObj.contains("duration")) {
                double durSec = formatObj["duration"].toString().toDouble();
                int h = durSec / 3600;
                int m = (int(durSec) % 3600) / 60;
                int s = int(durSec) % 60;
                if (h > 0) meta.duration = QString("%1:%2:%3").arg(h, 2, 10, QChar('0')).arg(m, 2, 10, QChar('0')).arg(s, 2, 10, QChar('0'));
                else       meta.duration = QString("%1:%2").arg(m, 2, 10, QChar('0')).arg(s, 2, 10, QChar('0'));
            } else {
                meta.duration = "--:--";
            }

            QJsonArray streams = root["streams"].toArray();
            for (const QJsonValue& sv : streams) {
                QJsonObject s = sv.toObject();
                if (s["codec_type"].toString() != "video") continue;
                meta.codec  = isDngProxy ? "RAW+H.264" : friendlyCodec(s["codec_name"].toString());
                meta.width  = isDngProxy ? 1456 : s["width"].toInt(); // For UI clarity, display raw res
                meta.height = isDngProxy ? 1088 : s["height"].toInt();
                meta.fps    = friendlyFps(s["r_frame_rate"].toString());
                parsePixFmt(s["pix_fmt"].toString(), meta.chroma, meta.bitDepth);
                if (isDngProxy) { meta.chroma = "RAW"; meta.bitDepth = 14; }
                qint64 bps = s["bit_rate"].toString().toLongLong();
                if (bps <= 0)
                    bps = root["format"].toObject()["bit_rate"].toString().toLongLong();
                meta.bitrate = friendlyBitrate(bps);
                break;
            }
        } else {
            qDebug() << "AlbumWidget: ffprobe failed for" << filePath;
        }

        m_activeProcs.removeOne(ffprobe);
        ffprobe->deleteLater();

        // 提取第一帧缩略图
        const QString tp = thumbPath(fi);

        if (QFile::exists(tp)) {
            meta.thumbnail.load(tp);
            meta.loaded  = true;
            meta.loading = false;
            if (index == m_currentIndex) update();
            return;
        }

        auto* ffmpeg = new QProcess(this);
        m_activeProcs.append(ffmpeg);

        connect(ffmpeg, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                this, [=](int, QProcess::ExitStatus) {
            VideoMeta& m2 = m_metaCache[index];
            m2.thumbnail.load(tp);
            m2.loaded  = true;
            m2.loading = false;
            m_activeProcs.removeOne(ffmpeg);
            ffmpeg->deleteLater();
            if (index == m_currentIndex) update();
        });

        // H.264 Proxy is 728x544 usually or similar, we scale for UI
        ffmpeg->start("ffmpeg", {
            "-ss", "0.001",
            "-i", filePath,
            "-vframes", "1",
            "-vf", "scale=640:-2",
            "-q:v", "2",
            "-y", tp,
            "-loglevel", "quiet"
        });
    });

    ffprobe->start("ffprobe", {
        "-v", "quiet",
        "-print_format", "json",
        "-show_streams",
        "-show_format",
        filePath
    });
}

// 统一导航（含预加载）
void AlbumWidget::navigateVideo(int newIndex) {
    if (newIndex < 0 || newIndex >= m_files.size()) return;
    m_currentIndex = newIndex;
    requestVideoMeta(m_currentIndex);
    if (m_currentIndex + 1 < m_files.size()) requestVideoMeta(m_currentIndex + 1);
    if (m_currentIndex - 1 >= 0)             requestVideoMeta(m_currentIndex - 1);
    update();
}


//  照片加载

void AlbumWidget::loadCurrentImage() {
    if (m_currentIndex < 0 || m_currentIndex >= m_files.size()) return;
    m_isLoading = true;
    m_isZoomed = false;
    
    QString filePath = m_files[m_currentIndex].absoluteFilePath();
    bool loaded = false;
    
    if (filePath.endsWith(".dng", Qt::CaseInsensitive)) {
        // 解析内嵌 JPG
        QFile f(filePath);
        if (f.open(QIODevice::ReadOnly)) {
            QByteArray header = f.read(1024);
            if (header.size() == 1024) {
                // 查找 0x0201 (JPEGInterchangeFormat) 和 0x0202 (Length)
                uint32_t jpegOffset = 0;
                uint32_t jpegLength = 0;
                
                uint16_t ifd0_count = *reinterpret_cast<const uint16_t*>(header.data() + 8);
                // 安全校验，防止越界
                if (10 + ifd0_count * 12 <= 1024) {
                    for (int i = 0; i < ifd0_count; ++i) {
                        const char* entry = header.data() + 10 + i * 12;
                        uint16_t tag = *reinterpret_cast<const uint16_t*>(entry);
                        uint32_t val = *reinterpret_cast<const uint32_t*>(entry + 8);
                        if (tag == 0x0201) jpegOffset = val;
                        if (tag == 0x0202) jpegLength = val;
                    }
                }
                
                if (jpegOffset > 0 && jpegLength > 0 && jpegLength != 0xDEADBEEF) {
                    f.seek(jpegOffset);
                    QByteArray jpegData = f.read(jpegLength);
                    loaded = m_currentImage.loadFromData(jpegData, "JPG");
                }
            }
            f.close();
        }
        
        // 降级策略：尝试加载外部同名 JPG
        if (!loaded) {
            QString jpgFallback = filePath;
            jpgFallback.replace(".dng", ".jpg");
            loaded = m_currentImage.load(jpgFallback);
        }
    } else {
        loaded = m_currentImage.load(filePath);
    }
    
    if (!loaded) {
        qDebug() << "AlbumWidget: failed to load image" << filePath;
        m_currentImage = QImage();
    }
    
    m_isLoading = false;
    update();
}


//
void AlbumWidget::drawStrokeText(QPainter& p, int x, int y,
                                 const QString& text, const QFont& font,
                                 const QColor& fillColor, double strokeWidth) {
    QPainterPath path;
    path.addText(x, y, font, text);
    p.setPen(QPen(Qt::black, strokeWidth * 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    p.drawPath(path);
    p.setPen(Qt::NoPen);
    p.setBrush(fillColor);
    p.drawPath(path);
}

void AlbumWidget::drawVideoBackground(QPainter& p, const VideoMeta& meta) {
    if (!meta.thumbnail.isNull()) {
        QSize s = meta.thumbnail.size();
        s.scale(size(), Qt::KeepAspectRatioByExpanding);
        int x = (width()  - s.width())  / 2;
        int y = (height() - s.height()) / 2;
        p.drawImage(QRect(x, y, s.width(), s.height()), meta.thumbnail);
    } else {
        QLinearGradient bg(0, 0, 0, height());
        bg.setColorAt(0.0, QColor(28, 28, 32));
        bg.setColorAt(1.0, QColor(10, 10, 12));
        p.fillRect(rect(), bg);
        if (meta.loading) {
            QFont lf("Arial", 16);
            QString msg = "Loading...";
            drawStrokeText(p,
                           width()  / 2 - QFontMetrics(lf).horizontalAdvance(msg) / 2,
                           height() / 2, msg, lf, QColor(160, 160, 160));
        }
    }
}

void AlbumWidget::drawCinemaOverlay(QPainter& p, const VideoMeta& meta, const QFileInfo& fi) {
    int W = width(), H = height();

    // 1. Top Black Letterbox Bar (HUD)
    int topBarH = 40;
    p.fillRect(0, 0, W, topBarH, QColor(0, 0, 0, 180));

    // Top HUD - BACK Button (Left)
    int backW = W * 0.1;
    m_backRect = QRect(10, 0, backW, 40);
    p.setPen(QColor(200, 200, 200));
    p.setFont(QFont("Arial", 12, QFont::Bold));
    p.drawText(m_backRect, Qt::AlignLeft | Qt::AlignVCenter, " < BACK");

    // Top HUD - Clip Name (Center)
    QString clipName = fi.fileName();
    if (fi.isDir()) clipName = "video_" + clipName + "_proxy";
    p.setPen(Qt::white);
    p.setFont(QFont("Arial", 12, QFont::Bold));
    int centerW = W * 0.5;
    p.drawText(QRect((W - centerW - 100) / 2, 0, centerW, 40), Qt::AlignCenter, clipName);

    // Top HUD - Timecode/Duration & Index (Right)
    QString rightText = QString("CLIP %1/%2").arg(m_currentIndex + 1).arg(m_files.size());
    if (!meta.duration.isEmpty() && meta.duration != "--:--") {
        rightText = meta.duration + "  |  " + rightText;
    }
    p.setPen(QColor(200, 200, 200));
    p.setFont(QFont("Monospace", 12, QFont::Bold));
    int rightW = W * 0.30;
    p.drawText(QRect(W - rightW - 10, 0, rightW, 40), Qt::AlignRight | Qt::AlignVCenter, rightText);

    // 2. Bottom Black Letterbox Bar (Transport & Metadata)
    int bottomBarH = 60;
    int bottomY = H - bottomBarH;
    p.fillRect(0, bottomY, W, bottomBarH, QColor(0, 0, 0, 180));

    // Transport Controls
    int controlCenterX = W / 2 + 180;
    int transportY = bottomY + 30;
    int spacing = 80; // Spread out

    // Lower Third - Metadata (Left)
    QString resStr = (meta.width > 0 && meta.height > 0) ? QString("%1x%2").arg(meta.width).arg(meta.height) : "---";
    QString codecStr = meta.codec.isEmpty() ? "---" : meta.codec;
    QString fpsStr = meta.fps.isEmpty() ? "---" : meta.fps + " FPS";
    QString bitStr = meta.bitrate.isEmpty() ? "---" : meta.bitrate;
    QString metaLine = QString("%1 | %2 | %3 | %4").arg(resStr, fpsStr, codecStr, bitStr);
    
    p.setPen(QColor(180, 180, 180));
    p.setFont(QFont("Arial", 11, QFont::Bold));
    // 让 Metadata 区域宽度直接扩展到 "上一首" 按钮前 20 像素的位置
    int metaMaxWidth = (controlCenterX - spacing) - 30;
    p.drawText(QRect(10, bottomY, metaMaxWidth, bottomBarH), Qt::AlignLeft | Qt::AlignVCenter, metaLine);

    // Prev Button
    int px = controlCenterX - spacing;
    m_prevRect = QRect(px - 15, transportY - 20, 30, 40);
    QPainterPath prevPath;
    prevPath.moveTo(px - 5, transportY);
    prevPath.lineTo(px + 5, transportY - 10);
    prevPath.lineTo(px + 5, transportY + 10);
    prevPath.closeSubpath();
    prevPath.moveTo(px - 15, transportY);
    prevPath.lineTo(px - 5, transportY - 10);
    prevPath.lineTo(px - 5, transportY + 10);
    prevPath.closeSubpath();
    p.fillPath(prevPath, Qt::white);
    
    // Play Button
    m_playRect = QRect(controlCenterX - 25, transportY - 25, 50, 50);
    QPainterPath playPath;
    playPath.moveTo(controlCenterX - 8, transportY - 12);
    playPath.lineTo(controlCenterX + 16, transportY);
    playPath.lineTo(controlCenterX - 8, transportY + 12);
    playPath.closeSubpath();
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(Qt::white, 2));
    p.drawEllipse(QPoint(controlCenterX, transportY), 22, 22);
    p.fillPath(playPath, Qt::white);
    p.setPen(Qt::NoPen); // reset

    // Next Button
    int nx = controlCenterX + spacing;
    m_nextRect = QRect(nx - 15, transportY - 20, 30, 40);
    QPainterPath nextPath;
    nextPath.moveTo(nx + 5, transportY);
    nextPath.lineTo(nx - 5, transportY - 10);
    nextPath.lineTo(nx - 5, transportY + 10);
    nextPath.closeSubpath();
    nextPath.moveTo(nx + 15, transportY);
    nextPath.lineTo(nx + 5, transportY - 10);
    nextPath.lineTo(nx + 5, transportY + 10);
    nextPath.closeSubpath();
    p.fillPath(nextPath, Qt::white);

    // MPV Playing HUD Message
    if (m_mediaPlayer && m_mediaPlayer->playbackState() == QMediaPlayer::PlayingState) {
        QString playMsg = QString::fromUtf8("▶  PLAYING   |  Press Fn to stop");
        QFont pf("Arial", 13, QFont::Bold);
        QFontMetrics fmP(pf);
        int pw  = fmP.horizontalAdvance(playMsg) + 20;
        int px  = (W - pw) / 2;
        int py  = topBarH + 20;
        p.setBrush(QColor(180, 0, 0, 200));
        p.setPen(Qt::NoPen);
        p.drawRoundedRect(px, py, pw, fmP.height() + 10, 6, 6);
        drawStrokeText(p, px + 10, py + fmP.ascent() + 5, playMsg, pf, Qt::white, 1.0);
    }
}

void AlbumWidget::drawDeleteConfirmOverlay(QPainter& p) {
    int W = width(), H = height();
    p.fillRect(0, 0, W, H, QColor(0, 0, 0, 180));

    int dialogW = 340;
    int dialogH = 180;
    int dialogX = (W - dialogW) / 2;
    int dialogY = (H - dialogH) / 2;

    QRect dialogRect(dialogX, dialogY, dialogW, dialogH);
    p.setBrush(QColor(40, 40, 40, 240));
    p.setPen(QPen(QColor(100, 100, 100), 2));
    p.drawRoundedRect(dialogRect, 10, 10);

    QFont msgFont("Arial", 18, QFont::Bold);
    p.setFont(msgFont);
    p.setPen(Qt::white);
    p.drawText(QRect(dialogX, dialogY + 20, dialogW, 40), Qt::AlignCenter, "Delete this file?");

    QFont subFont("Arial", 12);
    p.setFont(subFont);
    p.setPen(QColor(200, 200, 200));
    p.drawText(QRect(dialogX, dialogY + 60, dialogW, 40), Qt::AlignCenter, "Press Joystick or Confirm");

    int btnW = 120;
    int btnH = 40;
    int btnX = dialogX + (dialogW - btnW) / 2;
    int btnY = dialogY + 110;
    m_confirmRect = QRect(btnX, btnY, btnW, btnH);

    p.setBrush(QColor(200, 50, 50));
    p.setPen(QPen(Qt::white, 2));
    p.drawRoundedRect(m_confirmRect, 5, 5);

    QFont btnFont("Arial", 14, QFont::Bold);
    p.setFont(btnFont);
    p.setPen(Qt::white);
    p.drawText(m_confirmRect, Qt::AlignCenter, "Confirm");
}

void AlbumWidget::paintEvent(QPaintEvent*) {
    // 如果正在播放，不需要画任何东西，只需让透明背景透过即可
    if (m_mediaPlayer && m_mediaPlayer->playbackState() == QMediaPlayer::PlayingState) {
        return;
    }

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    
    p.fillRect(rect(), Qt::black);

    // 空文件夹
    if (m_files.isEmpty()) {
        QFont f("Arial", 24);
        QString msg = m_isVideoMode ? "No Videos" : "No Images";
        drawStrokeText(p,
                       width()  / 2 - QFontMetrics(f).horizontalAdvance(msg) / 2,
                       height() / 2, msg, f, Qt::white);
        return;
    }

    // 照片模式
    if (!m_isVideoMode) {
        if (!m_currentImage.isNull()) {
            QSize sz = m_currentImage.size();
            sz.scale(size(), Qt::KeepAspectRatio);
            
            if (m_isZoomed) {
                int scaledW = sz.width() * 2;
                int scaledH = sz.height() * 2;
                p.drawImage(QRect(m_zoomOffset.x(), m_zoomOffset.y(), scaledW, scaledH), m_currentImage);
            } else {
                int x = (width()  - sz.width())  / 2;
                int y = (height() - sz.height()) / 2;
                p.drawImage(QRect(x, y, sz.width(), sz.height()), m_currentImage);
            }
        }
        QString cnt = QString("%1 / %2").arg(m_currentIndex + 1).arg(m_files.size());
        QFont cf("Arial", 16, QFont::Bold);
        drawStrokeText(p,
                       width() - QFontMetrics(cf).horizontalAdvance(cnt) - 110,
                       34, cnt, cf, Qt::white);
        int esz = 60;
        QRect exitR(width() - esz - 20, (height() - esz) / 2, esz, esz);
        p.setBrush(QColor(0, 0, 0, 150));
        p.setPen(QPen(Qt::white, 2));
        p.drawRoundedRect(exitR, 10, 10);
        QFont ef("Arial", 26, QFont::Bold);
        QFontMetrics fmEf(ef);
        drawStrokeText(p,
                       exitR.left() + (esz - fmEf.horizontalAdvance("X")) / 2,
                       exitR.top()  + (esz + fmEf.ascent() - fmEf.descent()) / 2,
                       "X", ef, Qt::white);
        
        if (m_showDeleteConfirm) {
            drawDeleteConfirmOverlay(p);
        }
        return;
    }

    // 视频模式
    if (m_currentIndex < 0) {
        if (m_showDeleteConfirm) m_showDeleteConfirm = false;
        return;
    }
    const VideoMeta& meta = m_metaCache[m_currentIndex];
    
    drawVideoBackground(p, meta);
    drawCinemaOverlay(p, meta, m_files[m_currentIndex]);

    if (m_showDeleteConfirm) {
        drawDeleteConfirmOverlay(p);
    }
}

void AlbumWidget::launchMpv() {
    // 已在运行则忽略
    if (m_mediaPlayer && m_mediaPlayer->playbackState() == QMediaPlayer::PlayingState) return;

    QFileInfo fi = m_files[m_currentIndex];
    QString filePath = fi.absoluteFilePath();
    if (fi.isDir()) {
        QString sessionName = fi.fileName();
        filePath = fi.absoluteFilePath() + "/video_" + sessionName + "_proxy.mp4";
        if (!QFile::exists(filePath)) {
            QString shmPath = "/dev/shm/video_" + sessionName + "_proxy.mp4";
            if (QFile::exists(shmPath)) {
                filePath = shmPath;
            }
        }
    }

    qDebug() << "AlbumWidget: launching QMediaPlayer via QGraphicsView for" << filePath;

    if (m_videoView) {
        m_videoView->setGeometry(rect());
        m_videoScene->setSceneRect(rect());
        
        // 反转画布宽高以适配 90 度旋转全屏
        m_videoItem->setSize(QSizeF(width(), height()));
        m_videoItem->setPos(0, 0);
        m_videoItem->setTransformOriginPoint(0, 0);
        m_videoItem->setRotation(0);

        m_videoView->lower();
        m_videoView->show();
        
        // 播放时隐藏透明 UI 层，避免 EGLFS 环境下画面撕裂
        if (m_overlayWidget) {
            m_overlayWidget->hide();
        }
        
        m_mediaPlayer->setSource(QUrl::fromLocalFile(filePath));
        m_mediaPlayer->play();
    }
    
    update();
}

void AlbumWidget::onJoystickLeftPressed() {
    if (!isVisible()) return;
    if (m_showDeleteConfirm) return;
    
    // 如果正在播放，切换视频前先停止
    if (m_mediaPlayer && m_mediaPlayer->playbackState() == QMediaPlayer::PlayingState) {
        m_mediaPlayer->stop();
    }
    
    if (!m_isVideoMode && m_currentIndex > 0) { m_currentIndex--; loadCurrentImage(); }
    else if (m_isVideoMode && m_currentIndex > 0) navigateVideo(m_currentIndex - 1);
    update();
}

void AlbumWidget::onJoystickRightPressed() {
    if (!isVisible()) return;
    if (m_showDeleteConfirm) return;
    
    // 如果正在播放，切换视频前先停止
    if (m_mediaPlayer && m_mediaPlayer->playbackState() == QMediaPlayer::PlayingState) {
        m_mediaPlayer->stop();
    }
    
    if (!m_isVideoMode && m_currentIndex < m_files.size() - 1) { m_currentIndex++; loadCurrentImage(); }
    else if (m_isVideoMode && m_currentIndex < m_files.size() - 1) navigateVideo(m_currentIndex + 1);
    update();
}

void AlbumWidget::onJoystickCenterPressed() {
    if (!isVisible()) return;
    if (m_showDeleteConfirm) {
        executeDelete();
        return;
    }
    if (m_isVideoMode && m_currentIndex >= 0) {
        launchMpv();
    }
}

//  keyPressEvent
void AlbumWidget::keyPressEvent(QKeyEvent* event) {
    if (m_files.isEmpty()) {
        if (event->key() == Qt::Key_Escape) emit closed();
        return;
    }
    switch (event->key()) {
    case Qt::Key_Left:
        onJoystickLeftPressed();
        break;
    case Qt::Key_Right:
        onJoystickRightPressed();
        break;
    case Qt::Key_Enter:
    case Qt::Key_Return:
        onJoystickCenterPressed();
        break;
    case Qt::Key_Escape:
    case Qt::Key_Back:
    case Qt::Key_Delete:
        emit closed();
        break;
    default:
        QWidget::keyPressEvent(event);
    }
}


//  鼠标事件
void AlbumWidget::mousePressEvent(QMouseEvent* event) {
    m_touchStartX = event->pos().x();
    m_lastPanPos  = event->pos();
    m_isDragging  = true;
}

void AlbumWidget::mouseMoveEvent(QMouseEvent* event) {
    if (m_isZoomed && m_isDragging && !m_isVideoMode) {
        QPoint delta = event->pos() - m_lastPanPos;
        m_zoomOffset += delta;
        m_lastPanPos = event->pos();
        
        QSize sz = m_currentImage.size();
        sz.scale(size(), Qt::KeepAspectRatio);
        int scaledW = sz.width() * 2;
        int scaledH = sz.height() * 2;
        
        if (scaledW > width()) {
            m_zoomOffset.setX(qBound(width() - scaledW, m_zoomOffset.x(), 0));
        } else {
            m_zoomOffset.setX((width() - scaledW) / 2);
        }
        if (scaledH > height()) {
            m_zoomOffset.setY(qBound(height() - scaledH, m_zoomOffset.y(), 0));
        } else {
            m_zoomOffset.setY((height() - scaledH) / 2);
        }
        update();
    }
}

void AlbumWidget::mouseReleaseEvent(QMouseEvent* event) {
    if (!m_isDragging) return;
    m_isDragging = false;
    int diffX = event->pos().x() - m_touchStartX;

    if (m_isZoomed && !m_isVideoMode) {
        return; // Disable swiping and clicking when zoomed in
    }

    if (qAbs(diffX) < 20) {
        // 轻点（非滑动）
        if (m_showDeleteConfirm) {
            if (m_confirmRect.contains(event->pos())) {
                executeDelete();
            } else {
                m_showDeleteConfirm = false;
                update();
            }
            return;
        }

        if (!m_isVideoMode) {
            int esz = 60;
            QRect exitR(width() - esz - 20, (height() - esz) / 2, esz, esz);
            if (exitR.contains(event->pos())) { emit closed(); return; }
        }
        else {
            // 退出按钮
            if (m_backRect.contains(event->pos())) { emit closed(); return; }

            // 上一条
            if (m_currentIndex >= 0 && m_prevRect.contains(event->pos())) {
                navigateVideo(m_currentIndex > 0 ? m_currentIndex - 1 : m_files.size() - 1);
                return;
            }

            // 下一条
            if (m_currentIndex >= 0 && m_nextRect.contains(event->pos())) {
                navigateVideo(m_currentIndex < m_files.size() - 1 ? m_currentIndex + 1 : 0);
                return;
            }

            // 播放按钮 (或者点击主画面中间)
            if (m_currentIndex >= 0 && (m_playRect.contains(event->pos()) || (event->pos().y() > 60 && event->pos().y() < height() - 100))) {
                launchMpv();
                return;
            }
        }
    } else {
        // 滑动翻页 (diffX >= 20 || diffX <= -20)
        if (m_files.isEmpty() || m_showDeleteConfirm) return; // 弹窗时禁用滑动

        if (diffX > 50) {
            // 向右滑动 (手指从左向右)：上一张
            onJoystickLeftPressed();
        } else if (diffX < -50) {
            // 向左滑动 (手指从右向左)：下一张
            onJoystickRightPressed();
        }
    }
}

void AlbumWidget::mouseDoubleClickEvent(QMouseEvent* event) {
    if (m_isVideoMode || m_currentImage.isNull() || m_showDeleteConfirm) return;

    if (m_isZoomed) {
        m_isZoomed = false;
    } else {
        m_isZoomed = true;
        QSize sz = m_currentImage.size();
        sz.scale(size(), Qt::KeepAspectRatio);
        
        int fitX = (width() - sz.width()) / 2;
        int fitY = (height() - sz.height()) / 2;
        
        int scaledW = sz.width() * 2;
        int scaledH = sz.height() * 2;
        
        QPoint clickPos = event->pos();
        
        int targetX = clickPos.x() - (clickPos.x() - fitX) * 2;
        int targetY = clickPos.y() - (clickPos.y() - fitY) * 2;
        
        m_zoomOffset.setX(targetX);
        m_zoomOffset.setY(targetY);
        
        if (scaledW > width()) {
            m_zoomOffset.setX(qBound(width() - scaledW, m_zoomOffset.x(), 0));
        } else {
            m_zoomOffset.setX((width() - scaledW) / 2);
        }
        if (scaledH > height()) {
            m_zoomOffset.setY(qBound(height() - scaledH, m_zoomOffset.y(), 0));
        } else {
            m_zoomOffset.setY((height() - scaledH) / 2);
        }
    }
    update();
}
