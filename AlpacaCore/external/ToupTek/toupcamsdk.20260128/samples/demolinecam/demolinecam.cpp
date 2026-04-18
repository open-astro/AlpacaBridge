#include <QApplication>
#include "demolinecam.h"

MainWidget::MainWidget(QWidget* parent)
    : QWidget(parent)
    , m_hcam(nullptr), m_timer(new QTimer(this))
    , m_imgWidth(0), m_imgHeight(0), m_pData(nullptr), m_pshowData(nullptr)
    , m_res(0), m_temp(TOUPCAM_TEMP_DEF), m_tint(TOUPCAM_TINT_DEF), m_count(0), m_bshow(0)
    , m_LineDataPanel(new LineDataPanel(this))
{
    setMinimumSize(1440, 512);
    showMaximized();
    QGridLayout* gmain = new QGridLayout();

    QGroupBox* gboxres = new QGroupBox("Resolution");
    {
        m_cmb_res = new QComboBox();
        m_cmb_res->setEnabled(false);
        connect(m_cmb_res, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index)
        {
            if (m_hcam) //step 1: stop camera
                Toupcam_Stop(m_hcam);

            m_res = index;
            m_imgWidth = m_cur.model->res[index].width;
            m_imgHeight = m_cur.model->res[index].height;

            if (m_hcam) //step 2: restart camera
            {
                Toupcam_put_eSize(m_hcam, static_cast<unsigned>(m_res));
                startCamera();
            }
        });

        QVBoxLayout* v = new QVBoxLayout();
        v->addWidget(m_cmb_res);
        gboxres->setLayout(v);
    }

    QGroupBox* gboxexp = new QGroupBox("Exposure");
    {
        m_slider_expoTime = new QSlider(Qt::Horizontal);
        m_slider_expoTime->setEnabled(false);
        m_btn_expoTime = new QPushButton("Set Expotime");
        m_btn_expoTime->setEnabled(false);
        m_edt_expoTime = new QLineEdit();
        m_edt_expoTime->setEnabled(false);
        connect(m_slider_expoTime, &QSlider::valueChanged, this, [this](int value)
        {
            if (m_hcam)
            {
                m_edt_expoTime->setText(QString::number(value));
                Toupcam_put_ExpoTime(m_hcam, value * 1000);
            }
        });
        connect(m_btn_expoTime, &QPushButton::clicked, this, [this]()
        {
            if (m_hcam)
            {
                int nexpo = m_edt_expoTime->text().toInt();
                Toupcam_put_ExpoTime(m_hcam, nexpo * 1000);
                {
                    const QSignalBlocker blocker(m_slider_expoTime);
                    m_slider_expoTime->setValue(nexpo);
                }
            }
        });

        QHBoxLayout* h = new QHBoxLayout();
        h->addWidget(m_edt_expoTime);
        h->addWidget(new QLabel("ms"));
        h->addWidget(m_btn_expoTime);
        QVBoxLayout* v = new QVBoxLayout();
        v->addLayout(h);
        v->addWidget(m_slider_expoTime);
        gboxexp->setLayout(v);
    }

    QGroupBox* gboxshowmode = new QGroupBox("Show mode");
    {
        m_rdo_poly = new QRadioButton("Polyline");
        connect(m_rdo_poly, &QRadioButton::clicked, this, &MainWidget::onRdoPoly);
        m_rdo_image = new QRadioButton("image");
        connect(m_rdo_image, &QRadioButton::clicked, this, &MainWidget::onRdoImage);
        QHBoxLayout* h = new QHBoxLayout();
        h->addWidget(m_rdo_poly);
        h->addWidget(m_rdo_image);
        gboxshowmode->setLayout(h);
        m_rdo_poly->setChecked(true);
    }

    QGroupBox* gboxTriggermode = new QGroupBox("Trigger Mode");
    {
        m_rdo_trigger = new QRadioButton("Trigger");
        connect(m_rdo_trigger, &QRadioButton::clicked, this, &MainWidget::onRdoTrigger);
        m_rdo_trigger->setEnabled(false);
        m_rdo_video = new QRadioButton("Video");
        connect(m_rdo_video, &QRadioButton::clicked, this, &MainWidget::onRdoVideo);
        m_rdo_video->setEnabled(false);
        m_btn_trigger = new QPushButton("Trigger");
        connect(m_btn_trigger, &QPushButton::clicked, this, &MainWidget::onBtnTrigger);
        m_btn_trigger->setEnabled(false);
        QHBoxLayout* h = new QHBoxLayout();
        h->addWidget(m_rdo_video);
        h->addWidget(m_rdo_trigger);
        h->addWidget(m_btn_trigger);
        gboxTriggermode->setLayout(h);
        m_rdo_video->setChecked(true);
    }

    QGroupBox* gboxTEC = new QGroupBox("TEC");
    {
        m_rdo_OpenTEC = new QRadioButton("Open");
        connect(m_rdo_OpenTEC, &QRadioButton::clicked, this, &MainWidget::onRdoOpenTEC);
        m_rdo_OpenTEC->setEnabled(false);
        m_rdo_CloseTEC = new QRadioButton("Close");
        connect(m_rdo_CloseTEC, &QRadioButton::clicked, this, &MainWidget::onRdoCloseTEC);
        m_rdo_CloseTEC->setEnabled(false);
        m_edt_TECtarget = new QLineEdit();
        m_edt_TECtarget->setEnabled(false);
        m_btn_setTEC = new QPushButton("Set");
        connect(m_btn_setTEC, &QPushButton::clicked, this, &MainWidget::onBtnSetTEC);
        m_btn_setTEC->setEnabled(false);
        m_lbl_Temperature = new QLabel("0");


        QHBoxLayout* h1 = new QHBoxLayout();
        h1->addWidget(new QLabel("TEC target:"));
        h1->addWidget(m_edt_TECtarget);
        h1->addWidget(m_btn_setTEC);

        QHBoxLayout* h2 = new QHBoxLayout();
        h2->addWidget(new QLabel("Temperature:"));
        h2->addWidget(m_lbl_Temperature);

        QVBoxLayout* v = new QVBoxLayout();
        v->addWidget(m_rdo_OpenTEC);
        v->addLayout(h1);
        v->addWidget(m_rdo_CloseTEC);
        v->addLayout(h2);
        gboxTEC->setLayout(v);
    }

    {
        m_btn_open = new QPushButton("Open");
        connect(m_btn_open, &QPushButton::clicked, this, &MainWidget::onBtnOpen);
        m_btn_snap = new QPushButton("Snap");
        m_btn_snap->setEnabled(false);
        connect(m_btn_snap, &QPushButton::clicked, this, &MainWidget::onBtnSnap);

        m_lbl_frame = new QLabel();

        QVBoxLayout* v = new QVBoxLayout();
        v->addWidget(m_btn_open);
        v->addWidget(m_btn_snap);
        v->addWidget(gboxres);
        v->addWidget(gboxTriggermode);
        v->addWidget(gboxexp);
        v->addWidget(gboxTEC);
        v->addWidget(gboxshowmode);
        v->addWidget(m_lbl_frame);
        v->addStretch();
        gmain->addLayout(v, 0, 0);
    }

    {

        m_lbl_video = new QLabel();
        m_showWidget = new QStackedWidget();
        m_showWidget->addWidget(m_LineDataPanel);
        m_showWidget->addWidget(m_lbl_video);
        m_showWidget->setCurrentIndex(0);

        QVBoxLayout* v = new QVBoxLayout();
        v->addWidget(m_showWidget, 1);
        gmain->addLayout(v, 0, 1);
    }

    gmain->setColumnStretch(0, 1);
    gmain->setColumnStretch(1, 5);
    setLayout(gmain);

    connect(this, &MainWidget::evtCallback, this, [this](unsigned nEvent)
    {
        /* this run in the UI thread */
        if (m_hcam)
        {
            if (TOUPCAM_EVENT_IMAGE == nEvent)
                handleImageEvent();
            else if (TOUPCAM_EVENT_EXPOSURE == nEvent)
                handleExpoEvent();
            else if (TOUPCAM_EVENT_STILLIMAGE == nEvent)
                handleStillImageEvent();
            else if (TOUPCAM_EVENT_ERROR == nEvent)
            {
                closeCamera();
                QMessageBox::warning(this, "Warning", "Generic error.");
            }
            else if (TOUPCAM_EVENT_DISCONNECTED == nEvent)
            {
                closeCamera();
                QMessageBox::warning(this, "Warning", "Camera disconnect.");
            }
        }
    });

    connect(m_timer, &QTimer::timeout, this, [this]()
    {
        unsigned nFrame = 0, nTime = 0, nTotalFrame = 0;
        if (m_hcam && SUCCEEDED(Toupcam_get_FrameRate(m_hcam, &nFrame, &nTime, &nTotalFrame)) && (nTime > 0))
            m_lbl_frame->setText(QString::asprintf("%u, fps = %.1f", nTotalFrame, nFrame * 1000.0 / nTime));
        short nTemperature = 0;
        if (m_hcam && SUCCEEDED(Toupcam_get_Temperature(m_hcam, &nTemperature)))
            m_lbl_Temperature->setText(QString::asprintf("%.1f", nTemperature / 10.0f));
    });
}

