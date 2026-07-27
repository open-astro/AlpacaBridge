#include <qhyccd.h>

#include <QApplication>
#include <QCheckBox>
#include <QCoreApplication>
#include <QDate>
#include <QDateTime>
#include <QDebug>
#include <QDoubleSpinBox>
#include <QDir>
#include <QFile>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QMessageBox>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QPushButton>
#include <QSpinBox>
#include <QString>
#include <QTextStream>
#include <QTime>
#include <QVBoxLayout>
#include <QWidget>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <thread>
#include <vector>

namespace {
QBrush makeCheckerboardBrush() {
    QPixmap tile(24, 24);
    tile.fill(QColor(198, 198, 198));

    QPainter painter(&tile);
    const QColor dark_gray(162, 162, 162);
    painter.fillRect(0, 0, 12, 12, dark_gray);
    painter.fillRect(12, 12, 12, 12, dark_gray);

    return QBrush(tile);
}

QString makeSafeTag(const QString &raw) {
    QString safe;
    safe.reserve(raw.size());
    for (const QChar ch : raw) {
        if (ch.isLetterOrNumber()) {
            safe.append(ch);
        } else {
            safe.append('_');
        }
    }
    while (safe.contains("__")) {
        safe.replace("__", "_");
    }
    safe = safe.trimmed();
    if (safe.isEmpty()) {
        safe = "image";
    }
    return safe;
}

void appendTrace(const QString &message) {
    const QString line =
        QString("[%1] %2").arg(QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss.zzz"), message);
    qInfo().noquote() << line;

    const QString log_path = QDir(QCoreApplication::applicationDirPath()).filePath("test_qt_single_trace.log");
    QFile log_file(log_path);
    if (log_file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        QTextStream stream(&log_file);
        stream << line << "\n";
    }
}
}  // namespace

class CameraSingleWindow : public QWidget {
public:
    CameraSingleWindow() {
        setWindowTitle("QHYCCD Single Mode Demo (Qt)");
        resize(1000, 700);

        auto *root_layout = new QVBoxLayout(this);
        auto *button_layout = new QHBoxLayout();
        auto *proc_layout = new QHBoxLayout();

        connect_button_ = new QPushButton("连接相机");
        capture_button_ = new QPushButton("单帧拍摄");
        capture_proc_button_ = new QPushButton("单帧拍摄(proc)");
        save_button_ = new QPushButton("保存当前图像");
        capture_button_->setEnabled(false);
        capture_proc_button_->setEnabled(false);
        save_button_->setEnabled(false);
        exposure_label_ = new QLabel("曝光(us):");
        exposure_spin_ = new QDoubleSpinBox();
        exposure_spin_->setDecimals(0);
        exposure_spin_->setRange(1.0, 60000000.0);
        exposure_spin_->setSingleStep(1000.0);
        exposure_spin_->setValue(exposure_us_);
        exposure_spin_->setSuffix(" us");

        button_layout->addWidget(connect_button_);
        button_layout->addWidget(capture_button_);
        button_layout->addWidget(capture_proc_button_);
        button_layout->addWidget(save_button_);
        button_layout->addWidget(exposure_label_);
        button_layout->addWidget(exposure_spin_);

        proc_overscan_check_ = new QCheckBox("proc_overscan");
        proc_binx_label_ = new QLabel("proc_binx:");
        proc_binx_spin_ = new QSpinBox();
        proc_binx_spin_->setRange(1, 8);
        proc_binx_spin_->setValue(1);
        proc_biny_label_ = new QLabel("proc_biny:");
        proc_biny_spin_ = new QSpinBox();
        proc_biny_spin_->setRange(1, 8);
        proc_biny_spin_->setValue(1);
        proc_bin_avg_check_ = new QCheckBox("proc_bin_avg");

        proc_layout->addWidget(proc_overscan_check_);
        proc_layout->addWidget(proc_binx_label_);
        proc_layout->addWidget(proc_binx_spin_);
        proc_layout->addWidget(proc_biny_label_);
        proc_layout->addWidget(proc_biny_spin_);
        proc_layout->addWidget(proc_bin_avg_check_);

        status_label_ = new QLabel("状态：未连接");
        image_label_ = new QLabel();
        image_label_->setMinimumSize(960, 540);
        image_label_->setStyleSheet("color:#404040;");
        image_label_->setAutoFillBackground(true);
        image_label_->setBackgroundRole(QPalette::Window);
        QPalette image_palette = image_label_->palette();
        image_palette.setBrush(QPalette::Window, makeCheckerboardBrush());
        image_label_->setPalette(image_palette);
        image_label_->setAlignment(Qt::AlignCenter);
        image_label_->setText("等待图像...");

        root_layout->addLayout(button_layout);
        root_layout->addLayout(proc_layout);
        root_layout->addWidget(status_label_);
        root_layout->addWidget(image_label_, 1);

        connect(connect_button_, &QPushButton::clicked, this, [this]() {
            connectCamera();
        });
        connect(capture_button_, &QPushButton::clicked, this, [this]() {
            captureSingleFrame();
        });
        connect(capture_proc_button_, &QPushButton::clicked, this, [this]() {
            captureSingleFrameProc();
        });
        connect(save_button_, &QPushButton::clicked, this, [this]() {
            saveCurrentImage();
        });
    }

