#include "SettingsMenu.h"
#include <QDebug>
#include <QPainterPath>
#include <QFontMetrics>
#include <QApplication>
#include <QMouseEvent>
#include "../core/VideoRecorder.h"

SettingsMenu::SettingsMenu(QWidget* parent)
    : QWidget(parent)
{
    // Make sure we capture keyboard/focus if needed, though hardware buttons handle most
    setFocusPolicy(Qt::StrongFocus);
    
    m_wifiShareWidget = new WifiShareWidget(this);
    
    initData();
}

SettingsMenu::~SettingsMenu() {
}

void SettingsMenu::initData() {
    // Initialize Categories and Items
    SettingsCategory networkCat;
    networkCat.iconText = "NW"; // Network
    networkCat.name = "Network";
    networkCat.items.push_back({"WiFi", "OFF", 0, false});
    networkCat.items.push_back({"Hotspot", "OFF", 0, false});
    networkCat.items.push_back({"Bluetooth", "OFF", 0, false});
    
    SettingsCategory cameraCat;
    cameraCat.iconText = "CAM";
    cameraCat.name = "Camera";
    cameraCat.items.push_back({"Beep", "ON", 0, true});
    cameraCat.items.push_back({"Grid Line", "Rule of 3rds", 1, false});
    cameraCat.items.push_back({"闪光校准", "Execute", 1, false});
    cameraCat.items.push_back({"闪光校准（手动）", "Execute", 1, false});
    cameraCat.items.push_back({"连续可变帧率", "OFF", 0, false});
    
    SettingsCategory setupCat;
    setupCat.iconText = "SET";
    setupCat.name = "Setup";
    setupCat.items.push_back({"Format", "Execute", 1, false});
    setupCat.items.push_back({"Reset", "Execute", 1, false});
    setupCat.items.push_back({"Version", "v1.0.0", 1, false});
    
    m_categories.push_back(networkCat);
    m_categories.push_back(cameraCat);
    m_categories.push_back(setupCat);
    
    // Attempt to sync actual state
    updateWiFiStatus();
    updateBluetoothStatus();
}

void SettingsMenu::updateWiFiStatus() {
    // Check rfkill status
    QProcess rfkillProcess;
    rfkillProcess.start("rfkill", QStringList() << "list" << "wlan");
    rfkillProcess.waitForFinished();
    QString rfkillOutput = rfkillProcess.readAllStandardOutput();
    bool isOff = rfkillOutput.contains("Soft blocked: yes") || rfkillOutput.contains("Hard blocked: yes");

    // Check if Hotspot is active via nmcli
    QProcess nmcliProcess;
    nmcliProcess.start("sh", QStringList() << "-c" << "nmcli -t -f NAME connection show --active | grep 'Hotspot'");
    nmcliProcess.waitForFinished();
    QString nmcliOutput = nmcliProcess.readAllStandardOutput().trimmed();
    bool isHotspotActive = !nmcliOutput.isEmpty();

    if (m_categories.size() > 0 && m_categories[0].items.size() > 1) {
        auto& wifiItem = m_categories[0].items[0];
        auto& hotspotItem = m_categories[0].items[1];

        if (isOff) {
            wifiItem.toggleState = false;
            wifiItem.value = "OFF";
            hotspotItem.toggleState = false;
            hotspotItem.value = "OFF";
        } else {
            if (isHotspotActive) {
                wifiItem.toggleState = false;
                wifiItem.value = "OFF";
                hotspotItem.toggleState = true;
                hotspotItem.value = "ON";
            } else {
                wifiItem.toggleState = true;
                wifiItem.value = "ON";
                hotspotItem.toggleState = false;
                hotspotItem.value = "OFF";
            }
        }
    }
}

void SettingsMenu::updateBluetoothStatus() {
    QProcess process;
    process.start("rfkill", QStringList() << "list" << "bluetooth");
    process.waitForFinished();
    QString output = process.readAllStandardOutput();
    
    bool isOff = output.contains("Soft blocked: yes") || output.contains("Hard blocked: yes");
    if (m_categories.size() > 0 && m_categories[0].items.size() > 2) {
        m_categories[0].items[2].toggleState = !isOff;
        m_categories[0].items[2].value = !isOff ? "ON" : "OFF";
    }
}

void SettingsMenu::runSystemCommand(const QString& command, const QStringList& args, bool waitForFinished) {
    if (waitForFinished) {
        QProcess::execute(command, args);
    } else {
        QProcess::startDetached(command, args);
    }
}

void SettingsMenu::openMenu() {
    // Sync status every time menu opens
    updateWiFiStatus();
    updateBluetoothStatus();
    
    m_inCategoryList = true;
    m_currentCategoryIndex = 0;
    m_currentItemIndex = 0;
    show();
    update();
}

