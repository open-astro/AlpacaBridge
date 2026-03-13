#include <cstring>
#include <qhyccd.h>
#include <opencv2/opencv.hpp>
#include <time.h>



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
    SetQHYCCDStreamMode(camhandle,LIVE_MODE);
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

    // for(int i=0;i<4;i++){
    //     usleep(2000000);
    //     printf("  +  \n");
    //     double target_t = 10;
    //     SetQHYCCDParam(camhandle, CONTROL_COOLER, target_t);
    //     double now_t = -99;
    //     now_t = GetQHYCCDParam(camhandle, CONTROL_CURTEMP);
    //     printf("%.2f\n", now_t);
    //     double now_p = -99;
    //     now_p = GetQHYCCDParam(camhandle, CONTROL_CURPWM);
    //     printf("%.2f\n", now_p);
    // }


    SetQHYCCDParam(camhandle, CONTROL_EXPOSURE, 100000.0);
    BeginQHYCCDLive(camhandle);

    int frame_count = 0;
    cv::Mat img;
    cv::namedWindow("show", cv::WINDOW_NORMAL);
    while(true){
        ret = GetQHYCCDLiveFrame(camhandle, &w, &h, &bpp, &channels, ImgData);
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

    StopQHYCCDLive(camhandle);
    CloseQHYCCD(camhandle);
    ReleaseQHYCCDResource();
    return 0;

}


