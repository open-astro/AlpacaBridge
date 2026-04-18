QT += core gui widgets charts
SOURCES += demolinecam.cpp \
    global.cpp \
    polylinepanel.cpp
HEADERS += demolinecam.h \
    global.h \
    polylinepanel.h
LIBS += -L$$PWD/./ -ltoupcam

RESOURCES += demolinecam.qrc
