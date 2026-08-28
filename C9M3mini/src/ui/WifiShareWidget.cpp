#include "WifiShareWidget.h"
#include <QPainter>
#include <QProcess>
#include <QFile>
#include <QTextStream>
#include <QDebug>
#include <QFontMetrics>
#include <QRegularExpression>

WifiShareWidget::WifiShareWidget(QWidget* parent)
    : QWidget(parent)
{
    setFixedSize(640, 480);
    m_wifiQrPath = "/tmp/wifi_qr.png";
    m_smbQrPath = "/tmp/smb_qr.png";

    m_statsTimer = new QTimer(this);
    connect(m_statsTimer, &QTimer::timeout, this, &WifiShareWidget::updateNetworkStats);
    
    // Default hiding
    hide();
}

WifiShareWidget::~WifiShareWidget() {
    if (m_statsTimer->isActive()) {
        m_statsTimer->stop();
    }
}

void WifiShareWidget::generateQRCodes() {
    // Generate WiFi QR
    QString wifiCmd = QString("qrencode -o %1 -s 6 -m 2 \"WIFI:T:WPA;S:C9M3_Camera;P:PumpkinC9M3;;\"").arg(m_wifiQrPath);
    QProcess::execute("sh", QStringList() << "-c" << wifiCmd);

    // Get dynamic IP address of wlan0
    QProcess ipProcess;
    ipProcess.start("sh", QStringList() << "-c" << "ip -4 addr show wlan0 | grep -oP '(?<=inet\\s)\\d+(\\.\\d+){3}'");
    ipProcess.waitForFinished();
    QString ip = ipProcess.readAllStandardOutput().trimmed();
    if (ip.isEmpty()) {
        ip = "10.42.0.1"; // Fallback to NetworkManager hotspot default
    }

    // Generate SMB QR
    QString smbCmd = QString("qrencode -o %1 -s 6 -m 2 \"smb://%2\"").arg(m_smbQrPath, ip);
    QProcess::execute("sh", QStringList() << "-c" << smbCmd);
    m_wifiPixmap.load(m_wifiQrPath);
    m_smbPixmap.load(m_smbQrPath);
}

void WifiShareWidget::openWidget() {
    // 按需启动 nmbd 服务
    QProcess::startDetached("sudo", QStringList() << "systemctl" << "start" << "nmbd");
    generateQRCodes();
    m_lastRxBytes = 0;
    m_lastTxBytes = 0;
    m_connectedDevices = 0;
    m_transferSpeedMBps = 0.0f;
    
    // Initial fetch to prime the bytes counters
    updateNetworkStats(); 
    m_statsTimer->start(1000);
    show();
    raise();
    update();
}

void WifiShareWidget::closeWidget() {
    QProcess::startDetached("sudo", QStringList() << "systemctl" << "stop" << "nmbd");
    m_statsTimer->stop();
    hide();
}

void WifiShareWidget::updateNetworkStats() {
    QProcess iwProcess;
    iwProcess.start("sh", QStringList() << "-c" << "iw dev wlan0 station dump | grep -c 'Station'");
    iwProcess.waitForFinished();
    QString iwOut = iwProcess.readAllStandardOutput().trimmed();
    bool ok;
    int count = iwOut.toInt(&ok);
    if (ok) {
        m_connectedDevices = count;
    }

    QFile file("/proc/net/dev");
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream in(&file);
        long long currentRx = 0;
        long long currentTx = 0;
        while (!in.atEnd()) {
            QString line = in.readLine();
            if (line.contains("wlan0:")) {
                QStringList parts = line.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
                if (parts.size() >= 10) { // wlan0: RxBytes ... TxBytes ...
                    currentRx = parts[1].toLongLong();
                    currentTx = parts[9].toLongLong();
                }
                break;
            }
        }
        file.close();

        if (m_lastRxBytes > 0 && m_lastTxBytes > 0) {
            long long rxDiff = currentRx - m_lastRxBytes;
            long long txDiff = currentTx - m_lastTxBytes;
            long long totalBytes = rxDiff + txDiff;
            // Convert to MB/s
            m_transferSpeedMBps = (float)totalBytes / (1024.0f * 1024.0f);
        }

        m_lastRxBytes = currentRx;
        m_lastTxBytes = currentTx;
    }

    // Request redraw
    update();
}

void WifiShareWidget::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    // Background overlay (semi-transparent dark)
    p.fillRect(rect(), QColor(10, 10, 10, 240));

    // Title
    QFont titleFont("Arial", 28, QFont::Bold);
    p.setFont(titleFont);
    p.setPen(Qt::white);
    p.drawText(0, 30, width(), 50, Qt::AlignCenter, "WiFi Share & Monitor");

    int qrY = 120;
    int qrSize = 200; // Expected size based on qrencode -s 6, might vary slightly
    
    // Layout centers
    int leftCenterX = width() / 4;
    int rightCenterX = (width() / 4) * 3;

    // Left QR (WiFi)
    if (!m_wifiPixmap.isNull()) {
        int x = leftCenterX - m_wifiPixmap.width() / 2;
        p.drawPixmap(x, qrY, m_wifiPixmap);
    }
    
    QFont labelFont("Arial", 18, QFont::Bold);
    QFont valFont("Arial", 22, QFont::Bold);
    
    p.setFont(labelFont);
    p.setPen(QColor(200, 200, 200));
    p.drawText(leftCenterX - 150, qrY - 35, 300, 30, Qt::AlignCenter, "1. Connect WiFi");

    p.setFont(valFont);
    p.setPen(QColor(255, 204, 0)); // Sony Yellow
    p.drawText(leftCenterX - 150, qrY + m_wifiPixmap.height() + 20, 300, 30, Qt::AlignCenter, 
               QString("Devices: %1").arg(m_connectedDevices));


    // Right QR (SMB)
    if (!m_smbPixmap.isNull()) {
        int x = rightCenterX - m_smbPixmap.width() / 2;
        p.drawPixmap(x, qrY, m_smbPixmap);
    }
    
    p.setFont(labelFont);
    p.setPen(QColor(200, 200, 200));
    p.drawText(rightCenterX - 150, qrY - 35, 300, 30, Qt::AlignCenter, "2. Browse Files");

    p.setFont(valFont);
    p.setPen(QColor(255, 204, 0));
    p.drawText(rightCenterX - 150, qrY + m_smbPixmap.height() + 20, 300, 30, Qt::AlignCenter, 
               QString("Speed: %1 MB/s").arg(m_transferSpeedMBps, 0, 'f', 1));
    
    // Draw close hint
    QFont hintFont("Arial", 14);
    p.setFont(hintFont);
    p.setPen(QColor(150, 150, 150));
    p.drawText(0, height() - 40, width(), 30, Qt::AlignCenter, "Press any button or turn off WiFi to close");
}