void MainWidget::closeCamera()
{
    if (m_hcam)
    {
        Toupcam_Close(m_hcam);
        m_hcam = nullptr;
    }
    delete[] m_pData;
    m_pData = nullptr;
    delete[] m_pshowData;
    m_pshowData = nullptr;

    m_btn_open->setText("Open");
    m_timer->stop();
    m_lbl_frame->clear();
    m_slider_expoTime->setEnabled(false);
    m_btn_expoTime->setEnabled(false);
    m_edt_expoTime->setEnabled(false);
    m_btn_snap->setEnabled(false);
    m_cmb_res->setEnabled(false);
    m_cmb_res->clear();
    m_rdo_video->setEnabled(false);
    m_rdo_trigger->setEnabled(false);
    m_btn_trigger->setEnabled(false);
    m_rdo_OpenTEC->setEnabled(false);
    m_rdo_CloseTEC->setEnabled(false);
}

void MainWidget::closeEvent(QCloseEvent*)
{
    closeCamera();
}

void MainWidget::startCamera()
{
    if (m_pData)
    {
        delete[] m_pData;
        m_pData = nullptr;
    }
    m_pData = new ushort[m_imgWidth * m_imgHeight];
    if (m_pshowData)
    {
        delete[] m_pshowData;
        m_pshowData = nullptr;
    }
    m_pshowData = new uchar[m_imgWidth * m_imgHeight];
    unsigned uimax = 0, uimin = 0, uidef = 0;
    Toupcam_get_ExpTimeRange(m_hcam, &uimin, &uimax, &uidef);
    m_slider_expoTime->setRange(uimin / 1000, uimax / 1000);
    handleExpoEvent();

    Toupcam_put_Option(m_hcam, TOUPCAM_OPTION_BITDEPTH, 1);
    Toupcam_put_Option(m_hcam, TOUPCAM_OPTION_RAW, 1);
    Toupcam_put_AutoExpoEnable(m_hcam, 0);
    unsigned pFourCC;
    Toupcam_get_RawFormat(m_hcam, &pFourCC, &m_depth);
    if (SUCCEEDED(Toupcam_StartPullModeWithCallback(m_hcam, eventCallBack, this)))
    {
        m_rdo_video->setChecked(true);
        m_cmb_res->setEnabled(true);
        m_btn_open->setText("Close");
        m_btn_snap->setEnabled(true);
        m_btn_expoTime->setEnabled(true);
        m_edt_expoTime->setEnabled(true);
        m_slider_expoTime->setEnabled(true);
        m_rdo_video->setEnabled(true);
        m_rdo_trigger->setEnabled(true);
        m_timer->start(1000);

        int bTEC;
        if (SUCCEEDED(Toupcam_get_Option(m_hcam, TOUPCAM_OPTION_TEC, &bTEC)))
        {
            m_btn_setTEC->setEnabled(true);
            m_rdo_OpenTEC->setEnabled(true);
            m_rdo_CloseTEC->setEnabled(true);
            m_edt_TECtarget->setEnabled(true);
            if (bTEC == 1)
                m_rdo_OpenTEC->setChecked(true);
            else
                m_rdo_CloseTEC->setChecked(true);
            Toupcam_put_Option(m_hcam, TOUPCAM_OPTION_TECTARGET, -300);
            int nTECtarget;
            if (SUCCEEDED(Toupcam_get_Option(m_hcam, TOUPCAM_OPTION_TECTARGET, &nTECtarget)))
                m_edt_TECtarget->setText(QString::number(nTECtarget / 10.0));
        }
    }
    else
    {
        closeCamera();
        QMessageBox::warning(this, "Warning", "Failed to start camera.");
    }
}