    ~CameraSingleWindow() override {
        if (cam_handle_ != nullptr) {
            CloseQHYCCD(cam_handle_);
            cam_handle_ = nullptr;
        }
        ReleaseQHYCCDResource();
    }

private:
    bool connectCamera() {
        appendTrace("connectCamera: enter");
        if (cam_handle_ != nullptr) {
            appendTrace("connectCamera: camera already connected");
            status_label_->setText("状态：相机已连接");
            return true;
        }

        appendTrace("connectCamera: InitQHYCCDResource start");
        if (InitQHYCCDResource() != QHYCCD_SUCCESS) {
            appendTrace("connectCamera: InitQHYCCDResource failed");
            QMessageBox::critical(this, "错误", "InitQHYCCDResource 失败");
            return false;
        }
        appendTrace("connectCamera: InitQHYCCDResource ok");

        EnableQHYCCDMessage(true);
        appendTrace("connectCamera: EnableQHYCCDMessage(true)");
        const int camera_count = ScanQHYCCD();
        appendTrace(QString("connectCamera: ScanQHYCCD count=%1").arg(camera_count));
        if (camera_count <= 0) {
            QMessageBox::warning(this, "提示", "未发现相机");
            status_label_->setText("状态：未发现相机");
            return false;
        }

        char id[32] = {0};
        if (GetQHYCCDId(0, id) != QHYCCD_SUCCESS) {
            appendTrace("connectCamera: GetQHYCCDId failed");
            QMessageBox::critical(this, "错误", "GetQHYCCDId 失败");
            return false;
        }
        appendTrace(QString("connectCamera: GetQHYCCDId ok id=%1").arg(QString::fromLatin1(id)));

        cam_handle_ = OpenQHYCCD(id);
        if (cam_handle_ == nullptr) {
            appendTrace("connectCamera: OpenQHYCCD failed");
            QMessageBox::critical(this, "错误", "OpenQHYCCD 失败");
            return false;
        }
        appendTrace(QString("connectCamera: OpenQHYCCD ok handle=%1")
                        .arg(reinterpret_cast<qulonglong>(cam_handle_), 0, 16));

        if (SetQHYCCDReadMode(cam_handle_, 0) != QHYCCD_SUCCESS) {
            appendTrace("connectCamera: SetQHYCCDReadMode failed");
            QMessageBox::critical(this, "错误", "SetQHYCCDReadMode 失败");
            return false;
        }
        appendTrace("connectCamera: SetQHYCCDReadMode ok");

        if (SetQHYCCDStreamMode(cam_handle_, SINGLE_MODE) != QHYCCD_SUCCESS) {
            appendTrace("connectCamera: SetQHYCCDStreamMode failed");
            QMessageBox::critical(this, "错误", "SetQHYCCDStreamMode(SINGLE_MODE) 失败");
            return false;
        }
        appendTrace("connectCamera: SetQHYCCDStreamMode ok");

        if (InitQHYCCD(cam_handle_) != QHYCCD_SUCCESS) {
            appendTrace("connectCamera: InitQHYCCD failed");
            QMessageBox::critical(this, "错误", "InitQHYCCD 失败");
            return false;
        }
        appendTrace("connectCamera: InitQHYCCD ok");

        if (SetQHYCCDBitsMode(cam_handle_, 16) != QHYCCD_SUCCESS) {
            appendTrace("connectCamera: SetQHYCCDBitsMode(16) failed");
            QMessageBox::critical(this, "错误", "SetQHYCCDBitsMode(16) 失败");
            return false;
        }
        appendTrace("connectCamera: SetQHYCCDBitsMode(16) ok");

        double chipw = 0;
        double chiph = 0;
        double pixelw = 0;
        double pixelh = 0;
        if (GetQHYCCDChipInfo(cam_handle_, &chipw, &chiph, &img_w_, &img_h_, &pixelw, &pixelh, &img_bpp_) !=
            QHYCCD_SUCCESS) {
            appendTrace("connectCamera: GetQHYCCDChipInfo failed");
            QMessageBox::critical(this, "错误", "GetQHYCCDChipInfo 失败");
            return false;
        }
        appendTrace(QString("connectCamera: ChipInfo ok w=%1 h=%2 bpp=%3 chipw=%4 chiph=%5 pixelw=%6 pixelh=%7")
                        .arg(img_w_)
                        .arg(img_h_)
                        .arg(img_bpp_)
                        .arg(chipw)
                        .arg(chiph)
                        .arg(pixelw)
                        .arg(pixelh));

        if (SetQHYCCDResolution(cam_handle_, 0, 0, img_w_, img_h_) != QHYCCD_SUCCESS) {
            appendTrace("connectCamera: SetQHYCCDResolution failed");
            QMessageBox::critical(this, "错误", "SetQHYCCDResolution 失败");
            return false;
        }
        appendTrace("connectCamera: SetQHYCCDResolution ok");

        exposure_us_ = exposure_spin_->value();
        if (SetQHYCCDParam(cam_handle_, CONTROL_EXPOSURE, exposure_us_) != QHYCCD_SUCCESS) {
            appendTrace(QString("connectCamera: SetQHYCCDParam exposure failed exp=%1").arg(exposure_us_));
            QMessageBox::warning(this, "警告", "设置曝光失败，将继续使用默认曝光");
        } else {
            appendTrace(QString("connectCamera: SetQHYCCDParam exposure ok exp=%1").arg(exposure_us_));
        }

        const uint32_t mem_len_raw = GetQHYCCDMemLength(cam_handle_);
        appendTrace(QString("connectCamera: GetQHYCCDMemLength raw=%1").arg(mem_len_raw));
        if (mem_len_raw == 0 || mem_len_raw == 0xFFFFFFFFu) {
            QMessageBox::critical(this, "错误",
                                  QString("GetQHYCCDMemLength 返回无效值: 0x%1")
                                      .arg(static_cast<qulonglong>(mem_len_raw), 8, 16, QChar('0')));
            return false;
        }

        const size_t mem_len = static_cast<size_t>(mem_len_raw);
        const size_t expected_len =
            static_cast<size_t>(img_w_) * static_cast<size_t>(img_h_) * static_cast<size_t>(img_bpp_) / 8;
        constexpr size_t kMaxSafeFrameBytes = 512ull * 1024ull * 1024ull;  // 512 MiB
        appendTrace(QString("connectCamera: mem_len=%1 expected_len=%2").arg(mem_len).arg(expected_len));

        if (expected_len > 0 && mem_len < expected_len) {
            QMessageBox::critical(
                this, "错误",
                QString("GetQHYCCDMemLength 太小: %1, 期望至少 %2").arg(mem_len).arg(expected_len));
            return false;
        }
        if (mem_len > kMaxSafeFrameBytes) {
            QMessageBox::critical(this, "错误",
                                  QString("GetQHYCCDMemLength 异常偏大: %1 bytes").arg(mem_len));
            return false;
        }

        try {
            appendTrace("connectCamera: frame_buffer_.assign start");
            frame_buffer_.assign(mem_len, 0);
            appendTrace("connectCamera: frame_buffer_.assign ok");
        } catch (const std::bad_alloc &) {
            appendTrace("connectCamera: frame_buffer_.assign bad_alloc");
            QMessageBox::critical(this, "错误",
                                  QString("分配图像缓冲区失败: %1 bytes").arg(mem_len));
            return false;
        }

        appendTrace("connectCamera: set status text start");
        status_label_->setText(
            QString("状态：已连接，分辨率 %1 x %2，曝光 %3 us").arg(img_w_).arg(img_h_).arg(exposure_us_));
        appendTrace("connectCamera: set status text ok");
        appendTrace("connectCamera: enable capture_button start");
        capture_button_->setEnabled(true);
        appendTrace("connectCamera: enable capture_button ok");
        appendTrace("connectCamera: enable capture_proc_button start");
        capture_proc_button_->setEnabled(true);
        appendTrace("connectCamera: enable capture_proc_button ok");
        appendTrace("connectCamera: return true");
        return true;
    }