void SettingsMenu::closeMenu() {
    m_wifiShareWidget->closeWidget();
    resetConfirmStates();
    hide();
    emit closed();
}

void SettingsMenu::onJoystickUp() {
    if (!isVisible()) return;
    if (m_wifiShareWidget->isVisible()) { m_wifiShareWidget->closeWidget(); return; }
    resetConfirmStates();
    
    if (m_inCategoryList) {
        if (m_currentCategoryIndex > 0) {
            m_currentCategoryIndex--;
        } else {
            m_currentCategoryIndex = m_categories.size() - 1; // Wrap around
        }
        m_currentItemIndex = 0; // Reset item index when changing category
    } else {
        if (m_currentItemIndex > 0) {
            m_currentItemIndex--;
        } else {
            int itemsCount = m_categories[m_currentCategoryIndex].items.size();
            if (itemsCount > 0) {
                m_currentItemIndex = itemsCount - 1; // Wrap around
            }
        }
    }
    update();
}

void SettingsMenu::onJoystickDown() {
    if (!isVisible()) return;
    if (m_wifiShareWidget->isVisible()) { m_wifiShareWidget->closeWidget(); return; }
    resetConfirmStates();
    
    if (m_inCategoryList) {
        if (m_currentCategoryIndex < (int)m_categories.size() - 1) {
            m_currentCategoryIndex++;
        } else {
            m_currentCategoryIndex = 0; // Wrap around
        }
        m_currentItemIndex = 0;
    } else {
        int itemsCount = m_categories[m_currentCategoryIndex].items.size();
        if (m_currentItemIndex < itemsCount - 1) {
            m_currentItemIndex++;
        } else {
            m_currentItemIndex = 0; // Wrap around
        }
    }
    update();
}

void SettingsMenu::onJoystickLeft() {
    if (!isVisible()) return;
    if (m_wifiShareWidget->isVisible()) { m_wifiShareWidget->closeWidget(); return; }
    resetConfirmStates();
    
    if (!m_inCategoryList) {
        m_inCategoryList = true; // Go back to category list
        update();
    }
}

void SettingsMenu::onJoystickRight() {
    if (!isVisible()) return;
    if (m_wifiShareWidget->isVisible()) { m_wifiShareWidget->closeWidget(); return; }
    resetConfirmStates();
    
    if (m_inCategoryList) {
        if (m_categories[m_currentCategoryIndex].items.size() > 0) {
            m_inCategoryList = false; // Enter options list
            update();
        }
    }
}

void SettingsMenu::onJoystickCenter() {
    if (!isVisible()) return;
    
    if (m_wifiShareWidget->isVisible()) {
        m_wifiShareWidget->closeWidget();
        return;
    }
    
    if (m_inCategoryList) {
        // Entering category options is the same as pressing right
        onJoystickRight();
    } else {
        // Toggle or execute action
        toggleCurrentItem();
        update();
    }
}

void SettingsMenu::onMenuPressed() {
    if (isVisible()) {
        closeMenu();
    }
}

