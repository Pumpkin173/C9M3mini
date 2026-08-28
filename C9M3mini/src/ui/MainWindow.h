#pragma once
#include <QMainWindow>
#include <QLabel>
#include <QPushButton>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QPainter>
#include <QPainterPath>
#include <QMouseEvent> 
#include <QMutex>
#include <atomic>
#include "../core/CameraEngine.h" 
#include "../core/VideoRecorder.h"
#include "../hardware/HardwareManager.h"
#include "../hardware/PioTriggerController.h"
#include "../ui/AlbumWidget.h"
#include "../ui/VideoHudWidget.h"
#include "../ui/SettingsMenu.h"

// 描边按钮组件
class StrokeButton : public QPushButton {
    int m_offsetY = 0;
    int m_offsetX = 0;

public:
    using QPushButton::QPushButton; // 继承构造函数
    void setTextOffset(int x,int y) {
        m_offsetY = y;
        m_offsetX = x;
        update(); // 触发重绘
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        // 只有按下时才画半透明背景
        if (isDown()) {
            p.setBrush(QColor(255, 255, 255, 50));
        }
        else {
            p.setBrush(Qt::NoBrush);
        }

        // 按钮的外边框也加个黑边，增加对比度
        p.setPen(QPen(Qt::black, 3));
        p.drawRoundedRect(rect().adjusted(2, 2, -2, -2), 8, 8);

        p.setPen(QPen(Qt::white, 2));
        p.drawRoundedRect(rect().adjusted(2, 2, -2, -2), 8, 8);

        // 绘制带描边的文字
        p.setFont(font());
        QFontMetrics fm(font());

        // 计算居中位置
        int textWidth = fm.horizontalAdvance(text());
        int textX = (width() - textWidth) / 2 + m_offsetX;
        int textY = (height() + fm.ascent() - fm.descent()) / 2 + m_offsetY;

        QPainterPath path;
        path.addText(textX, textY, font(), text());

        // 描边
        p.setPen(QPen(Qt::black, 1));
        p.setBrush(Qt::white); // 填充白色
        p.drawPath(path);
    }
};

// 曝光补偿控制组件
class EvControlWidget : public QWidget {
    Q_OBJECT
public:
    explicit EvControlWidget(QWidget* parent = nullptr) : QWidget(parent) {
        setFixedSize(540, 120);
        setStyleSheet("background-color: rgba(0, 0, 0, 80); border-radius: 10px;");
        QHBoxLayout* layout = new QHBoxLayout(this);
        layout->setContentsMargins(10, 5, 10, 5);
        QFont btnFont("Arial", 40, QFont::Bold);

        // 减号按钮
        btnMinus = new StrokeButton("-", this);
        btnMinus->setFixedSize(80, 60);
        btnMinus->setFont(btnFont);
        btnMinus->setAutoRepeat(true);
        btnMinus->setAutoRepeatDelay(300);       // 自动重复延迟 (ms)
        btnMinus->setAutoRepeatInterval(150);    // 自动重复间隔 (ms)
        layout->addWidget(btnMinus);
        layout->addStretch();

        // 加号按钮
        btnPlus = new StrokeButton("+", this);
        btnPlus->setFixedSize(80, 60);
        btnPlus->setFont(btnFont);
        btnPlus->setAutoRepeat(true);
        btnPlus->setAutoRepeatDelay(300);
        btnPlus->setAutoRepeatInterval(150);
        layout->addWidget(btnPlus);

        connect(btnMinus, &QPushButton::clicked, this, [=]() { changeEv(-0.333f); });
        connect(btnPlus, &QPushButton::clicked, this, [=]() { changeEv(0.333f); });
    }

    void setCurrentEv(float ev) {
        currentEv = ev;
        update();
    }

signals:
    void evValueChanged(float newEv);

protected:
    float currentEv = 0.0f;
    StrokeButton* btnMinus;
    StrokeButton* btnPlus;

    void changeEv(float delta) {
        float newVal = currentEv + delta;
        if (newVal > 5.0f) newVal = 5.0f;
        if (newVal < -5.0f) newVal = -5.0f;
        emit evValueChanged(newVal);
    }

    // 加描边
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);

        int startX = 120;
        int endX = width() - 120;
        int rulerWidth = endX - startX;
        int y = height() / 2 + 20;

        for (int i = -5; i <= 5; ++i) {
            float ratio = (float)(i + 5) / 10.0f;
            int x = startX + (int)(ratio * rulerWidth);
            bool isSelected = (std::abs(currentEv - i) < 0.2f);

            int tickH = isSelected ? 15 : 8;
            int lineTop = y - tickH - 8;
            int lineBottom = y - 5;

            p.setPen(QPen(Qt::black, 4));
            p.drawLine(x, lineTop, x, lineBottom);

            p.setPen(QPen(isSelected ? QColor(255, 204, 0) : Qt::white, 2));
            p.drawLine(x, lineTop, x, lineBottom);

            QFont font("Arial", isSelected ? 25 : 20, isSelected ? QFont::Bold : QFont::Normal);
            p.setFont(font);

            QString text = QString::number(i);
            if (i > 0) text = "+" + text;

            QFontMetrics fm(font);
            int txtW = fm.horizontalAdvance(text);
            int txtX = x - txtW / 2;
            int txtY = y - 7 + fm.ascent(); 

            QPainterPath path;
            path.addText(txtX, txtY, font, text);

            p.setPen(QPen(Qt::black, 1));
            p.setBrush(isSelected ? QColor(255, 204, 0) : Qt::white);
            p.drawPath(path);
        }

        // 画游标
        float exactRatio = (currentEv + 5.0f) / 10.0f;
        int cursorX = startX + (int)(exactRatio * rulerWidth);

        QPolygon triangle;
        triangle << QPoint(cursorX, y - 32)
            << QPoint(cursorX - 8, y - 42) 
            << QPoint(cursorX + 8, y - 42);

        p.setPen(QPen(Qt::black, 1));
        p.setBrush(QColor(255, 204, 0));
        p.drawPolygon(triangle);
    }
};

