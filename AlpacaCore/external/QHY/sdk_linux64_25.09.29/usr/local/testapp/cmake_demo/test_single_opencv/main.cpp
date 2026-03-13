#include <cstring>
#include <qhyccd.h>
#include <opencv2/opencv.hpp>
#include <time.h>
#include <thread>
#include <chrono>
#include <iostream>


int main(int argc,char *argv[]){

    int num = 0;
    qhyccd_handle *camhandle;
    int ret;
    char id[32];
    unsigned int w,h,bpp,channels;
    unsigned char *ImgData;


    ret = InitQHYCCDResource();

    EnableQHYCCDMessage(true);

    num = ScanQHYCCD();
    printf("Found %d cameras  \n",num);
    if(num == 0){
        printf("No camera found\n");
        return 1;
    }

    ret = GetQHYCCDId(0,id);
    printf("connected to the first camera from the list,id is %s\n",id);

    camhandle = OpenQHYCCD(id);

    SetQHYCCDReadMode(camhandle,0);
    SetQHYCCDStreamMode(camhandle,SINGLE_MODE);
    InitQHYCCD(camhandle);

    ret = SetQHYCCDBitsMode(camhandle,8);
    double chipw,chiph,pixelw,pixelh;
    ret = GetQHYCCDChipInfo(camhandle,&chipw,&chiph,&w,&h,&pixelw,&pixelh,&bpp);
    printf("CCD/CMOS chip information:\n");

    printf("Chip width %3f mm,Chip height %3f mm\n",chipw,chiph);
    printf("Chip pixel width %3f um,Chip pixel height %3f um\n",pixelw,pixelh);
    printf("Chip Max Resolution is %d x %d,depth is %d\n",w,h,bpp);

    ret = SetQHYCCDResolution(camhandle,0,0,w,h);

    if(ret == QHYCCD_SUCCESS){
        printf("SetQHYCCDResolution success!\n");
    }else{
        printf("SetQHYCCDResolution fail\n");
        return 1;
    }


    int length = GetQHYCCDMemLength(camhandle);
    ImgData = (unsigned char *)malloc(length);
    memset(ImgData,0,length);

    int t_start,t_end;
    t_start = time(NULL);
    int fps = 0;

    // EnableQHYCCDMessage(true);

    SetQHYCCDParam(camhandle, CONTROL_EXPOSURE, 100000.0);

    int frame_count = 0;
    cv::Mat img;
    cv::namedWindow("show", cv::WINDOW_NORMAL);

    char input;
    while(true){

        std::cout << " 'q' = quit      any = continue ";
        std::cin >> input;

        if (input == 'q' || input == 'Q') {
            std::cout << "quit \n";
            break;
        }

        ExpQHYCCDSingleFrame(camhandle);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        ret = GetQHYCCDSingleFrame(camhandle, &w, &h, &bpp, &channels, ImgData);

        if(ret == QHYCCD_SUCCESS){
            printf("iCnt = %d\n", frame_count++);
            img = cv::Mat(h, w, CV_8UC1, ImgData);
            cv::imshow("show", img);
            cv::waitKey(30);
            fps++;
            t_end = time(NULL);
            if(t_end - t_start >= 5){
                fprintf(stderr, "|QHYCCD|LIVE_DEMO|fps = %d\n", fps / 5);
                fps = 0;
                t_start = time(NULL);
            }
            cv::waitKey(100);
        }
        cv::waitKey(10);
    }

    CloseQHYCCD(camhandle);
    ReleaseQHYCCDResource();
    return 0;

}