void SettingsMenu::toggleCurrentItem() {
    if (m_currentCategoryIndex < 0 || m_currentCategoryIndex >= m_categories.size()) return;
    auto& cat = m_categories[m_currentCategoryIndex];
    if (m_currentItemIndex < 0 || m_currentItemIndex >= cat.items.size()) return;
    
    auto& item = cat.items[m_currentItemIndex];
    
    if (cat.name == "Network" && item.name == "WiFi") {
        item.toggleState = !item.toggleState;
        item.value = item.toggleState ? "ON" : "OFF";
        
        if (item.toggleState) {
            // Turning WiFi ON -> Turn Hotspot OFF
            auto& hotspotItem = cat.items[1]; // Hotspot is index 1
            if (hotspotItem.toggleState) {
                hotspotItem.toggleState = false;
                hotspotItem.value = "OFF";
                runSystemCommand("sh", QStringList() << "-c" << "sudo nmcli connection down Hotspot", false);
                m_wifiShareWidget->closeWidget();
            }
            // Enable WiFi radio
            runSystemCommand("rfkill", QStringList() << "unblock" << "wlan");
        } else {
            // Turning WiFi OFF
            runSystemCommand("rfkill", QStringList() << "block" << "wlan");
        }
    } 
    else if (cat.name == "Network" && item.name == "Hotspot") {
        item.toggleState = !item.toggleState;
        item.value = item.toggleState ? "ON" : "OFF";
        
        if (item.toggleState) {
            // Turning Hotspot ON -> Turn WiFi OFF (in UI, because radio must be ON for AP)
            auto& wifiItem = cat.items[0]; // WiFi is index 0
            if (wifiItem.toggleState) {
                wifiItem.toggleState = false;
                wifiItem.value = "OFF";
            }
            
            QString cmd = "sudo rfkill unblock wlan && sudo nmcli device wifi hotspot ifname wlan0 ssid C9M3_Camera password PumpkinC9M3";
            runSystemCommand("sh", QStringList() << "-c" << cmd, true); // Wait for hotspot to be fully up so we can get the correct IP
            m_wifiShareWidget->openWidget();
        } else {
            // Turning Hotspot OFF
            QString cmd = "sudo nmcli connection down Hotspot";
            runSystemCommand("sh", QStringList() << "-c" << cmd, false);
            m_wifiShareWidget->closeWidget();
        }
    }
    else if (cat.name == "Network" && item.name == "Bluetooth") {
        item.toggleState = !item.toggleState;
        item.value = item.toggleState ? "ON" : "OFF";
        
        QString cmd;
        if (item.toggleState) {
            // Unblock radio and start the service dynamically
            cmd = "sudo rfkill unblock bluetooth && sudo systemctl start bluetooth";
        } else {
            // Stop the service and block radio
            cmd = "sudo systemctl stop bluetooth && sudo rfkill block bluetooth";
        }
        runSystemCommand("sh", QStringList() << "-c" << cmd, false);
    }
    else if (cat.name == "Camera" && item.name == "Grid Line") {
        if (item.value == "OFF") {
            item.value = "Rule of 3rds";
            emit gridLineModeChanged(1);
        } else if (item.value == "Rule of 3rds") {
            item.value = "Square";
            emit gridLineModeChanged(2);
        } else if (item.value == "Square") {
            item.value = "Center Cross";
            emit gridLineModeChanged(3);
        } else {
            item.value = "OFF";
            emit gridLineModeChanged(0);
        }
    }
    else if (cat.name == "Camera" && item.name == "闪光校准") {
        emit flashCalibrationRequested();
        closeMenu();
    }
    else if (cat.name == "Camera" && item.name == "闪光校准（手动）") {
        emit manualFlashCalibrationRequested();
        closeMenu();
    }
    else if (cat.name == "Camera" && item.name == "连续可变帧率") {
        item.toggleState = !item.toggleState;
        item.value = item.toggleState ? "ON" : "OFF";
        emit variableFpsToggled(item.toggleState);
        if (item.toggleState) {
            closeMenu();
        }
    }
    else if (cat.name == "Setup" && item.name == "Format") {
        if (item.value == "Execute" || item.value == "Done") {
            item.value = "Confirm?";
        } else if (item.value == "Confirm?") {
            item.value = "Formatting...";
            update();
            QApplication::processEvents();
            QString activePath = VideoRecorder::getActiveStoragePath();
            QString cmd = QString("rm -rf %1/Video/* && rm -rf %1/Photo/*").arg(activePath);
            runSystemCommand("sh", QStringList() << "-c" << cmd, true);
            item.value = "Done";
        }
    }
    else if (item.type == 0) { // Generic toggle
        item.toggleState = !item.toggleState;
        item.value = item.toggleState ? "ON" : "OFF";
    }
    else {
        // Action (e.g., Format, Reset). Could show a dialog here.
        qDebug() << "Action triggered for:" << item.name;
    }
}

