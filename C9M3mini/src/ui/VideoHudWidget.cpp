#include "VideoHudWidget.h"

VideoHudWidget::VideoHudWidget(QWidget* parent) : QWidget(parent) {
    // 允许鼠标事件，在 mousePressEvent 中按需透传
    setAttribute(Qt::WA_TransparentForMouseEvents, false);

    connect(&m_toastTimer, &QTimer::timeout, this, [=]() {
        m_toastMsg.clear();
        update();
    });
}

void VideoHudWidget::setRecordingState(bool isRecording) {
    if (m_isRecording == isRecording) return;
    m_isRecording = isRecording;
    // 重置内存状态
    if (isRecording) {
        m_memoryRemaining = 1.0f;
    }
    update(); // 触发重绘
}

void VideoHudWidget::setMemoryPoolStatus(float remainingPercentage) {
    m_memoryRemaining = remainingPercentage;
    update();
}

void VideoHudWidget::setCodecText(const QString& text) {
    m_codecText = text;
    update();
}

void VideoHudWidget::setTimecode(const QString& timeStr) {
    m_timecode = timeStr;
    update();
}

void VideoHudWidget::setResFpsText(const QString& text) {
    m_resFpsText = text;
    update();
}

void VideoHudWidget::setStorageText(const QString& text) {
    m_storageText = text;
    update();
}

void VideoHudWidget::showToast(const QString& msg, int timeoutMs) {
    m_toastMsg = msg;
    update();
    if (timeoutMs > 0) {
        m_toastTimer.start(timeoutMs); // 超时后自动清除
    } else {
        m_toastTimer.stop(); // 持久显示，不启动计时器
    }
}

void VideoHudWidget::hideToast() {
    m_toastTimer.stop();
    m_toastMsg.clear();
    update();
}

// 描边文字绘制器
void VideoHudWidget::drawStrokeText(QPainter& p, int x, int y, const QString& text,
    const QFont& font, const QColor& fillColor,
    Qt::Alignment align, QRect* outRect) {
    p.setFont(font);
    QFontMetrics fm(font);
    int textW = fm.horizontalAdvance(text);
    int textH = fm.height();

    int drawX = x;
    if (align & Qt::AlignHCenter) {
        drawX = x - textW / 2;
    }
    else if (align & Qt::AlignRight) {
        drawX = x - textW;
    }

    // Y坐标基准线对齐
    int drawY = y + fm.ascent();

    QPainterPath path;
    path.addText(drawX, drawY, font, text);

    // 1像素的纯黑边框
    p.setPen(QPen(Qt::black, 1));
    p.setBrush(fillColor);
    p.drawPath(path);

    // 如果需要记录点击区域
    if (outRect) {
        *outRect = QRect(drawX, y, textW, textH);
    }
}

void VideoHudWidget::paintEvent(QPaintEvent* event) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    // 1. 索尼红色 Tally 边框
    if (m_isRecording) {
        p.setPen(QPen(Qt::red, 8));
        p.setBrush(Qt::NoBrush);
        p.drawRect(rect());
    }

    // 准备字体
    QFont topFont("Arial", 20, QFont::Bold);
    int topY = 3; // 距离顶部的统一高度

    // 居中：STBY / REC (第一行) 和 时间码 (第二行)
    QString stateText;
    QColor stateColor;
    if (m_isRecording) {
        stateText = "● REC";
        stateColor = Qt::red;
    } else {
        stateText = "STBY";
        stateColor = QColor(0, 255, 0); // 待机荧光绿
    }
    // 绘制状态
    drawStrokeText(p, width() / 2, topY, stateText, topFont, stateColor, Qt::AlignHCenter);
    
    // 绘制时间码
    QFont timecodeFont("Arial", 18, QFont::Bold);
    drawStrokeText(p, width() / 2, topY + 23, m_timecode, timecodeFont, Qt::white, Qt::AlignHCenter);

    // 左侧: 编码格式
    drawStrokeText(p, 90, topY, m_codecText, topFont, Qt::white, Qt::AlignLeft, &m_codecRect);

    // 右侧: 分辨率与帧率
    drawStrokeText(p, width() * 0.75, topY, m_resFpsText, topFont, Qt::white, Qt::AlignHCenter, &m_resFpsRect);

    // 右侧: 存储空间
    drawStrokeText(p, width() - 5, topY, m_storageText, topFont, Qt::white, Qt::AlignRight);

    // 绘制内存池容量指示条
    if (m_isRecording || m_memoryRemaining < 0.999f) {
        int barW = 2;
        int barH = 128; 
        int barX = 15; 
        int barY = (height() - barH) / 2;

        p.setPen(QPen(Qt::black, 1));
        p.setBrush(Qt::NoBrush);
        p.drawRect(barX - 1, barY - 1, barW + 1, barH + 1);

        int fillH = static_cast<int>(m_memoryRemaining * barH);
        int fillY = barY + (barH - fillH);

        p.setPen(Qt::NoPen);
        if (m_memoryRemaining < 0.2f) {
            p.setBrush(Qt::red);
        } else {
            p.setBrush(QColor(0, 255, 0));
        }
        if (fillH > 0) {
            p.drawRect(barX, fillY, barW, fillH);
        }
    }

    // 绘制 Toast
    if (!m_toastMsg.isEmpty()) {
        int cx = width() / 2;
        int cy = height() / 2 - 20;

        // 画多行文字 (白色填充，drawStrokeText 内置黑色描边)
        QFont toastFont("Arial", 22, QFont::Bold); 
        QStringList lines = m_toastMsg.split('\n');
        int textY = cy;
        for (const QString& line : lines) {
            drawStrokeText(p, cx, textY, line, toastFont, Qt::white, Qt::AlignHCenter);
            textY += 30; // 行距
        }
    }
}

// 处理点击事件 (替代原本的 QPushButton)
void VideoHudWidget::mousePressEvent(QMouseEvent* event) {
    // 触控区域外扩 15 像素
    QRect hitCodec = m_codecRect.adjusted(-15, -15, 15, 15);
    QRect hitFps = m_resFpsRect.adjusted(-15, -15, 15, 15);

    if (hitCodec.contains(event->pos())) {
        // 如果点中了“编码格式”文字
        emit codecSwitchRequested();
        event->accept(); // 拦截事件
    }
    else if (hitFps.contains(event->pos())) {
        if (m_isRecording) {
            showToast("录制中无法切换帧率", 1500);
            event->accept();
            return;
        }

        if (m_isVariableFpsMode) {
            emit showVariableFpsUiRequested();
        } else {
            m_currentFpsIndex = (m_currentFpsIndex + 1) % m_supportedFps.size();
            float fps = m_supportedFps[m_currentFpsIndex];
            
            // 生成保留位数的字符 (比如 23.98p, 24p)
            if (fps == 24.0f || fps == 25.0f || fps == 30.0f || fps == 50.0f || fps == 60.0f) {
                setResFpsText(QString("%1p").arg(static_cast<int>(fps)));
            } else {
                setResFpsText(QString("%1p").arg(fps, 0, 'f', 2));
            }
            emit fpsSwitchRequested(fps);
        }
        event->accept();
    }
    else {
        // 空白区域事件透传，交由底层处理测光/对焦
        event->ignore();
    }
}