// 测光模式控制组件
class MeteringButton : public QPushButton {
public:
    int modeIndex; 
    bool isSelected;

    MeteringButton(int mode, QWidget* parent = nullptr) : QPushButton(parent), modeIndex(mode), isSelected(false) {
        setFixedSize(80, 60); 
        setStyleSheet("background: transparent;"); 
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);

        QRect rect = this->rect().adjusted(4, 4, -4, -4);

        QColor bgColor = isSelected ? QColor(50, 40, 0, 200) : QColor(0, 0, 0, 100);
        QColor strokeColor = isSelected ? QColor(255, 204, 0) : Qt::white;

        p.setBrush(bgColor);
        p.setPen(QPen(Qt::black, 3));
        p.drawRoundedRect(rect, 8, 8);

        p.setPen(QPen(strokeColor, 2));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(rect, 8, 8);

        QPainterPath path;
        int cx = width() / 2;
        int cy = height() / 2;

        if (modeIndex == 0) { 
            int r = 16;
            path.addRect(cx - r, cy - r, 2 * r, 2 * r);
            path.moveTo(cx, cy - r); path.lineTo(cx, cy + r);
            path.moveTo(cx - r, cy); path.lineTo(cx + r, cy);
        }
        else if (modeIndex == 1) { 
            path.addEllipse(QPoint(cx, cy), 12, 12);
            path.moveTo(cx - 20, cy - 10); path.quadTo(cx - 24, cy, cx - 20, cy + 10);
            path.moveTo(cx + 20, cy - 10); path.quadTo(cx + 24, cy, cx + 20, cy + 10);
        }
        else if (modeIndex == 2) { 
            path.addEllipse(QPoint(cx, cy), 6, 6);
            int r = 16; int len = 6;
            path.moveTo(cx - r, cy - r); path.lineTo(cx - r + len, cy - r);
            path.moveTo(cx - r, cy - r); path.lineTo(cx - r, cy - r + len);
            path.moveTo(cx + r, cy - r); path.lineTo(cx + r - len, cy - r);
            path.moveTo(cx + r, cy - r); path.lineTo(cx + r, cy - r + len);
            path.moveTo(cx - r, cy + r); path.lineTo(cx - r + len, cy + r);
            path.moveTo(cx - r, cy + r); path.lineTo(cx - r, cy + r - len);
            path.moveTo(cx + r, cy + r); path.lineTo(cx + r - len, cy + r);
            path.moveTo(cx + r, cy + r); path.lineTo(cx + r, cy + r - len);
        }

        QBrush fillBrush;

        if (modeIndex == 2) {
            fillBrush = QBrush(strokeColor);
        }
        else {
            QColor fillColor = strokeColor;
            fillColor.setAlpha(50); 
            fillBrush = QBrush(fillColor);
        }

        p.setPen(QPen(Qt::black, 4));
        p.setBrush(fillBrush);
        p.drawPath(path);

        // 第二层：亮色芯
        p.setPen(QPen(strokeColor, 2));
        p.setBrush(fillBrush);
        p.drawPath(path);
    }
};

class MeteringControlWidget : public QWidget {
    Q_OBJECT
public:
    explicit MeteringControlWidget(QWidget* parent = nullptr) : QWidget(parent) {
        setFixedSize(320, 100);

        setStyleSheet("background-color: rgba(0, 0, 0, 80); border-radius: 15px;");

        QVBoxLayout* mainLayout = new QVBoxLayout(this);
        mainLayout->setContentsMargins(10, 5, 10, 5); 
        mainLayout->setSpacing(10); 

        QHBoxLayout* btnLayout = new QHBoxLayout();
        btnLayout->setSpacing(20);
        btnLayout->setContentsMargins(0, 0, 0, 0);
        for (int i = 0; i < 3; ++i) {
            btns[i] = new MeteringButton(i, this);
            btnLayout->addWidget(btns[i]);

            connect(btns[i], &QPushButton::clicked, this, [=]() {
                setSelection(i);
                emit modeSelected(i);
                });
        }

        mainLayout->addLayout(btnLayout);

        QLabel* label = new QLabel(QStringLiteral("测光模式"), this);
        label->setStyleSheet(
            "QLabel {"
            "   color: white;"
            "   font-family: 'WenQuanYi Zen Hei', 'SimHei', 'Sans Serif';"
            "   font-size: 30px;"
            "   font-weight: bold;"
            "   background: transparent;"
            "}"
        );
        label->setContentsMargins(85, 0, 0, 0);
        label->setFixedHeight(30); 

        mainLayout->addWidget(label);
    }

    void setSelection(int index) {
        for (int i = 0; i < 3; ++i) {
            btns[i]->isSelected = (i == index);
            btns[i]->update(); 
        }
    }

signals:
    void modeSelected(int modeIndex); 

private:
    MeteringButton* btns[3];
};

// 白平衡控制组件
class AwbControlWidget : public QWidget {
    Q_OBJECT
public:
    explicit AwbControlWidget(QWidget* parent = nullptr) : QWidget(parent) {
        setFixedSize(540, 120);
        setStyleSheet("background-color: rgba(0, 0, 0, 80); border-radius: 10px;");

        QHBoxLayout* layout = new QHBoxLayout(this);
        layout->setContentsMargins(10, 5, 10, 5);

        btnAuto = new StrokeButton("AUTO", this);
        btnAuto->setFixedSize(110, 60);
        btnAuto->setFont(QFont("Arial", 22, QFont::Bold));
        layout->addWidget(btnAuto);

        layout->addStretch();
        
        QFont btnFont("Arial", 40, QFont::Bold);

        btnMinus = new StrokeButton("-", this);
        btnMinus->setFixedSize(60, 60);
        btnMinus->setFont(btnFont);
        btnMinus->setAutoRepeat(true);
        btnMinus->setAutoRepeatDelay(300);
        btnMinus->setAutoRepeatInterval(100);
        layout->addWidget(btnMinus);
        
        btnPlus = new StrokeButton("+", this);
        btnPlus->setFixedSize(60, 60);
        btnPlus->setFont(btnFont);
        btnPlus->setAutoRepeat(true);
        btnPlus->setAutoRepeatDelay(300);
        btnPlus->setAutoRepeatInterval(100);
        layout->addWidget(btnPlus);

        connect(btnAuto, &QPushButton::clicked, this, [=]() { setAuto(true); });
        connect(btnMinus, &QPushButton::clicked, this, [=]() { setAuto(false); changeTemp(-100); });
        connect(btnPlus, &QPushButton::clicked, this, [=]() { setAuto(false); changeTemp(100); });
    }

