#ifndef _CLOUDXR_STREAMING_SERVER_
#define _CLOUDXR_STREAMING_SERVER_

#include "cloudxr_streaming_common.h"
#include "cloudxr_input_events.h"

#ifdef SCloudXRServerLib_EXPORTS
#define SCXR_SERVER_API __declspec(dllexport)
#else
#define SCXR_SERVER_API __declspec(dllimport)
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    scxrGraphicDeviceType_D3D11 = 0,
    scxrGraphicDeviceType_OpenGL,
    scxrGraphicDeviceType_Vulkan
}scxrGaphicDeviceType;

typedef enum {
    scxrServerConnectionState_ReadyToConnect = 0,                  ///< ��ʼ��״̬
    scxrServerConnectionState_ConnectionAttemptInProgress = 1,     ///< �������ӷ�����
    scxrServerConnectionState_ConnectionAttemptFailed = 2,         ///< ���ӷ�����ʧ�ܣ������������������ʧ�ܣ�ice���Ӵ����
    scxrServerConnectionState_StreamingSessionInProgress = 3,      ///< ���ӳɹ�����ʼstreaming�����״̬����Կ�ʼ����pose�Լ�input����
    scxrServerConnectionState_Disconnected = 4,                    ///< �Ͽ�����
    scxrServerConnectionState_Exiting = 5                          ///< �ͻ��˽���
}scxrServerConnectionState;

typedef enum {
    scxrServerSyncEvent_NewFrameReady = 0,
    scxrServerSyncEvent_WaitNewFrame,
    scxrServerSyncEvent_EncodeFinished,
    scxrServerSyncEvent_WaitEncoding,
    scxrServerSyncEvent_EncodeEntered,
    scxrServerSyncEvent_EncodeExited,
    scxrServerSyncEvent_WaitEncodeExiting
}scxrServerSyncEvent ;

typedef enum {
    scxrTextureFormat_R8G8B8A8
}scxrTextureFormat;

typedef struct scxrTextureData {
    int frameId;
    void* textureHandle;
} scxrTextureData;

typedef void* scxrGraphicDevice;
typedef struct scxrServer* scxrServerHandle;

typedef struct scxrServerCallback {
    void (*projectionCallback)(float* proj);
#ifdef _WIN32
	void (*stateCallback)(scxrXRTrackingState state, double hmdTimeOffset, double controllerTimeOffset);  
#else
	void (*stateCallback)(scxrXRTrackingState state);
#endif
    void (*inputCallback)(scxrInputEvent2 input);
    void (*connectState)(scxrServerConnectionState state, const char *msg);
    void (*logout)(const char* msg);
    void (*saveTexture)(void* texture, const char* name);
    void (*ipdCallback)(float ipd);
    void (*renderReconfig)(int width, int height, int devModel, int fps, bool ffrEnable);
} scxrServerCallbacks;



typedef struct scxrRenderEngineParam {
    scxrGaphicDeviceType graphicDeviceType;
    scxrGraphicDevice graphicDevice;
    scxrTextureFormat textureFormat;
} scxrEngineParam;

typedef struct scxrGraphicDeviceVulkan {
    void* vkInstance;
    void* vkPhysicDevice;
    void* vkDevice;
    void* vkGraphicQueue;
    void* vkInstanceGetProcAddrFn;
    uint32_t queueFamilyIndex;
} scxrGraphicDeviceVulkan;

typedef struct scxrEncodeParam {
    int width;
    int height;
    int frameRate;
    int bitRate;
    bool isAuto;
	scxrBitrateControlMode bitrateMode;
} scxrEncodeParam;

typedef struct scxrServiceParam
{
    uint32_t port;
    char* ipAddr;
    char* configPath;
    char* profileConfigPath;
}scxrServiceParam;

typedef struct scxrServerDesc {
    uint32_t version;
    scxrEncodeParam encodeParam;
    scxrServiceParam serviceParam;
    scxrEngineParam engineParam;
    scxrServerCallbacks callbacks;
    scxrDeviceModel deviceModel;
    scxrRenderMethod renderMethod;
    bool audioEnable;
    bool enablePaas;
    bool enableLowLatencyMode;
	bool imageQualityAssessOpen;
    bool microphoneEnable;
} scxrServerDesc;

#if defined(_WIN32)
SCXR_SERVER_API scxrResult scxrServerInitialize(scxrServerHandle* serverHandler, scxrServerDesc* serverDesc);

SCXR_SERVER_API scxrResult scxrServerConnect(scxrServerHandle serverHandle);

SCXR_SERVER_API scxrResult scxrServerSendVideoFrame(scxrServerHandle serverHandler, scxrTextureData* textureHandle);

SCXR_SERVER_API scxrResult scxrServerSendHaptics(scxrServerHandle serverHandler, scxrHapticFeedback* haptics);

SCXR_SERVER_API scxrResult scxrServerSetRates(scxrServerHandle serverHandler, int bitRate, int frameRate);

SCXR_SERVER_API scxrResult scxrServerSetRatesAuto(scxrServerHandle serverHandler, bool isAutoRate);

SCXR_SERVER_API scxrResult scxrServerDispose(scxrServerHandle serverHandler);

SCXR_SERVER_API scxrResult scxrServerUpdateResolution(scxrServerHandle serverHandler, uint32_t width, uint32_t height);

SCXR_SERVER_API scxrResult scxrServerSyncWithEvent(scxrServerHandle serverHandler, scxrServerSyncEvent event);

#elif defined(__linux__)
scxrResult scxrServerInitialize(scxrServerHandle* serverHandler, scxrServerDesc* serverDesc);

scxrResult scxrServerConnect(scxrServerHandle serverHandle);

scxrResult scxrServerSendVideoFrame(scxrServerHandle serverHandler, scxrTextureData* textureHandle);

scxrResult scxrServerSetRates(scxrServerHandle serverHandler, int bitRate, int frameRate);

scxrResult scxrServerSetRatesAuto(scxrServerHandle serverHandler, bool isAutoRate);

scxrResult scxrServerDispose(scxrServerHandle serverHandler);

scxrResult scxrServerUpdateResolution(scxrServerHandle serverHandler, uint32_t width, uint32_t height);
#endif

#ifdef __cplusplus
}
#endif

#endif