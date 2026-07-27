#ifndef DLLQHYCCD_H
#define DLLQHYCCD_H

#include "qhyccdstruct.h"
typedef void qhyccd_handle;
#define QHYCCD_SUCCESS                  0
#define QHYCCD_ERROR                    0xFFFFFFFF
#define QHYCCD_READ_DIRECTLY            0x2001
#define QHYCCD_DELAY_200MS              0x2000
#ifndef  Q_OS_WIN32
#define __stdcall
#endif



class dllqhyccd
{
public:
    typedef  uint32_t (__stdcall *QTInitQHYCCDResource)(void);
    typedef  uint32_t (__stdcall *QTGetQHYCCDFWVersion)(qhyccd_handle *,uint8_t*);
    typedef  uint32_t (__stdcall *QTGetQHYCCDFPGAVersion)(qhyccd_handle *,uint8_t,uint8_t*);
    typedef  uint32_t (__stdcall *QTGetQHYCCDSDKVersion)(uint32_t *,uint32_t *,uint32_t *,uint32_t *);
    typedef  uint32_t (__stdcall *QTGetQHYCCDSDKBuildVersion)(void);
    typedef  uint32_t (__stdcall *QTIsQHYCCDControlAvailable)(qhyccd_handle *,CONTROL_ID);
    typedef  uint32_t (__stdcall *QTGetQHYCCDChipInfo)(qhyccd_handle *,double *,double *,uint32_t *,uint32_t *,double *,double *,uint32_t *);
    typedef  uint32_t (__stdcall *QTGetQHYCCDMemLength)(qhyccd_handle *);
    typedef  double (__stdcall *QTGetQHYCCDParam)(qhyccd_handle *,CONTROL_ID);
    typedef  uint32_t (__stdcall *QTGetQHYCCDParamMinMaxStep)(qhyccd_handle *,CONTROL_ID,double *,double *,double *);
    typedef  uint32_t (__stdcall *QTGetQHYCCDHumidity)(qhyccd_handle *,double *);
    typedef  uint32_t (__stdcall *QTGetQHYCCDPressure)(qhyccd_handle *,double *);
    typedef  uint32_t (__stdcall *QTGetQHYCCDNumberOfReadModes)(qhyccd_handle *,uint32_t *);
    typedef  uint32_t (__stdcall *QTGetQHYCCDReadModeName)(qhyccd_handle *,uint32_t , char*);
    typedef  uint32_t (__stdcall *QTGetQHYCCDReadMode)(qhyccd_handle *,uint32_t);
    typedef  double (__stdcall *QTGetQHYCCDReadingProgress)(qhyccd_handle *);
    typedef  uint32_t (__stdcall *QTGetQHYCCDOverScanArea)(qhyccd_handle *,uint32_t *, uint32_t *, uint32_t *, uint32_t *);
    typedef  uint32_t (__stdcall *QTGetQHYCCDEffectiveArea)(qhyccd_handle *,uint32_t *, uint32_t *, uint32_t *, uint32_t *);
    typedef  uint32_t (__stdcall *QTCancelQHYCCDExposingAndReadout)(qhyccd_handle *);
    typedef  uint32_t (__stdcall *QTCloseQHYCCD)(qhyccd_handle *);
    typedef  uint32_t (__stdcall *QTReleaseQHYCCDResource)(void);
    typedef  uint32_t (__stdcall *QTScanQHYCCD)(void);
    typedef  uint32_t (__stdcall *QTGetQHYCCDId)(uint32_t,char*);
    typedef  qhyccd_handle * (__stdcall *QTOpenQHYCCD)(char*);
    typedef  uint32_t (__stdcall *QTSetQHYCCDParam)(qhyccd_handle *,CONTROL_ID, double);
    typedef  uint32_t (__stdcall *QTSetQHYCCDReadMode)(qhyccd_handle *,uint32_t);
    typedef  uint32_t (__stdcall *QTSetQHYCCDStreamMode)(qhyccd_handle *,uint8_t);
    typedef  uint32_t (__stdcall *QTSetQHYCCDBinMode)(qhyccd_handle *,uint32_t,uint32_t);
    typedef  uint32_t (__stdcall *QTSetQHYCCDResolution)(qhyccd_handle *,uint32_t ,uint32_t,uint32_t,uint32_t);
    typedef  uint32_t (__stdcall *QTInitQHYCCD)(qhyccd_handle *);
    typedef  uint32_t (__stdcall *QTQHYCCDI2CTwoWrite)(qhyccd_handle *,uint16_t,uint16_t);
    typedef  uint32_t (__stdcall *QTQHYCCDI2CTwoRead)(qhyccd_handle *,uint16_t);
    typedef  uint32_t (__stdcall *QTExpQHYCCDSingleFrame)(qhyccd_handle *);
    typedef  uint32_t (__stdcall *QTBeginQHYCCDLive)(qhyccd_handle *);
    typedef  uint32_t (__stdcall *QTGetQHYCCDSingleFrame)(qhyccd_handle *,uint32_t *,uint32_t *,uint32_t *,uint32_t *,uint8_t *);
    typedef  uint32_t (__stdcall *QTGetQHYCCDLiveFrame)(qhyccd_handle *,uint32_t *,uint32_t *,uint32_t *,uint32_t *,uint8_t *);
    typedef  uint32_t (__stdcall *QTControlQHYCCDShutter)(qhyccd_handle *,uint8_t);
    typedef  uint32_t (__stdcall *QTSetQHYCCDTrigerFunction)(qhyccd_handle *,bool);
    typedef  uint32_t (__stdcall *QTSendFourLine2QHYCCDInterCamOled)(qhyccd_handle *,char *,char *,char *,char *);
    typedef  uint32_t (__stdcall *QTSendOrder2QHYCCDCFW)(qhyccd_handle *,char *,uint32_t);
    typedef  uint32_t (__stdcall *QTGetQHYCCDCFWStatus)(qhyccd_handle *,char *);
    typedef  uint32_t (__stdcall *QTOSXInitQHYCCDFirmware)(char *);
    typedef  uint32_t (__stdcall *QTSetQHYCCDFineTone)(qhyccd_handle *,uint8_t ,uint8_t ,uint8_t ,uint8_t);
    typedef  uint32_t (__stdcall *QTEnableQHYCCDImageOSD)(qhyccd_handle *,uint32_t);
    typedef  uint32_t (__stdcall *QTTestQHYCCDPIDParas)(qhyccd_handle *, double, double, double);
    typedef  int (__stdcall *QTCheckPHD2Status) (char *);
    typedef  int (__stdcall *QTConnect2PHD2)(void);
    typedef  int (__stdcall *QTDisConnect2PHD2)(void);
    typedef  int (__stdcall *QTControlPHD2Dither)(int,double);
    typedef  int (__stdcall *QTPausePHD2Dither)(void);
    typedef  void (__stdcall *QTEnableQHYCCDMessage)(bool);