void MainWidget::openCamera()
{
    m_hcam = Toupcam_Open(m_cur.id);
    if (m_hcam)
    {
        Toupcam_get_eSize(m_hcam, (unsigned*)&m_res);
        m_imgWidth = m_cur.model->res[m_res].width;
        m_imgHeight = m_cur.model->res[m_res].height;
        {
            const QSignalBlocker blocker(m_cmb_res);
            m_cmb_res->clear();
            for (unsigned i = 0; i < m_cur.model->preview; ++i)
                m_cmb_res->addItem(QString::asprintf("%u*%u", m_cur.model->res[i].width, m_cur.model->res[i].height));
            m_cmb_res->setCurrentIndex(m_res);
            m_cmb_res->setEnabled(true);
        }

        Toupcam_put_Option(m_hcam, TOUPCAM_OPTION_BYTEORDER, 0); //Qimage use RGB byte order
        Toupcam_put_AutoExpoEnable(m_hcam, 1);
        startCamera();
    }
}

void MainWidget::onBtnOpen()
{
    if (m_hcam)
        closeCamera();
    else
    {
        ToupcamDeviceV2 arr[TOUPCAM_MAX] = { 0 };
        unsigned count = Toupcam_EnumV2(arr);
        if (0 == count)
            QMessageBox::warning(this, "Warning", "No camera found.");
        else if (1 == count)
        {
            m_cur = arr[0];
            openCamera();
        }
        else
        {
            QMenu menu;
            for (unsigned i = 0; i < count; ++i)
            {
                menu.addAction(
#if defined(_WIN32)
                            QString::fromWCharArray(arr[i].displayname)
#else
                            arr[i].displayname
#endif
                            , this, [this, i, arr](bool)
                {
                    m_cur = arr[i];
                    openCamera();
                });
            }
            menu.exec(mapToGlobal(m_btn_snap->pos()));
        }
    }
}

