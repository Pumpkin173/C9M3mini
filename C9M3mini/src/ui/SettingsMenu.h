#pragma once

#include <QWidget>
#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPainter>
#include <QProcess>
#include <QTimer>
#include <vector>
#include <QString>
#include "WifiShareWidget.h"

// Defines the data structure for a setting item
struct SettingItem {
    QString name;
    QString value;      // For display, e.g., "ON", "OFF"
    int type;           // 0 = toggle, 1 = action (could be expanded)
    bool toggleState;   // true = ON, false = OFF
};

// Defines the data structure for a main category tab
struct SettingsCategory {
    QString iconText; // Icon or abbreviation (e.g. "📷", "🌐", "⚙️")
    QString name;
    std::vector<SettingItem> items;
};

class SettingsMenu : public QWidget {
    Q_OBJECT

public:
    explicit SettingsMenu(QWidget* parent = nullptr);
    ~SettingsMenu();

    void openMenu();
    void closeMenu();

public slots:
    // Hardware joystick handlers
    void onJoystickUp();
    void onJoystickDown();
    void onJoystickLeft();
    void onJoystickRight();
    void onJoystickCenter();
    void onMenuPressed();

signals:
    void closed();
    void gridLineModeChanged(int mode);
    void flashCalibrationRequested();
    void manualFlashCalibrationRequested();
    void variableFpsToggled(bool enabled);
protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;

private:
    void initData();
    void toggleCurrentItem();
    void updateWiFiStatus();
    void updateBluetoothStatus();
    void resetConfirmStates();

    // Data
    std::vector<SettingsCategory> m_categories;

    // State
    int m_currentCategoryIndex = 0;
    int m_currentItemIndex = 0;
    bool m_inCategoryList = true; // true = focus on left tabs, false = focus on right items
    
    WifiShareWidget* m_wifiShareWidget = nullptr;
    
    // Commands Execution
    void runSystemCommand(const QString& command, const QStringList& args, bool waitForFinished = true);
};
