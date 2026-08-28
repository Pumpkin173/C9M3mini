#pragma once

#include <QWidget>
#include <QFileInfoList>
#include <QImage>
#include <QMap>
#include <QList>
#include <QPointer>
#include <functional>
#include <QPainter>

// 专门用于在相册上层绘制 UI 的覆盖控件
class AlbumOverlayWidget : public QWidget {
public:
    AlbumOverlayWidget(QWidget* parent) : QWidget(parent) {
        setAttribute(Qt::WA_TransparentForMouseEvents);
    }
    
    // 让父控件把画图的具体逻辑传进来
    std::function<void(QPainter&)> paintCallback;
    
    void paintEvent(QPaintEvent*) override {
        if (paintCallback) {
            QPainter p(this);
            p.setRenderHint(QPainter::Antialiasing);
            paintCallback(p);
        }
    }
};

class QProcess;
class QMediaPlayer;
class QAudioOutput;
class QGraphicsView;
class QGraphicsScene;
class QGraphicsVideoItem;

//  视频文件元数据（由 ffprobe + ffmpeg 异步提取）
struct VideoMeta {
    bool loaded  = false;
    bool loading = false;

    QString codec;
    int     width    = 0;
    int     height   = 0;
    QString fps;
    QString bitrate;
    QString chroma;
    int     bitDepth = 8;
    QString duration;
    QImage  thumbnail;
};

// AlbumWidget 
class AlbumWidget : public QWidget {
    Q_OBJECT
public:
    explicit AlbumWidget(QWidget* parent = nullptr);
    ~AlbumWidget();

    // 打开照片相册（扫描 /media/pumpkin/FILM/Photo）
    void openAlbum();

    // 打开视频相册（扫描 /media/pumpkin/FILM/Video 含子目录，定位到最新文件）
    void openVideoAlbum();

public slots:
    // 停止当前 mpv 播放（绑定到 Fn 实体键）
    void stopPlayback();

    // 硬件按钮触发槽
    void onDeletePressed();
    void onJoystickCenterPressed();
    void onJoystickLeftPressed();
    void onJoystickRightPressed();

signals:
    void closed();

protected:
    void paintEvent(QPaintEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;

private:
    void loadCurrentImage();
    void requestVideoMeta(int index);
    void navigateVideo(int newIndex);   // 统一导航并触发预加载
    void launchMpv(); // 抽离出的 mpv 启动逻辑

    void drawVideoBackground(QPainter& p, const VideoMeta& meta);
    void drawCinemaOverlay(QPainter& p, const VideoMeta& meta, const QFileInfo& fi);
    void drawDeleteConfirmOverlay(QPainter& p);

    void executeDelete();

    static void drawStrokeText(QPainter& p, int x, int y,
                               const QString& text, const QFont& font,
                               const QColor& fillColor, double strokeWidth = 1.5);

    // 状态 
    bool          m_isVideoMode  = false;
    QFileInfoList m_files;
    int           m_currentIndex = 0;
    QImage        m_currentImage;
    bool          m_isLoading    = false;
    int           m_touchStartX  = 0;
    bool          m_isDragging   = false;
    
    // Zoom State
    bool          m_isZoomed     = false;
    QPoint        m_zoomOffset;
    QPoint        m_lastPanPos;
    
    // Deletion State
    bool          m_showDeleteConfirm = false;
    QRect         m_confirmRect;

    // UI Click Regions
    QRect         m_playRect;
    QRect         m_prevRect;
    QRect         m_nextRect;
    QRect         m_backRect;

    // 元数据缓存（index → VideoMeta）
    QMap<int, VideoMeta> m_metaCache;

    // 原生的 Qt Multimedia 播放组件
    QMediaPlayer* m_mediaPlayer;
    QAudioOutput* m_audioOutput;
    
    QGraphicsView* m_videoView;
    QGraphicsScene* m_videoScene;
    QGraphicsVideoItem* m_videoItem;
    AlbumOverlayWidget* m_overlayWidget;

    // 正在运行的子进程列表（析构时 kill）
    QList<QProcess*> m_activeProcs;
};