    void captureSingleFrame() {
        if (!connectCamera()) {
            return;
        }

        exposure_us_ = exposure_spin_->value();
        if (SetQHYCCDParam(cam_handle_, CONTROL_EXPOSURE, exposure_us_) != QHYCCD_SUCCESS) {
            status_label_->setText("状态：设置曝光失败");
            return;
        }

        ExpQHYCCDSingleFrame(cam_handle_);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        unsigned int w = img_w_;
        unsigned int h = img_h_;
        unsigned int bpp = img_bpp_;
        unsigned int channels = 1;

        const int ret = GetQHYCCDSingleFrame(cam_handle_, &w, &h, &bpp, &channels, frame_buffer_.data());
        if (ret != QHYCCD_SUCCESS) {
            status_label_->setText("状态：单帧采集失败");
            return;
        }

        renderFrameAndShowStatus(w, h, bpp, channels, "GetQHYCCDSingleFrame",
                                 QString("single_exp%1").arg(static_cast<int>(exposure_us_)));
    }

    void captureSingleFrameProc() {
        if (!connectCamera()) {
            return;
        }

        exposure_us_ = exposure_spin_->value();
        if (SetQHYCCDParam(cam_handle_, CONTROL_EXPOSURE, exposure_us_) != QHYCCD_SUCCESS) {
            status_label_->setText("状态：设置曝光失败");
            return;
        }

        ExpQHYCCDSingleFrame(cam_handle_);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        unsigned int w = img_w_;
        unsigned int h = img_h_;
        unsigned int bpp = img_bpp_;
        unsigned int channels = 1;

        const uint8_t proc_overscan = proc_overscan_check_->isChecked() ? 1 : 0;
        const uint32_t proc_binx = static_cast<uint32_t>(proc_binx_spin_->value());
        const uint32_t proc_biny = static_cast<uint32_t>(proc_biny_spin_->value());
        const uint8_t proc_bin_avg = proc_bin_avg_check_->isChecked() ? 1 : 0;

        const int ret = GetQHYCCDSingleFrame_proc(cam_handle_, &w, &h, &bpp, &channels, frame_buffer_.data(),
                                                  proc_overscan, proc_binx, proc_biny, proc_bin_avg);
        if (ret != QHYCCD_SUCCESS) {
            status_label_->setText("状态：proc 单帧采集失败");
            return;
        }

        const QString proc_desc =
            QString("GetQHYCCDSingleFrame_proc(overscan=%1, binx=%2, biny=%3, bin_avg=%4)")
                .arg(proc_overscan)
                .arg(proc_binx)
                .arg(proc_biny)
                .arg(proc_bin_avg);
        const QString proc_tag =
            QString("proc_o%1_x%2_y%3_a%4_exp%5")
                .arg(proc_overscan)
                .arg(proc_binx)
                .arg(proc_biny)
                .arg(proc_bin_avg)
                .arg(static_cast<int>(exposure_us_));
        renderFrameAndShowStatus(w, h, bpp, channels, proc_desc, proc_tag);
    }