    void setCurrentMode(bool isAuto, int temp) {
        m_isAuto = isAuto;
        m_colorTemp = temp;
        update();
    }

signals:
    void awbChanged(bool isAuto, int tempK);

protected:
    bool m_isAuto = true;
    int m_colorTemp = 5500;
    StrokeButton* btnAuto;
    StrokeButton* btnMinus;
    StrokeButton* btnPlus;

    void setAuto(bool a) {
        m_isAuto = a;
        emit awbChanged(m_isAuto, m_colorTemp);
        update();
    }

    void changeTemp(int delta) {
        m_colorTemp += delta;
        if (m_colorTemp > 9500) m_colorTemp = 9500;
        if (m_colorTemp < 2500) m_colorTemp = 2500;
        emit awbChanged(m_isAuto, m_colorTemp);
        update();
    }

    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);

        int startX = 140;
        int endX = width() - 150;
        int rulerWidth = endX - startX;
        int y = height() / 2 + 20;

        for (int i = 0; i <= 7; ++i) { 
            float ratio = (float)i / 7.0f;
            int x = startX + (int)(ratio * rulerWidth);
            int tempVal = 2500 + i * 1000;
            bool isSelected = (!m_isAuto && std::abs(m_colorTemp - tempVal) < 500);

            int tickH = isSelected ? 15 : 8;
            int lineTop = y - tickH - 8;
            int lineBottom = y - 5;

            p.setPen(QPen(Qt::black, 4));
            p.drawLine(x, lineTop, x, lineBottom);

            p.setPen(QPen(isSelected ? QColor(255, 204, 0) : Qt::white, 2));
            p.drawLine(x, lineTop, x, lineBottom);

            if (i % 2 == 0) {
                QFont font("Arial", 20, isSelected ? QFont::Bold : QFont::Normal);
                p.setFont(font);
                QString text = QString::number(tempVal / 100) + "C";
                
                QFontMetrics fm(font);
                int txtW = fm.horizontalAdvance(text);
                int txtX = x - txtW / 2;
                int txtY = y - 2 + fm.ascent(); 

                QPainterPath path;
                path.addText(txtX, txtY, font, text);

                p.setPen(QPen(Qt::black, 1));
                p.setBrush(isSelected ? QColor(255, 204, 0) : Qt::white);
                p.drawPath(path);
            }
        }

        if (!m_isAuto) {
            float exactRatio = (m_colorTemp - 2500.0f) / 7000.0f;
            int cursorX = startX + (int)(exactRatio * rulerWidth);
            QPolygon triangle;
            triangle << QPoint(cursorX, y - 32)
                << QPoint(cursorX - 8, y - 42)
                << QPoint(cursorX + 8, y - 42);

            p.setPen(QPen(Qt::black, 1));
            p.setBrush(QColor(255, 204, 0));
            p.drawPolygon(triangle);

            QFont font("Arial", 25, QFont::Bold);
            p.setFont(font);
            QString text = QString::number(m_colorTemp) + "K";
            QFontMetrics fm(font);
            int txtW = fm.horizontalAdvance(text);
            QPainterPath path;
            path.addText(startX + rulerWidth/2 - txtW/2, 35, font, text);
            p.setPen(QPen(Qt::black, 1));
            p.setBrush(QColor(255, 204, 0));
            p.drawPath(path);
        } else {
            QFont font("Arial", 25, QFont::Bold);
            p.setFont(font);
            QString text = "AWB AUTO";
            QFontMetrics fm(font);
            int txtW = fm.horizontalAdvance(text);
            QPainterPath path;
            path.addText(startX + rulerWidth/2 - txtW/2, 35, font, text);
            p.setPen(QPen(Qt::black, 1));
            p.setBrush(Qt::white);
            p.drawPath(path);
        }
    }
};

class IsoControlWidget : public QWidget {
    Q_OBJECT
public:
    explicit IsoControlWidget(QWidget* parent = nullptr) : QWidget(parent) {
        setFixedSize(540, 120);
        setStyleSheet("background-color: rgba(0, 0, 0, 80); border-radius: 10px;");
        QHBoxLayout* layout = new QHBoxLayout(this);
        layout->setContentsMargins(10, 5, 10, 5);

        btnAuto = new StrokeButton("AUTO", this);
        btnAuto->setFixedSize(110, 60);
        btnAuto->setFont(QFont("Arial", 22, QFont::Bold));
        layout->addWidget(btnAuto);

        layout->addStretch();
        
        QFont btnFont("Arial", 40, QFont::Bold);
        btnMinus = new StrokeButton("-", this);
        btnMinus->setFixedSize(60, 60);
        btnMinus->setFont(btnFont);
        btnMinus->setAutoRepeat(true);
        btnMinus->setAutoRepeatDelay(300);
        btnMinus->setAutoRepeatInterval(100);
        layout->addWidget(btnMinus);
        
        btnPlus = new StrokeButton("+", this);
        btnPlus->setFixedSize(60, 60);
        btnPlus->setFont(btnFont);
        btnPlus->setAutoRepeat(true);
        btnPlus->setAutoRepeatDelay(300);
        btnPlus->setAutoRepeatInterval(100);
        layout->addWidget(btnPlus);

        connect(btnAuto, &QPushButton::clicked, this, [=]() { setIso(0); });
        connect(btnMinus, &QPushButton::clicked, this, [=]() { changeIso(-1); });
        connect(btnPlus, &QPushButton::clicked, this, [=]() { changeIso(1); });
    }