void MainWidget::onBtnSnap()
{
    if (m_hcam)
    {
        if (0 == m_cur.model->still)    // not support still image capture
            Toupcam_Snap(m_hcam, 0xffff);
        else
        {
            QMenu menu;
            for (unsigned i = 0; i < m_cur.model->still; ++i)
            {
                menu.addAction(QString::asprintf("%u*%u", m_cur.model->res[i].width, m_cur.model->res[i].height), this, [this, i](bool)
                {
                    Toupcam_Snap(m_hcam, i);
                });
            }
            menu.exec(mapToGlobal(m_btn_snap->pos()));
        }
    }
}

void MainWidget::onBtnTrigger()
{
    if (m_hcam)
        Toupcam_Trigger(m_hcam, 1);
}

void MainWidget::onBtnSetTEC()
{
    if (m_hcam)
    {
        int val = int(m_edt_TECtarget->text().toFloat() * 10);
        Toupcam_put_Option(m_hcam, TOUPCAM_OPTION_TECTARGET, val);
    }
}

void MainWidget::onRdoTrigger()
{
    m_btn_trigger->setEnabled(true);
    Toupcam_put_Option(m_hcam, TOUPCAM_OPTION_TRIGGER, 1);
}

void MainWidget::onRdoVideo()
{
    m_btn_trigger->setEnabled(false);
    Toupcam_put_Option(m_hcam, TOUPCAM_OPTION_TRIGGER, 0);
}

