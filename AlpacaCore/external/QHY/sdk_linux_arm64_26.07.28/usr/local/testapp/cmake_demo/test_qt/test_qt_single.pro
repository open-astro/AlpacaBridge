QT += core gui widgets

TARGET = test_qt_single
TEMPLATE = app

CONFIG += c++14

SOURCES += \
    main.cpp

INCLUDEPATH += \
    $$PWD/include

win32 {
    LIBS += -L$$PWD/lib -lqhyccd
    QHYCCD_DLL = $$PWD\\lib\\qhyccd.dll
    QMAKE_POST_LINK += $$quote(if exist \"$${QHYCCD_DLL}\" copy /Y \"$${QHYCCD_DLL}\" \"$${OUT_PWD}\\\")$$escape_expand(\\n\\t)
}

# 运行前请确保 qhyccd.dll 在可执行文件目录或系统 PATH 中。