    void setIso(int iso) {
        m_iso = iso;
        if (m_iso != 0) {
            auto it = std::find(isoList.begin(), isoList.end(), m_iso);
            if (it != isoList.end()) m_currentIndex = std::distance(isoList.begin(), it);
        }
        emit isoChanged(m_iso);
        update();
    }

signals:
    void isoChanged(int iso);

protected:
    int m_iso = 0; // 0 = Auto
    int m_currentIndex = 0;
    std::vector<int> isoList = {100, 125, 160, 200, 250, 320, 400, 500, 640, 800, 1000, 1250, 1600};
    StrokeButton* btnAuto;
    StrokeButton* btnMinus;
    StrokeButton* btnPlus;

public:
    void changeIso(int delta) {
        if (m_iso == 0) m_currentIndex = 3; // 默认跳到 200
        int newIdx = m_currentIndex + delta;
        if (newIdx < 0) newIdx = 0;
        if (newIdx >= (int)isoList.size()) newIdx = isoList.size() - 1;
        m_currentIndex = newIdx;
        setIso(isoList[newIdx]);
    }

    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);

        int startX = 140;
        int endX = width() - 150;
        int rulerWidth = endX - startX;
        int y = height() / 2 + 20;

        int numSteps = isoList.size() - 1;
        for (int i = 0; i <= numSteps; ++i) {
            float ratio = (float)i / numSteps;
            int x = startX + (int)(ratio * rulerWidth);
            bool isSelected = (m_iso == isoList[i]);

            int tickH = isSelected ? 15 : 8;
            int lineTop = y - tickH - 8;
            int lineBottom = y - 5;

            p.setPen(QPen(Qt::black, 4));
            p.drawLine(x, lineTop, x, lineBottom);

            p.setPen(QPen(isSelected ? QColor(255, 204, 0) : Qt::white, 2));
            p.drawLine(x, lineTop, x, lineBottom);
        }

        if (m_iso != 0) {
            float exactRatio = (float)m_currentIndex / numSteps;
            int cursorX = startX + (int)(exactRatio * rulerWidth);
            QPolygon triangle;
            triangle << QPoint(cursorX, y - 32) << QPoint(cursorX - 8, y - 42) << QPoint(cursorX + 8, y - 42);

            p.setPen(QPen(Qt::black, 1));
            p.setBrush(QColor(255, 204, 0));
            p.drawPolygon(triangle);

            QFont font("Arial", 25, QFont::Bold);
            p.setFont(font);
            QString text = QString::number(m_iso);
            QFontMetrics fm(font);
            int txtW = fm.horizontalAdvance(text);
            QPainterPath path;
            path.addText(startX + rulerWidth/2 - txtW/2, 35, font, text);
            p.setPen(QPen(Qt::black, 1));
            p.setBrush(QColor(255, 204, 0));
            p.drawPath(path);
        } else {
            QFont font("Arial", 25, QFont::Bold);
            p.setFont(font);
            QString text = "ISO AUTO";
            QFontMetrics fm(font);
            int txtW = fm.horizontalAdvance(text);
            QPainterPath path;
            path.addText(startX + rulerWidth/2 - txtW/2, 35, font, text);
            p.setPen(QPen(Qt::black, 1));
            p.setBrush(Qt::white);
            p.drawPath(path);
        }
    }
};

class ShutterControlWidget : public QWidget {
    Q_OBJECT
public:
    explicit ShutterControlWidget(QWidget* parent = nullptr) : QWidget(parent) {
        setFixedSize(540, 120);
        setStyleSheet("background-color: rgba(0, 0, 0, 80); border-radius: 10px;");
        QHBoxLayout* layout = new QHBoxLayout(this);
        layout->setContentsMargins(10, 5, 10, 5);

        layout->addStretch();
        
        QFont btnFont("Arial", 40, QFont::Bold);
        btnMinus = new StrokeButton("-", this);
        btnMinus->setFixedSize(60, 60);
        btnMinus->setFont(btnFont);
        btnMinus->setAutoRepeat(true);
        btnMinus->setAutoRepeatDelay(300);
        btnMinus->setAutoRepeatInterval(100);
        layout->addWidget(btnMinus);
        
        btnPlus = new StrokeButton("+", this);
        btnPlus->setFixedSize(60, 60);
        btnPlus->setFont(btnFont);
        btnPlus->setAutoRepeat(true);
        btnPlus->setAutoRepeatDelay(300);
        btnPlus->setAutoRepeatInterval(100);
        layout->addWidget(btnPlus);

        connect(btnMinus, &QPushButton::clicked, this, [=]() { changeShutter(-1); });
        connect(btnPlus, &QPushButton::clicked, this, [=]() { changeShutter(1); });
    }

    void setMaxShutter(int maxUs) {
        m_maxShutterUs = maxUs;
    }