void MainWidget::onRdoOpenTEC()
{
    m_btn_setTEC->setEnabled(true);
    m_edt_TECtarget->setEnabled(true);
    Toupcam_put_Option(m_hcam, TOUPCAM_OPTION_TEC, 1);
}

void MainWidget::onRdoCloseTEC()
{
    m_btn_setTEC->setEnabled(false);
    m_edt_TECtarget->setEnabled(false);
    Toupcam_put_Option(m_hcam, TOUPCAM_OPTION_TEC, 0);
}

void MainWidget::onRdoPoly()
{
    m_bshow = 0;
    m_showWidget->setCurrentIndex(0);
}

void MainWidget::onRdoImage()
{
    m_bshow = 1;
    m_showWidget->setCurrentIndex(1);
}

void MainWidget::eventCallBack(unsigned nEvent, void* pCallbackCtx)
{
    MainWidget* pThis = reinterpret_cast<MainWidget*>(pCallbackCtx);
    emit pThis->evtCallback(nEvent);
}

void MainWidget::handleImageEvent()
{
    unsigned width = 0, height = 0;
    if (SUCCEEDED(Toupcam_PullImage(m_hcam, m_pData, 0, &width, &height)))
    {
        if (m_bshow == 1)
        {
            ushort* pdata = reinterpret_cast<ushort*>(m_pData);
            const int bitShift = m_depth - 8;
#pragma omp parallel for simd
            for (unsigned y = 0; y < height; ++y)
            {
                unsigned index = y * width;
                for (unsigned x = 0; x < width; ++x)
                    m_pshowData[index + x] = pdata[index + x] >> bitShift;
            }

            QImage image(m_pshowData, width, height, QImage::Format_Grayscale8);
            QImage newimage = image.scaled(m_lbl_video->width(), m_lbl_video->height(), Qt::KeepAspectRatio, Qt::FastTransformation);
            m_lbl_video->setPixmap(QPixmap::fromImage(newimage));
        }
        else
        {
            m_LineDataPanel->OnBuffer(reinterpret_cast<ushort*>(m_pData), width, m_depth);
        }
    }
}

void MainWidget::handleExpoEvent()
{
    unsigned time = 0;
    Toupcam_get_ExpoTime(m_hcam, &time);
    time /= 1000;
    {
        const QSignalBlocker blocker(m_slider_expoTime);
        m_slider_expoTime->setValue(int(time));
    }
    {
        const QSignalBlocker blocker(m_edt_expoTime);
        m_edt_expoTime->setText(QString::number(time));
    }
}

void MainWidget::handleStillImageEvent()
{
    unsigned width = 0, height = 0;
    if (SUCCEEDED(Toupcam_PullStillImage(m_hcam, nullptr, 24, &width, &height))) // peek
    {
        std::vector<uchar> vec(TDIBWIDTHBYTES(width * 24) * height);
        if (SUCCEEDED(Toupcam_PullStillImage(m_hcam, &vec[0], 24, &width, &height)))
        {
            QImage image(&vec[0], width, height, QImage::Format_RGB888);
            image.save(QString::asprintf("demoqt_%u.jpg", ++m_count));
        }
    }
}

QVBoxLayout* MainWidget::makeLayout(QLabel* lbl1, QSlider* sli1, QLabel* val1, QLabel* lbl2, QSlider* sli2, QLabel* val2)
{
    QHBoxLayout* hlyt1 = new QHBoxLayout();
    hlyt1->addWidget(lbl1);
    hlyt1->addStretch();
    hlyt1->addWidget(val1);
    QHBoxLayout* hlyt2 = new QHBoxLayout();
    hlyt2->addWidget(lbl2);
    hlyt2->addStretch();
    hlyt2->addWidget(val2);
    QVBoxLayout* vlyt = new QVBoxLayout();
    vlyt->addLayout(hlyt1);
    vlyt->addWidget(sli1);
    vlyt->addLayout(hlyt2);
    vlyt->addWidget(sli2);
    return vlyt;
}

int main(int argc, char* argv[])
{
    Toupcam_GigeEnable(nullptr, nullptr);
    QApplication a(argc, argv);
    MainWidget mw;
    mw.show();
    return a.exec();
}
