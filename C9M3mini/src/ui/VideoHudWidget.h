#pragma once
#include <QWidget>
#include <QPainter>
#include <QMouseEvent>
#include <QPainterPath>
#include <QTimer>

class VideoHudWidget : public QWidget {
    Q_OBJECT
public:
    explicit VideoHudWidget(QWidget* parent = nullptr);

public slots:
    void setCodecText(const QString& text);
    void setTimecode(const QString& timeStr);

    // 将来用于更新分辨率、帧率和存储空间
    // 更新分辨率、帧率和存储空间信息
    void setResFpsText(const QString& text);
    void setStorageText(const QString& text);

    void setRecordingState(bool isRecording);
    void setMemoryPoolStatus(float remainingPercentage);

    // 显示 Toast 消息 (timeoutMs=0 为持久显示)
    void showToast(const QString& msg, int timeoutMs = 2000);
    // 立即清除 Toast
    void hideToast();

signals:
    void codecSwitchRequested();
    void fpsSwitchRequested(float newFps);
    void showVariableFpsUiRequested();

public:
    void setVariableFpsMode(bool mode) { m_isVariableFpsMode = mode; }

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;

private:
    bool m_isRecording = false;
    float m_memoryRemaining = 1.0f;
    QString m_codecText = "CinemaDNG";
    QString m_timecode = "00:00:00";
    
    QString m_toastMsg;
    QTimer m_toastTimer;

    // 占位字符
    QString m_resFpsText = "24p";
    QString m_storageText = "128G";

    QRect m_codecRect; // 编码格式触控区域
    QRect m_resFpsRect; // 帧率触控区域
    
    // 支持的帧率列表
    const QList<float> m_supportedFps = {23.98f, 24.0f, 25.0f, 29.97f, 30.0f, 50.0f, 59.94f, 60.0f};
    int m_currentFpsIndex = 1; // 默认 24.0f

    // 绘制带黑边的实心文字
    void drawStrokeText(QPainter& p, int x, int y, const QString& text,
        const QFont& font, const QColor& fillColor,
        Qt::Alignment align, QRect* outRect = nullptr);

    bool m_isVariableFpsMode = false;
};