    void setShutter(int shutterUs) {
        m_shutterUs = shutterUs;
        auto it = std::find(shutterList.begin(), shutterList.end(), m_shutterUs);
        if (it != shutterList.end()) {
            m_currentIndex = std::distance(shutterList.begin(), it);
        }
        
        // 强制限制快门不能低于 1/帧率
        if (m_shutterUs > m_maxShutterUs) {
            int maxIdx = shutterList.size() - 1;
            while (maxIdx >= 0 && shutterList[maxIdx] > m_maxShutterUs) maxIdx--;
            if (maxIdx < 0) maxIdx = 0;
            m_currentIndex = maxIdx;
            m_shutterUs = shutterList[maxIdx];
        }

        emit shutterChanged(m_shutterUs);
        update();
    }

signals:
    void shutterChanged(int shutterUs);

protected:
    int m_maxShutterUs = 100000000; // 默认极大值无限制
    int m_shutterUs = 1000000 / 60; 
    int m_currentIndex = 28;
    std::vector<int> shutterList = {
        30, 31, 40, 50, 62, 78, 100, 125, 156, 200, 250, 312, 400, 500, 625, 800, 1000,
        1250, 1562, 2000, 2500, 3125, 4000, 5000, 6250, 8000, 10000, 12500, 16666,
        20000, 25000, 33333, 40000, 50000, 66666, 76923, 100000, 125000, 166666,
        200000, 250000, 333333, 400000, 500000, 600000, 800000, 1000000, 1300000,
        1600000, 2000000, 2500000, 3200000, 4000000, 5000000, 6000000, 8000000,
        10000000, 13000000, 15500000
    };
    std::vector<QString> shutterStrs = {
        "1/33000", "1/32000", "1/25000", "1/20000", "1/16000", "1/12800", "1/10000", "1/8000",
        "1/6400", "1/5000", "1/4000", "1/3200", "1/2500", "1/2000", "1/1600",
        "1/1250", "1/1000", "1/800", "1/640", "1/500", "1/400", "1/320", "1/250",
        "1/200", "1/160", "1/125", "1/100", "1/80", "1/60", "1/50", "1/40",
        "1/30", "1/25", "1/20", "1/15", "1/13", "1/10", "1/8", "1/6", "1/5",
        "1/4", "1/3", "0.4\"", "0.5\"", "0.6\"", "0.8\"", "1\"", "1.3\"",
        "1.6\"", "2\"", "2.5\"", "3.2\"", "4\"", "5\"", "6\"", "8\"",
        "10\"", "13\"", "15.5\""
    };
    StrokeButton* btnMinus;
    StrokeButton* btnPlus;

public:
    void changeShutter(int delta) {
        int newIdx = m_currentIndex + delta;
        if (newIdx < 0) newIdx = 0;
        if (newIdx >= (int)shutterList.size()) newIdx = shutterList.size() - 1;
        m_currentIndex = newIdx;
        setShutter(shutterList[newIdx]);
    }

    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);

        int startX = 60;
        int endX = width() - 150;
        int rulerWidth = endX - startX;
        int y = height() / 2 + 20;

        int numSteps = shutterList.size() - 1;
        for (int i = 0; i <= numSteps; ++i) {
            float ratio = (float)i / numSteps;
            int x = startX + (int)(ratio * rulerWidth);
            bool isSelected = (m_currentIndex == i);

            int tickH = isSelected ? 15 : 8;
            int lineTop = y - tickH - 8;
            int lineBottom = y - 5;

            p.setPen(QPen(Qt::black, 4));
            p.drawLine(x, lineTop, x, lineBottom);

            p.setPen(QPen(isSelected ? QColor(255, 204, 0) : Qt::white, 2));
            p.drawLine(x, lineTop, x, lineBottom);
        }

        float exactRatio = (float)m_currentIndex / numSteps;
        int cursorX = startX + (int)(exactRatio * rulerWidth);
        QPolygon triangle;
        triangle << QPoint(cursorX, y - 32) << QPoint(cursorX - 8, y - 42) << QPoint(cursorX + 8, y - 42);

        p.setPen(QPen(Qt::black, 1));
        p.setBrush(QColor(255, 204, 0));
        p.drawPolygon(triangle);

        QFont font("Arial", 25, QFont::Bold);
        p.setFont(font);
        QString text = shutterStrs[m_currentIndex];
        QFontMetrics fm(font);
        int txtW = fm.horizontalAdvance(text);
        QPainterPath path;
        path.addText(startX + rulerWidth/2 - txtW/2, 35, font, text);
        p.setPen(QPen(Qt::black, 1));
        p.setBrush(QColor(255, 204, 0));
        p.drawPath(path);
    }
};

// 峰值对焦选项按钮
class PeakingButton : public QPushButton {
    bool   m_selected = false;
    QColor m_fgColor;
public:
    PeakingButton(const QString& text, QWidget* parent = nullptr, QColor fg = Qt::white)
        : QPushButton(text, parent), m_fgColor(fg) {
        setFixedSize(68, 46);
        setStyleSheet("background: transparent;");
    }
    void setSelected(bool sel) { m_selected = sel; update(); }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        QRect r = rect().adjusted(3, 3, -3, -3);
        QColor bg     = m_selected ? QColor(50,40,0,220) : QColor(0,0,0,120);
        QColor stroke = m_selected ? QColor(255,204,0)   : Qt::white;
        p.setBrush(bg);
        p.setPen(QPen(Qt::black, 1));
        p.drawRoundedRect(r, 6, 6);
        p.setPen(QPen(stroke, 1.5));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(r, 6, 6);
        QFont f("Arial", 14, QFont::Bold);
        p.setFont(f);
        QFontMetrics fm(f);
        int tw = fm.horizontalAdvance(text());
        int tx = (width()  - tw) / 2;
        int ty = (height() + fm.ascent() - fm.descent()) / 2;
        QPainterPath path;
        path.addText(tx, ty, f, text());
        p.setPen(QPen(Qt::black, 1));
        p.setBrush(m_selected ? QColor(255,204,0) : m_fgColor);
        p.drawPath(path);
    }
};

