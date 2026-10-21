#include <qhyccd.h>
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <direct.h> // _mkdir
#include <chrono>
#include <string>
#include <thread>
#include <vector>

static int cv_type_from_bpp_channels(unsigned int bpp, unsigned int channels) {
    const bool u16 = (bpp > 8);
    if (channels == 1) return u16 ? CV_16UC1 : CV_8UC1;
    if (channels == 3) return u16 ? CV_16UC3 : CV_8UC3;
    if (channels == 4) return u16 ? CV_16UC4 : CV_8UC4;
    return -1;
}

static void sleep_us(int us) {
    std::this_thread::sleep_for(std::chrono::microseconds(us));
}

static void print_ret(const char* name, int ret) {
    std::printf("%s ret = %d\n", name, ret);
}

static std::string timestamp_now_ymdhms() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm lt = {};
    localtime_s(&lt, &t);
    char buf[32] = {0};
    std::strftime(buf, sizeof(buf), "%Y-%m-%d_%H-%M-%S", &lt);
    return std::string(buf);
}

static void save_binned_images(const cv::Mat& src, const char* ts, int frame_idx) {
    // 软件 bin: 使用 INTER_AREA 做等效面积平均缩放，适合整倍数降采样
    for (int bin = 2; bin <= 6; ++bin) {
        const int out_w = src.cols / bin;
        const int out_h = src.rows / bin;
        if (out_w <= 0 || out_h <= 0) continue;

        cv::Mat dst;
        cv::resize(src, dst, cv::Size(out_w, out_h), 0.0, 0.0, cv::INTER_AREA);

        char out_path[260];
        _snprintf_s(out_path, sizeof(out_path), _TRUNCATE, "output/%s_bin%d_%06d.png", ts, bin, frame_idx);
        cv::imwrite(out_path, dst);
    }
}

