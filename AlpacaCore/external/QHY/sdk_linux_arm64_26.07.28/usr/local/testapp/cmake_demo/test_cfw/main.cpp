#include <cstring>
#include <qhyccd.h>


int main(int argc,char *argv[]){

    int num;
    qhyccd_handle *camhandle;
    int ret;
    char id[32];
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

    SetQHYCCDReadMode(camhandle,0);
    SetQHYCCDStreamMode(camhandle,SINGLE_MODE);
    InitQHYCCD(camhandle);


    ret = IsQHYCCDCFWPlugged(camhandle);
    printf("CFW plugged in (%s)",  ret==QHYCCD_SUCCESS?"Yes":"No");

    int max_position = 0;
    int current_position = 0;

    max_position =(int)GetQHYCCDParam(camhandle,CONTROL_CFWSLOTSNUM);
    printf("count: (%d) ",  max_position);

    ret =GetQHYCCDParam(camhandle,CONTROL_CFWPORT);
    printf("current: (%d) ",  ret);

    current_position =(int)GetQHYCCDParam(camhandle,CONTROL_CFWPORT);
    printf("current: (%d) ",  (current_position-48));

    int next_position = current_position + 1;
    next_position = next_position>(max_position+48-1)?48:next_position;
    ret =SetQHYCCDParam(camhandle,CONTROL_CFWPORT,next_position);
    printf("set to:%d  ret:(%d)", next_position-48, ret);


    return 0;

}