#include <cstring>
#include <qhyccd.h>
#include <opencv2/opencv.hpp>
#include <time.h>
#include <vector>



int main(int argc,char *argv[]){
    std::vector<qhyccd_handle*> camhandles;
    std::vector<long> cam_frames;
    std::vector<unsigned char*> ImgDataArray;
    std::vector<cv::Mat> imgArray;
    int num = 0;
    int ret;
    char id[32];
    unsigned int w,h,bpp,channels;
    std::vector<cv::Mat> defaultImgArray;


    ret = InitQHYCCDResource();
    EnableQHYCCDMessage(true);
    num = ScanQHYCCD();
    printf("Found %d cameras  \n",num);
    if(num == 0){
        printf("No camera found\n");
        return 1;
    }

    for(int i = 0; i < num; i++){
        ret = GetQHYCCDId(i,id);
        printf("connected to the %d camera from the list,id is %s\n", i, id);
        qhyccd_handle* camhandle = OpenQHYCCD(id);
        if(camhandle){
            camhandles.push_back(camhandle);
            cam_frames.push_back(0);
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
                CloseQHYCCD(camhandle);
                continue;
            }
            int length = GetQHYCCDMemLength(camhandle);
            unsigned char *ImgData = (unsigned char *)malloc(length);
            memset(ImgData,0,length);
            ImgDataArray.push_back(ImgData);
            SetQHYCCDParam(camhandle, CONTROL_EXPOSURE, 500000.0);
            BeginQHYCCDLive(camhandle);

            cv::Mat defaultImg = cv::Mat::zeros(h, w, CV_8UC1);
            defaultImg.setTo(cv::Scalar(i * 50));
            defaultImgArray.push_back(defaultImg);
            imgArray.push_back(defaultImg);
        }
    }

    if(camhandles.empty()){
        printf("No camera initialized successfully\n");
        ReleaseQHYCCDResource();
        return 1;
    }

    int max_frame = 3000;
    int now_frame = 0;
    int t_start,t_end;
    t_start = time(NULL);
    int fps = 0;
    int frame_count = 0;
    int64 start = cv::getTickCount();
    cv::namedWindow("show", cv::WINDOW_NORMAL);
    cv::resizeWindow("show", 1200, 600);

    imgArray.resize(camhandles.size());
    while(now_frame < max_frame){
       if(cv::getWindowProperty("show", cv::WND_PROP_VISIBLE) <= 0){
            break;
       }

        for(size_t i = 0; i < camhandles.size(); i++){
            ret = GetQHYCCDLiveFrame(camhandles[i], &w, &h, &bpp, &channels, ImgDataArray[i]);
            if(ret == QHYCCD_SUCCESS){
                cv::Mat img = cv::Mat(h, w, CV_8UC1, ImgDataArray[i]);
                imgArray[i] = img;

                cam_frames[i] = cam_frames[i] +1;
                std::string text = "Cam " + std::to_string(i);
                std::string text_id = id;
                std::string text_frames = std::to_string(cam_frames[i]);
                int fontFace = cv::FONT_HERSHEY_SIMPLEX;
                double fontScale = 4.0;
                cv::Scalar color(255);
                int thickness = 2;

                int baseline = 0;
                cv::Size textSize = cv::getTextSize(text, fontFace, fontScale, thickness, &baseline);
                cv::Point textOrg((w - textSize.width) / 2, 100);
                cv::putText(imgArray[i], text, textOrg, fontFace, fontScale, color, thickness);
                cv::Point textOrg_id((w - textSize.width) / 5, 200);
                cv::putText(imgArray[i], text_id, textOrg_id, fontFace, fontScale, color, thickness);
                cv::Point textOrg_frames((w - textSize.width) / 2, 300);
                cv::putText(imgArray[i], text_frames, textOrg_frames, fontFace, fontScale, color, thickness);
                now_frame++;
            }
        }

        if(!imgArray.empty()){
            cv::Mat combined;
            cv::hconcat(imgArray, combined);
            cv::imshow("show", combined);
            cv::waitKey(30);
            fps++;
            t_end = time(NULL);
            if(t_end - t_start >= 5){
                fprintf(stderr, "|QHYCCD|LIVE_DEMO|fps = %d\n", fps / 5);
                fps = 0;
                t_start = time(NULL);
            }
        }
        int key = cv::waitKey(30);
        if(key == 27){ break;}
    }

    for(size_t i = 0; i < camhandles.size(); i++){
        StopQHYCCDLive(camhandles[i]);
        CloseQHYCCD(camhandles[i]);
        free(ImgDataArray[i]);
    }
    ReleaseQHYCCDResource();
    cv::destroyWindow("show");
    return 0;
}


