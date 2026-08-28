#pragma once

#include <QWidget>
#include <QLabel>
#include <QTimer>
#include <QPixmap>
#include <QString>

class WifiShareWidget : public QWidget {
    Q_OBJECT

public:
    explicit WifiShareWidget(QWidget* parent = nullptr);
    ~WifiShareWidget();

    void openWidget();
    void closeWidget();

protected:
    void paintEvent(QPaintEvent* event) override;

private slots:
    void updateNetworkStats();

private:
    void generateQRCodes();
    QString m_wifiQrPath;
    QString m_smbQrPath;
    
    QPixmap m_wifiPixmap;
    QPixmap m_smbPixmap;

    QTimer* m_statsTimer;

    int m_connectedDevices = 0;
    float m_transferSpeedMBps = 0.0f;
    long long m_lastRxBytes = 0;
    long long m_lastTxBytes = 0;
};