// 峰值对焦配置组件
class PeakingControlWidget : public QWidget {
    Q_OBJECT
public:
    explicit PeakingControlWidget(QWidget* parent = nullptr) : QWidget(parent) {
        setFixedSize(232, 200);
        setStyleSheet("background-color: rgba(0,0,0,80); border-radius: 15px;");
        QVBoxLayout* vl = new QVBoxLayout(this);
        vl->setContentsMargins(8, 8, 8, 6);
        vl->setSpacing(6);
        // Row 1: OFF | ON
        QHBoxLayout* r1 = new QHBoxLayout();
        r1->setSpacing(8);
        btnOff = new PeakingButton("OFF", this);
        btnOn  = new PeakingButton("ON",  this);
        btnOff->setFixedSize(102, 46);
        btnOn ->setFixedSize(102, 46);
        r1->addWidget(btnOff); r1->addWidget(btnOn);
        vl->addLayout(r1);
        // Row 2: RED | GRN | BLU
        QHBoxLayout* r2 = new QHBoxLayout();
        r2->setSpacing(6);
        btnRed = new PeakingButton("RED", this, QColor(255,80,80));
        btnGrn = new PeakingButton("GRN", this, QColor(80,220,80));
        btnBlu = new PeakingButton("BLU", this, QColor(80,160,255));
        for (auto* b : {btnRed, btnGrn, btnBlu}) r2->addWidget(b);
        vl->addLayout(r2);
        // Row 3: LOW | MID | HIGH
        QHBoxLayout* r3 = new QHBoxLayout();
        r3->setSpacing(6);
        btnLow  = new PeakingButton("LOW",  this);
        btnMid  = new PeakingButton("MID",  this);
        btnHigh = new PeakingButton("HIGH", this);
        for (auto* b : {btnLow, btnMid, btnHigh}) r3->addWidget(b);
        vl->addLayout(r3);
        
        // 默认：OFF, 绿, 中
        setSelection(false, 1, 1);
        connect(btnOff,  &QPushButton::clicked, this, [=]() { setEnabled_(false); });
        connect(btnOn,   &QPushButton::clicked, this, [=]() { setEnabled_(true);  });
        connect(btnRed,  &QPushButton::clicked, this, [=]() { setColor(0); });
        connect(btnGrn,  &QPushButton::clicked, this, [=]() { setColor(1); });
        connect(btnBlu,  &QPushButton::clicked, this, [=]() { setColor(2); });
        connect(btnLow,  &QPushButton::clicked, this, [=]() { setLevel(0); });
        connect(btnMid,  &QPushButton::clicked, this, [=]() { setLevel(1); });
        connect(btnHigh, &QPushButton::clicked, this, [=]() { setLevel(2); });
    }
    void setSelection(bool enabled, int color, int level) {
        m_enabled = enabled; m_color = color; m_level = level;
        btnOff->setSelected(!enabled); btnOn->setSelected(enabled);
        btnRed->setSelected(color==0);  btnGrn->setSelected(color==1); btnBlu->setSelected(color==2);
        btnLow->setSelected(level==0);  btnMid->setSelected(level==1); btnHigh->setSelected(level==2);
    }
signals:
    void peakingChanged(bool enabled, int color, int level);
private:
    bool m_enabled = false; int m_color = 1; int m_level = 1;
    PeakingButton *btnOff, *btnOn;
    PeakingButton *btnRed, *btnGrn, *btnBlu;
    PeakingButton *btnLow, *btnMid, *btnHigh;
    void setEnabled_(bool en) {
        m_enabled = en;
        btnOff->setSelected(!en); btnOn->setSelected(en);
        emit peakingChanged(m_enabled, m_color, m_level);
    }
    void setColor(int c) {
        m_color = c;
        btnRed->setSelected(c==0); btnGrn->setSelected(c==1); btnBlu->setSelected(c==2);
        emit peakingChanged(m_enabled, m_color, m_level);
    }
    void setLevel(int l) {
        m_level = l;
        btnLow->setSelected(l==0); btnMid->setSelected(l==1); btnHigh->setSelected(l==2);
        emit peakingChanged(m_enabled, m_color, m_level);
    }
};

class VariableFpsWidget : public QWidget {
    Q_OBJECT
public:
    explicit VariableFpsWidget(QWidget* parent = nullptr) : QWidget(parent) {
        setFixedSize(600, 350); 
        setStyleSheet("background-color: rgba(0,0,0,160); border-radius: 15px;");
    }

    void setFps(float fps) {
        m_fps = fps;
        update();
    }
    
    void setFineTuneMode(bool fine) {
        if (m_fineTune != fine) {
            m_fineTune = fine;
            update();
        }
    }

