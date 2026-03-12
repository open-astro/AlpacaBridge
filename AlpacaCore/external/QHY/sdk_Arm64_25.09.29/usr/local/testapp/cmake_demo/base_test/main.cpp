#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <cstring>
#include <qhyccd.h>
#include <ctime>
#include <iostream>
#include <unistd.h>



unsigned char buffer[15*1024*1024];

void SDKVersion()
{
    unsigned int  YMDS[4];
    unsigned char sVersion[80];

    memset ((char *)sVersion,0x00,sizeof(sVersion));
    GetQHYCCDSDKVersion(&YMDS[0],&YMDS[1],&YMDS[2],&YMDS[3]);

    if ((YMDS[1] < 10)&&(YMDS[2] < 10))
    {
        sprintf((char *)sVersion,"V20%d0%d0%d_%d\n",YMDS[0],YMDS[1],YMDS[2],YMDS[3]	);
    }
    else if ((YMDS[1] < 10)&&(YMDS[2] > 10))
    {
        sprintf((char *)sVersion,"V20%d0%d%d_%d\n",YMDS[0],YMDS[1],YMDS[2],YMDS[3]	);
    }
    else if ((YMDS[1] > 10)&&(YMDS[2] < 10))
    {
        sprintf((char *)sVersion,"V20%d%d0%d_%d\n",YMDS[0],YMDS[1],YMDS[2],YMDS[3]	);
    }
    else
    {
        sprintf((char *)sVersion,"V20%d%d%d_%d\n",YMDS[0],YMDS[1],YMDS[2],YMDS[3]	);
    }

    fprintf(stderr,"QHYCCD SDK Version: %s\n", sVersion);
}


void FirmWareVersion(qhyccd_handle *h)
{
    int i = 0;
    unsigned char fwv[32],FWInfo[256];
    unsigned int ret;
    memset (FWInfo,0x00,sizeof(FWInfo));
    ret = GetQHYCCDFWVersion(h,fwv);
    if(ret == QHYCCD_SUCCESS)
    {
        if((fwv[0] >> 4) <= 9)
        {

            sprintf((char *)FWInfo,"Firmware version:20%d_%d_%d\n",((fwv[0] >> 4) + 0x10),
                    (fwv[0]&~0xf0),fwv[1]);

        }
        else
        {

            sprintf((char *)FWInfo,"Firmware version:20%d_%d_%d\n",(fwv[0] >> 4),
                    (fwv[0]&~0xf0),fwv[1]);

        }
    }
    else
    {
        sprintf((char *)FWInfo,"Firmware version:Not Found!\n");
    }
    fprintf(stderr,"%s\n", FWInfo);

}