void SettingsMenu::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    // Dark Theme Colors
    QColor bgColor(15, 15, 15);           // Deep black/grey
    QColor sidebarColor(30, 30, 30);      // Slightly lighter grey for sidebar
    QColor accentColor(255, 140, 0);      // Sony Alpha Orange/Yellow accent
    QColor textColor(240, 240, 240);
    QColor unselectedTextColor(150, 150, 150);
    QColor itemBgColor(45, 45, 45);       // Background for right side items
    QColor selectedItemBgColor(60, 60, 60);

    // Draw main background
    p.fillRect(rect(), bgColor);

    int sidebarWidth = 90;
    int headerHeight = 50;

    // Draw sidebar background
    p.fillRect(0, 0, sidebarWidth, height(), sidebarColor);

    QFont headerFont("Arial", 20, QFont::Bold);
    QFont tabFont("Arial", 16, QFont::Bold);
    QFont itemFont("Arial", 20);
    QFont valueFont("Arial", 18);

    // Draw Header
    p.setFont(headerFont);
    p.setPen(textColor);
    p.drawText(sidebarWidth + 20, 0, width() - sidebarWidth, headerHeight, Qt::AlignVCenter | Qt::AlignLeft, "MENU");
    
    // Draw separator line under header
    p.setPen(QPen(QColor(60, 60, 60), 2));
    p.drawLine(sidebarWidth, headerHeight, width(), headerHeight);

    // Draw Left Sidebar
    int tabHeight = 80;
    int startY = headerHeight + 10;

    for (int i = 0; i < (int)m_categories.size(); ++i) {
        QRect tabRect(0, startY + i * tabHeight, sidebarWidth, tabHeight);
        
        bool isSelected = (i == m_currentCategoryIndex);
        
        if (isSelected) {
            // Draw accent line on the left
            p.fillRect(0, tabRect.y(), 4, tabRect.height(), accentColor);
            
            // Draw slightly lighter background for selected tab
            p.fillRect(4, tabRect.y(), tabRect.width() - 4, tabRect.height(), QColor(50, 50, 50));
            
            // If focus is in the category list, draw a border
            if (m_inCategoryList) {
                p.setPen(QPen(accentColor, 2));
                p.drawRect(4, tabRect.y() + 1, tabRect.width() - 6, tabRect.height() - 2);
            }
        }

        p.setFont(tabFont);
        p.setPen(isSelected ? textColor : unselectedTextColor);
        p.drawText(tabRect, Qt::AlignCenter, m_categories[i].iconText);
    }

    // Draw Right Options List
    if (m_currentCategoryIndex >= 0 && m_currentCategoryIndex < m_categories.size()) {
        const auto& cat = m_categories[m_currentCategoryIndex];
        
        int itemStartY = headerHeight + 20;
        int itemHeight = 60;
        int padding = 20;
        int contentX = sidebarWidth + padding;
        int contentWidth = width() - sidebarWidth - padding * 2;

        for (int i = 0; i < (int)cat.items.size(); ++i) {
            QRect itemRect(contentX, itemStartY + i * (itemHeight + 10), contentWidth, itemHeight);
            
            bool isSelectedItem = (!m_inCategoryList && i == m_currentItemIndex);
            
            // Draw item background
            p.fillRect(itemRect, isSelectedItem ? selectedItemBgColor : itemBgColor);
            
            // If selected, draw outline
            if (isSelectedItem) {
                p.setPen(QPen(accentColor, 2));
                p.drawRect(itemRect);
            }

            // Draw Item Name
            p.setFont(itemFont);
            p.setPen(textColor);
            p.drawText(itemRect.adjusted(15, 0, 0, 0), Qt::AlignVCenter | Qt::AlignLeft, cat.items[i].name);

            // Draw Item Value / Status
            p.setFont(valueFont);
            if (isSelectedItem) {
                p.setPen(accentColor); // Highlight value in orange if selected
            } else {
                p.setPen(unselectedTextColor);
            }
            p.drawText(itemRect.adjusted(0, 0, -15, 0), Qt::AlignVCenter | Qt::AlignRight, cat.items[i].value);
        }
    }
}

void SettingsMenu::mousePressEvent(QMouseEvent* event) {
    if (!isVisible()) return;
    if (m_wifiShareWidget->isVisible()) {
        m_wifiShareWidget->closeWidget();
        return;
    }
    
    QPoint pos = event->pos();
    resetConfirmStates();

    int sidebarWidth = 90;
    int headerHeight = 50;

    // Check if clicked in left sidebar tabs
    if (pos.x() < sidebarWidth) {
        int tabHeight = 80;
        int startY = headerHeight + 10;
        
        for (int i = 0; i < (int)m_categories.size(); ++i) {
            QRect tabRect(0, startY + i * tabHeight, sidebarWidth, tabHeight);
            if (tabRect.contains(pos)) {
                m_currentCategoryIndex = i;
                m_currentItemIndex = 0;
                m_inCategoryList = true;
                update();
                return;
            }
        }
    } else {
        // Check if clicked in right options list
        if (m_currentCategoryIndex >= 0 && m_currentCategoryIndex < (int)m_categories.size()) {
            const auto& cat = m_categories[m_currentCategoryIndex];
            int itemStartY = headerHeight + 20;
            int itemHeight = 60;
            int padding = 20;
            int contentX = sidebarWidth + padding;
            int contentWidth = width() - sidebarWidth - padding * 2;

            for (int i = 0; i < (int)cat.items.size(); ++i) {
                QRect itemRect(contentX, itemStartY + i * (itemHeight + 10), contentWidth, itemHeight);
                if (itemRect.contains(pos)) {
                    m_currentItemIndex = i;
                    m_inCategoryList = false;
                    toggleCurrentItem();
                    update();
                    return;
                }
            }
        }
    }
}

void SettingsMenu::resetConfirmStates() {
    for (auto& cat : m_categories) {
        if (cat.name == "Setup") {
            for (auto& item : cat.items) {
                if (item.name == "Format" && item.value != "Execute") {
                    item.value = "Execute";
                }
            }
        }
    }
}
