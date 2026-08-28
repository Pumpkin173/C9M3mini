#include <QApplication>
#include <cstdlib>
#include <qglobal.h>
#include <QFile>
#include "ui/MainWindow.h"

int main(int argc, char* argv[]) {
    if (qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY") && qEnvironmentVariableIsEmpty("DISPLAY")) {
        const char* xdgDir = qgetenv("XDG_RUNTIME_DIR").constData();
        QByteArray wl1 = QByteArray(xdgDir) + "/wayland-1";
        QByteArray wl0 = QByteArray(xdgDir) + "/wayland-0";

        if (QFile::exists(QString::fromLocal8Bit(wl1))) {
            qputenv("WAYLAND_DISPLAY", "wayland-1");
            qputenv("QT_QPA_PLATFORM", "wayland");
        } else if (QFile::exists(QString::fromLocal8Bit(wl0))) {
            qputenv("WAYLAND_DISPLAY", "wayland-0");
            qputenv("QT_QPA_PLATFORM", "wayland");
        } else {
            qputenv("QT_QPA_PLATFORM", "eglfs");
            qputenv("QT_QPA_EGLFS_ROTATION", "90");
            qputenv("QT_QPA_EGLFS_ALWAYS_SET_MODE", "1");
            // 锁定字体 DPI，防止 EGLFS 下字体因无法读取屏幕 EDID 而被异常放大
            qputenv("QT_FONT_DPI", "85");
            qputenv("QT_QPA_EGLFS_NO_LIBINPUT", "1");
            qputenv("QT_QPA_EVDEV_TOUCHSCREEN_PARAMETERS", "rotate=90");
        }
    }

    QApplication app(argc, argv);
    MainWindow w;
    w.showFullScreen();

    return app.exec();
}