int main(int argc,char *argv[])
{
    int num = 0;
    qhyccd_handle *camhandle;
    int ret;
    char id[32];
    //char camtype[16];
    int found = 0;
    unsigned int w,h,bpp,channels;
    unsigned char *ImgData;
    //int camtime = 10000,camgain = 0,camspeed = 2,cambinx = 1,cambiny = 1;

    SDKVersion();


    ret = InitQHYCCDResource();
    EnableQHYCCDMessage(false);
    if(ret == QHYCCD_SUCCESS)
    {
        printf("Init SDK success    ------!\n");
    }
    else
    {
        goto failure;
    }
    num = ScanQHYCCD();
    if(num > 0)
    {
        printf("Yes!Found QHYCCD,the num is %d \n",num);
    }
    else
    {
        printf("Not Found QHYCCD,please check the usblink or the power\n");
        goto failure;
    }

    for(int i = 0;i < num;i++)
    {
        ret = GetQHYCCDId(i,id);
        if(ret == QHYCCD_SUCCESS)
        {
            printf("connected to the first camera from the list,id is %s\n",id);
            found = 1;
            break;
        }
    }

    if(found == 1)
    {
        camhandle = OpenQHYCCD(id);
        if(camhandle != NULL)
        {
            printf("Open QHYCCD success!\n");
        }
        else
        {
            printf("Open QHYCCD failed \n");
            goto failure;
        }



        SetQHYCCDReadMode(camhandle,4);
        FirmWareVersion(camhandle);

        // check camera support live frame
        ret = IsQHYCCDControlAvailable(camhandle, CAM_LIVEVIDEOMODE);
        if (QHYCCD_ERROR == ret)
        {
            printf("The detected camera is not support live frame.\n");
            // release sdk resources
            ret = ReleaseQHYCCDResource();
            if (QHYCCD_SUCCESS == ret)
            {
                printf("SDK resources released.\n");
            }
            else
            {
                printf("Cannot release SDK resources, error %d.\n", ret);
            }
            return 1;
        }

        ret = SetQHYCCDStreamMode(camhandle,1);


        ret = InitQHYCCD(camhandle);
        if(ret == QHYCCD_SUCCESS)
        {
            printf("bit = 8.2\n");
            printf("Init QHYCCD success -------------!\n");

        }
        else
        {
            printf("Init QHYCCD fail code:%d\n",ret);
            goto failure;
        }



        ret = IsQHYCCDControlAvailable(camhandle,CONTROL_TRANSFERBIT);
        if(ret == QHYCCD_SUCCESS)
        {
            ret = SetQHYCCDBitsMode(camhandle,8);
            printf("bit = 8\n");
//            ret = SetQHYCCDBitsMode(camhandle,16);
            if(ret != QHYCCD_SUCCESS)
            {
                printf("SetQHYCCDParam CONTROL_GAIN failed\n");
                getchar();
                return 1;
            }
        }


        ret = SetQHYCCDBitsMode(camhandle,8);
        printf("bit = 8.2\n");


        double chipw,chiph,pixelw,pixelh;
        ret = GetQHYCCDChipInfo(camhandle,&chipw,&chiph,&w,&h,&pixelw,&pixelh,&bpp);
        if(ret == QHYCCD_SUCCESS)
        {
            printf("GetQHYCCDChipInfo success!\n");
            printf("CCD/CMOS chip information:\n");
            printf("Chip width %3f mm,Chip height %3f mm\n",chipw,chiph);
            printf("Chip pixel width %3f um,Chip pixel height %3f um\n",pixelw,pixelh);
            printf("Chip Max Resolution is %d x %d,depth is %d\n",w,h,bpp);
        }
        else
        {
            printf("GetQHYCCDChipInfo fail\n");
            goto failure;
        }


        ret = SetQHYCCDResolution(camhandle,0,0,w,h);
        if(ret == QHYCCD_SUCCESS)
        {
            printf("SetQHYCCDResolution success!\n");
        }
        else
        {
            printf("SetQHYCCDResolution fail\n");
            goto failure;
        }



        int length = GetQHYCCDMemLength(camhandle);

        if(length > 0)
        {
            ImgData = (unsigned char *)malloc(length);
            memset(ImgData,0,length);
        }
        else
        {
            printf("Get the min memory space length failure \n");
            goto failure;
        }


        int t_start,t_end;
        t_start = time(NULL);
        int fps = 0;

        unsigned int ddrnum=0;


        EnableQHYCCDMessage(true);
        ret = QHYCCD_ERROR;
        for(int i=0;i<4;i++)
        {
            usleep(2000000);

            std::cout<< "  +  " << std::endl;


            double target_t = 10;
            SetQHYCCDParam(camhandle, CONTROL_COOLER, target_t);

            double now_t = -99;
            now_t = GetQHYCCDParam(camhandle, CONTROL_CURTEMP);
            std::cout<< now_t << std::endl;

            double now_p = -99;
            now_p = GetQHYCCDParam(camhandle, CONTROL_CURPWM);
            std::cout<< now_p << std::endl;

        }

    }
    else
    {
        printf("The camera is not QHYCCD or other error \n");
        goto failure;
    }

    if(camhandle)
    {
        StopQHYCCDLive(camhandle);

        ret = CloseQHYCCD(camhandle);
        if(ret == QHYCCD_SUCCESS)
        {
            printf("Close QHYCCD success!\n");
        }
        else
        {
            goto failure;
        }
    }

    ret = ReleaseQHYCCDResource();
    if(ret == QHYCCD_SUCCESS)
    {
        printf("Rlease SDK Resource  success!\n");
    }
    else
    {
        goto failure;
    }

    return 0;

    failure:
    printf("some fatal error happened\n");
    return 1;
}
//~/sdk/QHYCCD_SDK_CrossPlatform
// svn up
//make
//./