    typedef  void(__stdcall *QTRegisterPnpEvent)(void(*)());
    typedef  void(__stdcall *QTRegisterTransferEventError)(void(*)());
    typedef  void(__stdcall *QTRegisterPnpEventIn)(void(*)(char*));
    typedef  void(__stdcall *QTRegisterPnpEventOut)(void(*)(char*));
    typedef  void(__stdcall *QTRegisterDataEventSingle)(void(*)(char*,uint8_t *));
    typedef  void(__stdcall *QTRegisterDataEventLive)(void(*)(char*,uint8_t *));
    typedef  uint8_t(__stdcall *QTGetCameraIsSuperSpeedFromID)(char *);
    typedef  uint8_t(__stdcall *QTGetCameraStatusFromID)(char *);
    typedef  uint32_t(__stdcall *QTControlQHYCCDGuide)(qhyccd_handle *, uint32_t,uint16_t);

    QTInitQHYCCDResource InitQHYCCDResource=NULL;
    QTGetQHYCCDFWVersion GetQHYCCDFWVersion=NULL;
    QTGetQHYCCDFPGAVersion GetQHYCCDFPGAVersion=NULL;
    QTGetQHYCCDSDKVersion GetQHYCCDSDKVersion=NULL;
    QTGetQHYCCDSDKBuildVersion GetQHYCCDSDKBuildVersion=NULL;
    QTIsQHYCCDControlAvailable IsQHYCCDControlAvailable=NULL;
    QTGetQHYCCDChipInfo GetQHYCCDChipInfo=NULL;
    QTGetQHYCCDMemLength GetQHYCCDMemLength=NULL;
    QTGetQHYCCDParam GetQHYCCDParam=NULL;
    QTGetQHYCCDParamMinMaxStep GetQHYCCDParamMinMaxStep=NULL;
    QTGetQHYCCDHumidity GetQHYCCDHumidity=NULL;
    QTGetQHYCCDPressure GetQHYCCDPressure=NULL;
    QTGetQHYCCDNumberOfReadModes GetQHYCCDNumberOfReadModes=NULL;
    QTGetQHYCCDReadModeName GetQHYCCDReadModeName=NULL;
    QTGetQHYCCDReadMode GetQHYCCDReadMode=NULL;
    QTGetQHYCCDReadingProgress GetQHYCCDReadingProgress=NULL;
    QTGetQHYCCDOverScanArea GetQHYCCDOverScanArea=NULL;
    QTGetQHYCCDEffectiveArea GetQHYCCDEffectiveArea=NULL;
    QTCancelQHYCCDExposingAndReadout CancelQHYCCDExposingAndReadout=NULL;
    QTCloseQHYCCD CloseQHYCCD=NULL;
    QTReleaseQHYCCDResource ReleaseQHYCCDResource=NULL;
    QTScanQHYCCD ScanQHYCCD=NULL;
    QTGetQHYCCDId GetQHYCCDId=NULL;
    QTOpenQHYCCD OpenQHYCCD=NULL;
    QTSetQHYCCDParam SetQHYCCDParam=NULL;
    QTSetQHYCCDReadMode SetQHYCCDReadMode=NULL;
    QTSetQHYCCDStreamMode SetQHYCCDStreamMode=NULL;
    QTSetQHYCCDBinMode SetQHYCCDBinMode=NULL;
    QTSetQHYCCDResolution SetQHYCCDResolution=NULL;
    QTInitQHYCCD InitQHYCCD=NULL;
    QTQHYCCDI2CTwoWrite QHYCCDI2CTwoWrite=NULL;
    QTQHYCCDI2CTwoRead QHYCCDI2CTwoRead=NULL;
    QTExpQHYCCDSingleFrame ExpQHYCCDSingleFrame=NULL;
    QTBeginQHYCCDLive  BeginQHYCCDLive=NULL;
    QTGetQHYCCDSingleFrame GetQHYCCDSingleFrame=NULL;
    QTGetQHYCCDLiveFrame GetQHYCCDLiveFrame=NULL;
    QTControlQHYCCDShutter ControlQHYCCDShutter=NULL;
    QTSetQHYCCDTrigerFunction SetQHYCCDTrigerFunction=NULL;
    QTSendFourLine2QHYCCDInterCamOled SendFourLine2QHYCCDInterCamOled=NULL;
    QTSendOrder2QHYCCDCFW SendOrder2QHYCCDCFW=NULL;
    QTGetQHYCCDCFWStatus GetQHYCCDCFWStatus=NULL;
    QTOSXInitQHYCCDFirmware OSXInitQHYCCDFirmware=NULL;
    QTSetQHYCCDFineTone SetQHYCCDFineTone=NULL;
    QTEnableQHYCCDImageOSD EnableQHYCCDImageOSD=NULL;
    QTTestQHYCCDPIDParas TestQHYCCDPIDParas=NULL;
    QTCheckPHD2Status  CheckPHD2Status=NULL;
    QTConnect2PHD2  Connect2PHD2=NULL;
    QTDisConnect2PHD2 DisConnect2PHD2=NULL;
    QTControlPHD2Dither  ControlPHD2Dither=NULL;
    QTPausePHD2Dither  PausePHD2Dither=NULL;
    QTEnableQHYCCDMessage EnableQHYCCDMessage=NULL;

    QTRegisterPnpEvent RegisterPnpEvent=NULL;
    QTRegisterTransferEventError RegisterTransferEventError=NULL;
    QTRegisterPnpEventIn RegisterPnpEventIn=NULL;
    QTRegisterPnpEventOut RegisterPnpEventOut=NULL;
    QTRegisterDataEventSingle RegisterDataEventSingle=NULL;
    QTRegisterDataEventLive RegisterDataEventLive=NULL;
    QTGetCameraIsSuperSpeedFromID  GetCameraIsSuperSpeedFromID=NULL;
    QTGetCameraStatusFromID  GetCameraStatusFromID=NULL;
    QTControlQHYCCDGuide ControlQHYCCDGuide=NULL;

    dllqhyccd();
    ~dllqhyccd();
};
extern dllqhyccd *libqhyccd;
#endif // DLLQHYCCD_H
