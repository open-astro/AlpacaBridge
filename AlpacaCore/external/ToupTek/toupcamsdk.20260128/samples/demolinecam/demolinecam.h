#ifndef __demoqt_H__
#define __demoqt_H__

#include <QRadioButton>
#include "global.h"
#include "polylinepanel.h"
#include <QStackedWidget>

class MainWidget : public QWidget
{
    Q_OBJECT
    ToupcamDeviceV2 m_cur;
    HToupcam        m_hcam;
    QComboBox*      m_cmb_res;
    QSlider*        m_slider_expoTime;
    QLineEdit*      m_edt_expoTime;
    QPushButton*    m_btn_expoTime;
    QLabel*         m_lbl_temp;
    QLabel*         m_lbl_tint;
    QLabel*         m_lbl_video;
    QLabel*         m_lbl_frame;
    QPushButton*    m_btn_open;
    QPushButton*    m_btn_snap;
    QPushButton*    m_btn_trigger;
    QTimer*         m_timer;
    unsigned        m_imgWidth;
    unsigned        m_imgHeight;
    ushort*         m_pData;
    uchar*          m_pshowData;
    int             m_res;
    int             m_temp;
    int             m_tint;
    unsigned        m_count;
    QStackedWidget* m_showWidget;
    int             m_bshow;
    LineDataPanel*  m_LineDataPanel;
    QRadioButton*   m_rdo_trigger;
    QRadioButton*   m_rdo_video;
    QRadioButton*   m_rdo_poly;
    QRadioButton*   m_rdo_image;
    QRadioButton*   m_rdo_OpenTEC;
    QRadioButton*   m_rdo_CloseTEC;
    QLineEdit*      m_edt_TECtarget;
    QPushButton*    m_btn_setTEC;
    QLabel*         m_lbl_Temperature;
    unsigned        m_depth;

public:
    MainWidget(QWidget* parent = nullptr);
protected:
    void closeEvent(QCloseEvent*) override;
signals:
    void evtCallback(unsigned nEvent);
private:
    void onBtnOpen();
    void onBtnSnap();
    void onBtnTrigger();
    void onBtnSetTEC();
    void onRdoPoly();
    void onRdoImage();
    void onRdoTrigger();
    void onRdoVideo();
    void onRdoOpenTEC();
    void onRdoCloseTEC();
    void handleImageEvent();
    void handleExpoEvent();
    void handleStillImageEvent();
    void openCamera();
    void closeCamera();
    void startCamera();
    static void __stdcall eventCallBack(unsigned nEvent, void* pCallbackCtx);
    static QVBoxLayout* makeLayout(QLabel*, QSlider*, QLabel*, QLabel*, QSlider*, QLabel*);
};

#endif