int main(int argc, char* argv[]) {
    // usage: test_burst [burst_count] [exposure_us] [burst_start] [burst_end] [patch_number]
    const int burst_count = (argc >= 2) ? std::atoi(argv[1]) : 2;
    const double exposure_us = (argc >= 3) ? std::atof(argv[2]) : 200000.0;
    const int burst_start = (argc >= 4) ? std::atoi(argv[3]) : 1;
    const int burst_end = (argc >= 5) ? std::atoi(argv[4]) : 3;
    const int patch_number = (argc >= 6) ? std::atoi(argv[5]) : 2000;

    InitQHYCCDResource();
    EnableQHYCCDMessage(true);
    if (ScanQHYCCD() <= 0) return 1;

    char id[256] = {0};
    GetQHYCCDId(0, id);

    _mkdir("output");

    qhyccd_handle* camhandle = nullptr;
    std::vector<unsigned char> buffer;
    unsigned int w = 0, h = 0, bpp = 0, channels = 0;

    do {
        camhandle = OpenQHYCCD(id);
        if (!camhandle) break;

        int ret = QHYCCD_SUCCESS;
        ret = SetQHYCCDReadMode(camhandle, 0);
        print_ret("SetQHYCCDReadMode", ret);

        // 参考代码：SetQHYCCDStreamMode(camhandle, 1) + BeginQHYCCDLive + GetQHYCCDLiveFrame
        ret = SetQHYCCDStreamMode(camhandle, 1);
        print_ret("SetQHYCCDStreamMode", ret);

        ret = InitQHYCCD(camhandle);
        print_ret("InitQHYCCD", ret);

        // Effective area (用于后续裁剪)
        unsigned int eff_ex = 0, eff_ey = 0, eff_sizex = 0, eff_sizey = 0;
        ret = GetQHYCCDEffectiveArea(camhandle, &eff_ex, &eff_ey, &eff_sizex, &eff_sizey);
        print_ret("GetQHYCCDEffectiveArea", ret);
        std::printf("EffectiveArea: ex=%u ey=%u sizex=%u sizey=%u\n", eff_ex, eff_ey, eff_sizex, eff_sizey);

        double chipw = 0, chiph = 0, pixelw = 0, pixelh = 0;
        ret = GetQHYCCDChipInfo(camhandle, &chipw, &chiph, &w, &h, &pixelw, &pixelh, &bpp);
        print_ret("GetQHYCCDChipInfo", ret);
        std::printf("ChipInfo: chipw=%.3f chiph=%.3f imagew=%u imageh=%u pixelw=%.6f pixelh=%.6f bpp=%u\n",
                    chipw, chiph, w, h, pixelw, pixelh, bpp);
        if (w == 0 || h == 0) break;

        // Bin + Resolution
        ret = SetQHYCCDBinMode(camhandle, 1, 1);
        print_ret("SetQHYCCDBinMode", ret);

        ret = SetQHYCCDResolution(camhandle, 0, 0, w, h);
        print_ret("SetQHYCCDResolution", ret);

        // Debayer off
        ret = SetQHYCCDDebayerOnOff(camhandle, false);
        print_ret("SetQHYCCDDebayerOnOff", ret);

        // Params (按参考顺序)
        ret = SetQHYCCDParam(camhandle, CONTROL_TRANSFERBIT, 16.0);
        print_ret("SetQHYCCDParam(CONTROL_TRANSFERBIT)", ret);
        ret = SetQHYCCDParam(camhandle, CONTROL_BRIGHTNESS, 0.0);
        print_ret("SetQHYCCDParam(CONTROL_BRIGHTNESS)", ret);
        ret = SetQHYCCDParam(camhandle, CONTROL_CONTRAST, 0.0);
        print_ret("SetQHYCCDParam(CONTROL_CONTRAST)", ret);
        ret = SetQHYCCDParam(camhandle, CONTROL_GAMMA, 1.0);
        print_ret("SetQHYCCDParam(CONTROL_GAMMA)", ret);
        ret = SetQHYCCDParam(camhandle, CONTROL_USBTRAFFIC, 0.0);
        print_ret("SetQHYCCDParam(CONTROL_USBTRAFFIC)", ret);
        ret = SetQHYCCDParam(camhandle, CONTROL_DDR, 1.0);
        print_ret("SetQHYCCDParam(CONTROL_DDR)", ret);
        ret = SetQHYCCDParam(camhandle, CONTROL_EXPOSURE, exposure_us);
        print_ret("SetQHYCCDParam(CONTROL_EXPOSURE)", ret);

        const int length = GetQHYCCDMemLength(camhandle);
        if (length <= 0) break;
        buffer.assign(static_cast<size_t>(length), 0);

        // Live start
        ret = BeginQHYCCDLive(camhandle);
        print_ret("BeginQHYCCDLive", ret);

        // Burst 配置（按参考顺序）
        ret = SetQHYCCDBurstModeStartEnd(camhandle, burst_start, burst_end);
        print_ret("SetQHYCCDBurstModeStartEnd", ret);
        ret = SetQHYCCDBurstModePatchNumber(camhandle, patch_number);
        print_ret("SetQHYCCDBurstModePatchNumber", ret);
        ret = EnableQHYCCDBurstMode(camhandle, true);
        print_ret("EnableQHYCCDBurstMode", ret);
        sleep_us(10000);

        ret = SetQHYCCDBurstIDLE(camhandle);
        print_ret("SetQHYCCDBurstIDLE", ret);
        sleep_us(20000);

        ret = ReleaseQHYCCDBurstIDLE(camhandle);
        print_ret("ReleaseQHYCCDBurstIDLE", ret);

        for (int i = 0; i < burst_count; ++i) {
            // 参考代码：循环 GetQHYCCDLiveFrame 直到成功取到一帧
            ret = QHYCCD_ERROR;
            constexpr int kMaxTries = 5000; // ~5s (每次失败 sleep 1ms)
            int tries = 0;
            while (ret != QHYCCD_SUCCESS && tries++ < kMaxTries) {
                ret = GetQHYCCDLiveFrame(camhandle, &w, &h, &bpp, &channels, buffer.data());
                if (ret != QHYCCD_SUCCESS) sleep_us(1000);
            }
            if (ret != QHYCCD_SUCCESS) {
                std::printf("GetQHYCCDLiveFrame timeout/fail ret=%d at i=%d\n", ret, i);
                break;
            }
            if (w == 0 || h == 0) break;

            const int type = cv_type_from_bpp_channels(bpp, channels);
            if (type < 0) break;

            const cv::Mat img(static_cast<int>(h), static_cast<int>(w), type, buffer.data());
            const std::string ts = timestamp_now_ymdhms();

            char out_path[260];
            _snprintf_s(out_path, sizeof(out_path), _TRUNCATE, "output/%s_full_%06d.png", ts.c_str(), i);
            cv::imwrite(out_path, img);

            // 根据 EffectiveArea 裁剪输出到 output（文件名用 cut 区分）
            if (eff_sizex > 0 && eff_sizey > 0 && w > 0 && h > 0) {
                const unsigned int roi_x = (eff_ex < w) ? eff_ex : (w - 1);
                const unsigned int roi_y = (eff_ey < h) ? eff_ey : (h - 1);
                const unsigned int roi_w = (eff_sizex <= (w - roi_x)) ? eff_sizex : (w - roi_x);
                const unsigned int roi_h = (eff_sizey <= (h - roi_y)) ? eff_sizey : (h - roi_y);
                if (roi_w > 0 && roi_h > 0) {
                    const cv::Rect roi(static_cast<int>(roi_x), static_cast<int>(roi_y),
                                       static_cast<int>(roi_w), static_cast<int>(roi_h));
                    const cv::Mat cut_img = img(roi).clone(); // clone 确保数据连续/独立
                    char cut_path[260];
                    _snprintf_s(cut_path, sizeof(cut_path), _TRUNCATE, "output/%s_cut_%06d.png", ts.c_str(), i);
                    cv::imwrite(cut_path, cut_img);

                    // 在 cut 之后输出 bin2~bin6（同样输出到 output，用 bin* 区分）
                    save_binned_images(cut_img, ts.c_str(), i);
                }
            }
        }

        // Capture 完成后让 burst 进入 IDLE
        ret = SetQHYCCDBurstIDLE(camhandle);
        print_ret("SetQHYCCDBurstIDLE(end)", ret);
    } while (0);

    if (camhandle) {
        // 参考头文件说明：BeginQHYCCDLive 后建议 StopQHYCCDLive 再 Close
        const int ret = StopQHYCCDLive(camhandle);
        print_ret("StopQHYCCDLive", ret);
    }
    if (camhandle) CloseQHYCCD(camhandle);
    ReleaseQHYCCDResource();
    return 0;
}