    void mousePressEvent(QMouseEvent* event) override {
        int y = 180;
        QRect leftBtnRect(20, y - 54, 100, 48);
        QRect rightBtnRect(width() - 120, y - 54, 100, 48);
        
        if (leftBtnRect.contains(event->pos())) {
            emit stepRequested(-10.0f);
        } else if (rightBtnRect.contains(event->pos())) {
            emit stepRequested(10.0f);
        }
    }

signals:
    void stepRequested(float delta);

protected:
    float m_fps = 24.0f;
    bool m_fineTune = false;

    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);

        // 绘制中央的 FPS 数字
        QFont numFont("Arial", 52, QFont::Bold);
        p.setFont(numFont);
        
        QString intPart = QString::number((int)m_fps);
        float frac = m_fps - (int)m_fps;
        QString fracPart = QString::asprintf(".%03d", (int)(frac * 1000 + 0.5f));
        
        QFontMetrics fm(numFont);
        int intW = fm.horizontalAdvance(intPart);
        int fracW = fm.horizontalAdvance(fracPart);
        int totalW = intW + fracW + 60; 
        
        int startX = (width() - totalW) / 2;
        int y = 180; 
        
        p.setPen(Qt::white);
        p.drawText(startX, y, intPart);
        startX += intW;
        
        if (m_fineTune) {
            QString fracPrefix = fracPart.left(3);
            QString fracLast = fracPart.right(1); 
            
            p.drawText(startX, y, fracPrefix);
            startX += fm.horizontalAdvance(fracPrefix);
            
            p.setPen(QColor(255, 204, 0)); 
            p.drawText(startX, y, fracLast);
            startX += fm.horizontalAdvance(fracLast);
            p.setPen(Qt::white);
        } else {
            p.setPen(QColor(200, 200, 200));
            p.drawText(startX, y, fracPart);
            startX += fracW;
            p.setPen(Qt::white);
        }
        
        QFont fpsFont("Arial", 24, QFont::Bold);
        p.setFont(fpsFont);
        p.drawText(startX + 15, y - 5, "FPS");

        // 触屏提示
        QFont touchFont("Arial", 14, QFont::Bold);
        p.setFont(touchFont);
        
        QRect leftBtnRect(20, y - 54, 100, 48);
        p.setBrush(QColor(60, 60, 60, 200));
        p.setPen(QPen(Qt::gray, 1));
        p.drawRoundedRect(leftBtnRect, 24, 24);
        p.setPen(Qt::white);
        p.drawText(leftBtnRect, Qt::AlignCenter, "◀ -10");
        
        QRect rightBtnRect(width() - 120, y - 54, 100, 48);
        p.setBrush(QColor(60, 60, 60, 200));
        p.setPen(QPen(Qt::gray, 1));
        p.drawRoundedRect(rightBtnRect, 24, 24);
        p.setPen(Qt::white);
        p.drawText(rightBtnRect, Qt::AlignCenter, "+10 ▶");
        
        // 旋钮图标 (Rotary Encoder)
        int rx = width() - 120;
        int ry = 260; 
        p.setPen(QPen(Qt::white, 2));
        p.setBrush(Qt::transparent);
        p.drawEllipse(QPoint(rx, ry), 25, 25);
        p.setBrush(Qt::white);
        p.drawEllipse(QPoint(rx, ry), 10, 10);
        
        // 左右弧形箭头
        p.setPen(QPen(Qt::gray, 2));
        p.drawArc(rx - 35, ry - 35, 70, 70, 30 * 16, 120 * 16); // 上弧
        p.drawArc(rx - 35, ry - 35, 70, 70, 210 * 16, 120 * 16); // 下弧
        
        QFont smallFont("Arial", 12, QFont::Bold);
        p.setFont(smallFont);
        p.setPen(Qt::gray);
        p.drawText(rx - 45, ry - 40, "-0.01");
        p.drawText(rx + 15, ry - 40, "+0.01");
        
        // 恢复 PRESS 为普通文本提示
        p.setPen(m_fineTune ? QColor(255, 204, 0) : Qt::gray);
        p.drawText(rx - 65, ry + 60, m_fineTune ? "FINE TUNE: ±0.001" : "PRESS FOR ±0.001");
        
        // 左侧五向摇杆图标 (Joystick)
        int jx = 120;
        int jy = 260; 
        
        p.setPen(QPen(Qt::white, 2));
        p.setBrush(Qt::transparent);
        p.drawEllipse(QPoint(jx, jy), 25, 25);
        p.setBrush(Qt::white);
        p.drawEllipse(QPoint(jx, jy), 8, 8);
        
        // 绘制上下左右的填充三角形
        p.setPen(Qt::NoPen);
        p.setBrush(Qt::gray);
        QPolygon polyUp; polyUp << QPoint(jx, jy-35) << QPoint(jx-8, jy-25) << QPoint(jx+8, jy-25); p.drawPolygon(polyUp);
        QPolygon polyDown; polyDown << QPoint(jx, jy+35) << QPoint(jx-8, jy+25) << QPoint(jx+8, jy+25); p.drawPolygon(polyDown);
        QPolygon polyLeft; polyLeft << QPoint(jx-35, jy) << QPoint(jx-25, jy-8) << QPoint(jx-25, jy+8); p.drawPolygon(polyLeft);
        QPolygon polyRight; polyRight << QPoint(jx+35, jy) << QPoint(jx+25, jy-8) << QPoint(jx+25, jy+8); p.drawPolygon(polyRight);
        
        p.setPen(Qt::gray);
        p.setFont(smallFont);
        p.drawText(jx - 15, jy - 45, "+1.0");
        p.drawText(jx - 15, jy + 55, "-1.0");
        p.drawText(jx - 75, jy + 5, "-0.1");
        p.drawText(jx + 45, jy + 5, "+0.1");
        
        p.setPen(Qt::gray);
        p.drawText(jx - 60, jy + 75, "PRESS TO HIDE");
    }
};

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    MainWindow(QWidget* parent = nullptr);
    ~MainWindow();
    enum ShootMode {
        Mode_Auto = 0,
        Mode_S = 1,
        Mode_M = 2,
        Mode_Empty1 = 3,
        Mode_Empty2 = 4,
        Mode_Empty3 = 5,
        Mode_Empty4 = 6,
        Mode_Empty5 = 7,
        Mode_Empty6 = 8,
        Mode_Empty7 = 9
    };
    // 静态拍摄格式
    enum PhotoFormat {
        Photo_DNG  = 0,  
        Photo_JPEG = 1,  
    };
    enum MeteringMode {
        Meter_Matrix = 0, // 矩阵
        Meter_Center = 1, // 中央
        Meter_Spot = 2  // 点
    };
    void setShootMode(ShootMode mode) { currentMode = mode; }
    void setMeteringMode(MeteringMode mode) { currentMeteringMode = mode; }

    float getTargetFps() const {
        if (m_isVideoMode) {
            return m_currentVideoFps;
        } else {
            if (m_manualShutterUs > 33333) {
                float fps = 1000000.0f / m_manualShutterUs;
                return std::max(10.0f, fps);
            }
            return 30.0f;
        }
    }

protected:
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    QPoint m_touchStartPos;
    bool m_uiHidden = false;

private slots:
    void updateFrame(QImage image, double shutterTimeUs, double analogGain, float targetEv);
    void onEvChanged(float newEv);
    void onMeteringChanged(int newMode);
    void onAwbChanged(bool isAuto, int tempK);
    void onModeSwitched(bool isVideo);
    void onDriveModeChanged(int modeIndex);
    void onRotaryEncoderScroll(int delta, bool switchPressed);
    void onIsoChanged(int iso);
    void onShutterChanged(int shutterUs);
    void onPeakingChanged(bool enabled, int color, int level);
    void onStillCapture();
    void onPhotoFormatCycled();
    void onMenuPressed();
    void onGridLineModeChanged(int mode);
    void startFlashCalibration();
    void onCalibrationTick();
    void finishCalibration(bool success);
    
    void onJoystickLeft();
    void onJoystickRight();
    void onJoystickUp();
    void onJoystickDown();
    void onJoystickCenter();
    void changeVariableFps(float delta);
    void showVariableFpsUi();
    
