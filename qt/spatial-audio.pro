TEMPLATE = app
TARGET = dm8168-audio-qt

QT += core gui
greaterThan(QT_MAJOR_VERSION, 4) {
    QT += widgets
    CONFIG += c++11
}

CONFIG += warn_on
CONFIG -= app_bundle

SOURCES += main.cpp \
           mainwindow.cpp
HEADERS += mainwindow.h

unix:!macx {
    DEFINES += _POSIX_C_SOURCE=200809L
}
