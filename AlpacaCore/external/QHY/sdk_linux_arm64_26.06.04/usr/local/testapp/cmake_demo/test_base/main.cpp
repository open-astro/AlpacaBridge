#include <cstring>
#include <qhyccd.h>


int main(int argc,char *argv[]){

    int num;
    qhyccd_handle *camhandle;
    int ret;
    char id[32];
    unsigned int w,h,bpp;
    unsigned char *ImgData;

    unsigned int  YMDS[4];
    GetQHYCCDSDKVersion(&YMDS[0],&YMDS[1],&YMDS[2],&YMDS[3]);
    printf("SDK Version: %d.%d.%d_%d\n", YMDS[0], YMDS[1], YMDS[2], YMDS[3]);

    InitQHYCCDResource();

    EnableQHYCCDMessage(true);

    num = ScanQHYCCD();
    printf("Found %d cameras  \n",num);

    if(num == 0){
        printf("No camera found\n");
        return 1;
    }

    GetQHYCCDId(0,id);
    printf("connected to the first camera from the list,id is %s\n",id);


    camhandle = OpenQHYCCD(id);
    uint8_t fwv[32];
    uint8_t fwv_version[3];
    memset(fwv, 0, 32);
    memset(fwv_version, 0, 3);
    GetQHYCCDFWVersion(camhandle,fwv);
    fwv_version[0] = (fwv[0] >> 4) <=9 ? (fwv[0] >> 4) + 0x10 : (fwv[0] >> 4);
    fwv_version[1] = fwv[0] & ~0xf0;
    fwv_version[2] = fwv[1];
    printf("firmware version: %d_%d_%d\n", fwv_version[0], fwv_version[1], fwv_version[2]);


    SetQHYCCDReadMode(camhandle,0);
    SetQHYCCDStreamMode(camhandle,LIVE_MODE);
    InitQHYCCD(camhandle);

    SetQHYCCDBitsMode(camhandle,8);
    double chipw,chiph,pixelw,pixelh;
    GetQHYCCDChipInfo(camhandle,&chipw,&chiph,&w,&h,&pixelw,&pixelh,&bpp);
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

    CloseQHYCCD(camhandle);
    ReleaseQHYCCDResource();
    return 0;

}