    void saveCurrentImage() {
        if (last_image_.isNull()) {
            status_label_->setText("状态：没有可保存的图像");
            return;
        }

        QDir app_dir(QCoreApplication::applicationDirPath());
        const QString date_dir_name = QDate::currentDate().toString("yyyyMMdd");
        if (!app_dir.mkpath(date_dir_name)) {
            status_label_->setText("状态：创建日期目录失败");
            return;
        }

        QDir target_dir(app_dir.filePath(date_dir_name));
        const QString time_str = QTime::currentTime().toString("hhmmss");
        const QString tag1 = makeSafeTag(last_image_tag1_);
        const QString tag2 = makeSafeTag(last_image_tag2_);
        const QString file_name = QString("%1_%2_%3.png").arg(time_str).arg(tag1).arg(tag2);
        const QString file_path = target_dir.filePath(file_name);

        if (!last_image_.save(file_path, "PNG")) {
            status_label_->setText("状态：保存 PNG 失败");
            return;
        }
        status_label_->setText(QString("状态：图像已保存到 %1").arg(file_path));
    }

    void renderFrameAndShowStatus(unsigned int w, unsigned int h, unsigned int bpp, unsigned int channels,
                                  const QString &source_name, const QString &file_tag2) {
        if (channels != 1) {
            QMessageBox::warning(this, "提示", "示例当前仅显示单通道图像");
            return;
        }

        const unsigned char *display_ptr = nullptr;
        if (bpp == 8) {
            display_ptr = frame_buffer_.data();
        } else if (bpp == 16) {
            const size_t pixel_count = static_cast<size_t>(w) * static_cast<size_t>(h);
            if (frame_buffer_.size() < pixel_count * sizeof(uint16_t)) {
                status_label_->setText("状态：16bit 图像缓冲区长度异常");
                return;
            }

            display_buffer_8bit_.assign(pixel_count, 0);
            const auto *src16 = reinterpret_cast<const uint16_t *>(frame_buffer_.data());

            uint16_t min_v = std::numeric_limits<uint16_t>::max();
            uint16_t max_v = std::numeric_limits<uint16_t>::min();
            for (size_t i = 0; i < pixel_count; ++i) {
                const uint16_t v = src16[i];
                if (v < min_v) min_v = v;
                if (v > max_v) max_v = v;
            }

            if (max_v == min_v) {
                std::fill(display_buffer_8bit_.begin(), display_buffer_8bit_.end(), 0);
            } else {
                const double scale = 255.0 / static_cast<double>(max_v - min_v);
                for (size_t i = 0; i < pixel_count; ++i) {
                    const double stretched = (static_cast<double>(src16[i]) - static_cast<double>(min_v)) * scale;
                    display_buffer_8bit_[i] = static_cast<unsigned char>(stretched);
                }
            }
            display_ptr = display_buffer_8bit_.data();
        } else {
            QMessageBox::warning(this, "提示", "示例当前仅显示 8bit/16bit 单通道图像");
            return;
        }

        const QImage image(display_ptr, static_cast<int>(w), static_cast<int>(h), static_cast<int>(w),
                           QImage::Format_Grayscale8);
        if (image.isNull()) {
            QMessageBox::critical(this, "错误", "QImage 构建失败");
            return;
        }

        const QPixmap pix = QPixmap::fromImage(image.copy());
        image_label_->setPixmap(
            pix.scaled(image_label_->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
        last_image_ = image.copy();
        last_image_tag1_ = source_name.contains("_proc") ? "proc" : "single";
        last_image_tag2_ = file_tag2;
        save_button_->setEnabled(true);
        status_label_->setText(
            QString("状态：采集成功 [%1]，输出尺寸 %2 x %3, bpp=%4, channels=%5")
                .arg(source_name)
                .arg(w)
                .arg(h)
                .arg(bpp)
                .arg(channels));
    }

private:
    QPushButton *connect_button_ = nullptr;
    QPushButton *capture_button_ = nullptr;
    QPushButton *capture_proc_button_ = nullptr;
    QPushButton *save_button_ = nullptr;
    QLabel *exposure_label_ = nullptr;
    QDoubleSpinBox *exposure_spin_ = nullptr;
    QCheckBox *proc_overscan_check_ = nullptr;
    QLabel *proc_binx_label_ = nullptr;
    QSpinBox *proc_binx_spin_ = nullptr;
    QLabel *proc_biny_label_ = nullptr;
    QSpinBox *proc_biny_spin_ = nullptr;
    QCheckBox *proc_bin_avg_check_ = nullptr;
    QLabel *status_label_ = nullptr;
    QLabel *image_label_ = nullptr;

    qhyccd_handle *cam_handle_ = nullptr;
    unsigned int img_w_ = 0;
    unsigned int img_h_ = 0;
    unsigned int img_bpp_ = 8;
    double exposure_us_ = 100000.0;
    std::vector<unsigned char> frame_buffer_;
    std::vector<unsigned char> display_buffer_8bit_;
    QImage last_image_;
    QString last_image_tag1_ = "single";
    QString last_image_tag2_ = "image";
};

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    CameraSingleWindow window;
    window.show();
    return app.exec();
}