private:
    QLabel* viewfinder;
    CameraEngine* engine;
    ShootMode currentMode = Mode_Auto;
    MeteringMode currentMeteringMode = Meter_Matrix;
    EvControlWidget* evPopup;
    MeteringControlWidget* meterPopup;
    AwbControlWidget* awbPopup;
    IsoControlWidget* isoPopup;
    ShutterControlWidget* shutterPopup;
    VariableFpsWidget* m_variableFpsWidget = nullptr;
    HardwareManager* hwManager;
    PioTriggerController* pioController;
    AlbumWidget* m_albumWidget;
    SettingsMenu* m_settingsMenu;
    StrokeButton* btnPlayback; // 回放按钮
    VideoRecorder* m_videoRecorder;
    VideoHudWidget* m_videoHud;
    bool hasSpotPoint = false;
    bool m_isVideoMode = false;
    float m_currentVideoFps = 24.0f; // 视频模式下的当前帧率
    QRect evBoxRect;
    QRect meterIndicatorRect;
    QRect awbIndicatorRect;
    QRect shutterBoxRect;
    QRect isoBoxRect;
    QPoint currentSpotPos;
    QRect playbackBtnRect;     // 用于 updateFrame 定位
    
    float lastEv = 0.0f;
    bool lastIsAutoAwb = true;
    int lastColorTemp = 5500;
    int m_manualIso = 0; // 0 = Auto
    int m_manualShutterUs = 16666; // 1/60s default

    // 峰值对焦辅助
    PeakingControlWidget*     peakingPopup = nullptr;
    QRect  peakingIndicatorRect;
    bool   m_peakingEnabled = false;
    int    m_peakingColor   = 1;   // 0=红 1=绿 2=蓝
    int    m_peakingLevel   = 1;   // 0=低 1=中 2=高
    
    struct PeakingPoint {
        int16_t x;
        int16_t y;
    };
    std::vector<PeakingPoint> m_peakingPoints;
    int m_peakingFrameCount = 0;

    int    m_gridLineMode   = 1;   // 0=Off, 1=Rule of 3rds, 2=Square, 3=Center Cross

    // 闪光灯软件开关
    bool   m_flashEnabled   = false;            // 软件开关状态
    std::atomic<bool> m_flashGpioActive{false};  // GPIO17 当前是否为高电平
    QRect  flashIndicatorRect;                   // HUD 图标点击区域
    int    m_flashPreDelay  = 1200;              // 闪光预延时(us)，校准后动态调整

    // 闪光延时自动校准状态机
    enum CalibState {
        CALIB_OFF = 0,
        CALIB_BASELINE_WAIT,    // 等待 ISO 160 生效
        CALIB_BASELINE_COLLECT, // 采集 1s 内的最大环境亮度
        CALIB_SEARCH_LEADING,   // 寻找闪光区间起始边缘
        CALIB_SEARCH_TRAILING,  // 寻找闪光区间结束边缘
        CALIB_COOLDOWN,         // 硬件冷却 (防止连闪过载)
        CALIB_VERIFY            // 稳定验证
    };
    CalibState      m_calibState          = CALIB_OFF;
    int             m_calibSavedIso       = 0;
    int             m_calibWaitTicks      = 0;

    int             m_calibInitDelay      = 1200; // 初始基准延时
    int             m_calibTrialDelay     = 1200; // 当前测试的延时值
    int             m_calibSuccessCount   = 0;    // 连续成功次数
    int             m_calibFailedCount    = 0;    // 累计失败次数 (用于螺旋搜索)
    int             m_calibTotalTries     = 0;    // 总尝试次数
    
    int             m_calibScanStartDelay = 0;
    int             m_calibScanEndDelay   = 0;
    int             m_calibBestDelay      = 0;
    int             m_calibLastTestedDelay= 0;
    int             m_calibLeadingEdge    = -1;
    int             m_calibTrailingEdge   = -1;
    int             m_calibConsecutiveFails = 0;

    float           m_calibBaseLuma       = 0.0f; // 正常环境亮度基准
    std::atomic<float> m_calibLastRawLuma{0.0f};  // 最新 RAW 帧亮度
    std::atomic<float> m_calibMaxLuma{0.0f};      // 100ms 窗口内的最大亮度
    QTimer*         m_calibTimer          = nullptr;
    QLabel*         m_calibOverlay        = nullptr;

    // 手动校准 UI
    QWidget*        m_manualCalibWidget   = nullptr;
    QPushButton*    m_btnManualCalibMinus = nullptr;
    QPushButton*    m_btnManualCalibPlus  = nullptr;
    QLabel*         m_lblManualCalibDelay = nullptr;
    bool            m_isManualCalibActive = false;

    // 拍照格式
    PhotoFormat m_photoFormat      = Photo_DNG;  // 当前格式
    QRect       m_photoFormatRect;               // HUD 格式标签点击区域（屏幕坐标）
    std::atomic<bool> m_isCapturing{false};      // 防止重复触发拍照
    std::atomic<int>  m_photoCaptureSkipFrames{-1};// 负数表示未请求，0表示立即捕获，大于0表示跳过特定帧数

    // 闪光帧时间戳精准匹配架构
    std::atomic<bool>    m_captureArmed{false};   // 等待时间戳命中的闪光帧
    QByteArray           m_latestJpgBuffer;         // 始终缓存最新帧的 JPEG（闪光帧命中后作为内嵌 JPEG）
    QMutex               m_latestJpgMutex;

    // 拍摄安全看门狗：5 秒后若 m_isCapturing 仍为 true，强制复位（防止 FIFO 阻塞后系统卡死）
    QTimer*              m_captureWatchdog = nullptr;
    QString     m_photoTimestamp;                // 统一时间戳
    
    // ZSL 内嵌 JPEG 单 DNG 合并缓存机制
    QMutex      m_captureMutex;
    QByteArray  m_capturedRawData;
    QByteArray  m_capturedJpgData;
    bool        m_rawCaptured = false;
    bool        m_jpgCaptured = false;
    void        checkAndSaveSingleDng();         // 检查数据并拼装单个 DNG 文件
    
    // 缓存最新帧的 libcamera 白平衡增益（在 rawFrameCaptured 中更新）
    // 用于 rpicam-still --awbgains 参数，无需原子操作（仅作拍摄时参考）
    float       m_lastRGain        = 1.5f;
    float       m_lastBGain        = 1.2f;
    double      m_lastShutterUs    = 0.0;
    int         m_lastIso          = 100;
    void applyFocusPeaking(QImage& img, int color, int level);
    void drawPhotoFormatLabel(QPainter& p, int viewX, int viewY);

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;

public slots:
    void startManualFlashCalibration();
    void onManualCalibMinus();
    void onManualCalibPlus();
    void finishManualCalibration();
};
