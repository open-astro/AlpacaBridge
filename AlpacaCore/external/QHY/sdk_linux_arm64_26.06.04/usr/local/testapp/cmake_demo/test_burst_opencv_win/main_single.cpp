#include <qhyccd.h>
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>

#include <cstdio>
#include <cstdlib>
#include <direct.h> // _mkdir
#include <vector>

static int cv_type_from_bpp_channels(unsigned int bpp, unsigned int channels) {
    const bool u16 = (bpp > 8);
    if (channels == 1) return u16 ? CV_16UC1 : CV_8UC1;
    if (channels == 3) return u16 ? CV_16UC3 : CV_8UC3;
    if (channels == 4) return u16 ? CV_16UC4 : CV_8UC4;
    return -1;
}

int main(int argc, char* argv[]) {
    // usage: test_single [exposure_us]
    const double exposure_us = (argc >= 2) ? std::atof(argv[1]) : 100000.0;

    InitQHYCCDResource();
    EnableQHYCCDMessage(true);
    if (ScanQHYCCD() <= 0) return 1; 

  
    char id[256] = {0};
    GetQHYCCDId(0, id);

    qhyccd_handle* camhandle = nullptr;

    std::vector<unsigned char> buffer;
    unsigned int w = 0, h = 0, bpp = 0, channels = 0;

    do {
        camhandle = OpenQHYCCD(id); 
        if (!camhandle) break;

        SetQHYCCDReadMode(camhandle, 0);
        SetQHYCCDStreamMode(camhandle, SINGLE_MODE);
        InitQHYCCD(camhandle);

        double chipw = 0, chiph = 0, pixelw = 0, pixelh = 0;
        GetQHYCCDChipInfo(camhandle, &chipw, &chiph, &w, &h, &pixelw, &pixelh, &bpp);
        if (w == 0 || h == 0) break;

        SetQHYCCDBitsMode(camhandle, 16);
        SetQHYCCDResolution(camhandle, 0, 0, w, h);

        SetQHYCCDParam(camhandle, CONTROL_EXPOSURE, exposure_us);

        const int length = GetQHYCCDMemLength(camhandle);
        if (length <= 0) break;
        buffer.assign(static_cast<size_t>(length), 0);

        ExpQHYCCDSingleFrame(camhandle);
        GetQHYCCDSingleFrame(camhandle, &w, &h, &bpp, &channels, buffer.data());
        if (w == 0 || h == 0) break;

        _mkdir("output");

        const int type = cv_type_from_bpp_channels(bpp, channels);
        if (type < 0) break;
        const cv::Mat img(static_cast<int>(h), static_cast<int>(w), type, buffer.data());
        cv::imwrite("output/full.png", img);
    } while (0);

    if (camhandle) CloseQHYCCD(camhandle);
    ReleaseQHYCCDResource();
    return 0;
}

