#pragma once
#ifdef __cplusplus
extern "C" {
#endif

// Local change: this copy is a static library (CMake target `bbx_atrac9`), not the
// upstream DLL, so the declarations must not carry an import attribute - a caller
// of `__declspec(dllimport) Atrac9Decode` asks the linker for `__imp_Atrac9Decode`,
// which a static archive never defines. Define ATRAC9_STATIC to take the plain path.
#if defined(_MSC_VER) && !defined(ATRAC9_STATIC)
#ifdef COMPILING_DLL 
#define DLLEXPORT __declspec(dllexport)
#else
#define DLLEXPORT __declspec(dllimport)  
#endif
#else
#define DLLEXPORT
#endif

#define ATRAC9_CONFIG_DATA_SIZE 4

typedef struct {
	int channels;
	int channelConfigIndex;
	int samplingRate;
	int superframeSize;
	int framesInSuperframe;
	int frameSamples;
	int wlength;
	unsigned char configData[ATRAC9_CONFIG_DATA_SIZE];
} Atrac9CodecInfo;

DLLEXPORT void* Atrac9GetHandle(void);
DLLEXPORT void Atrac9ReleaseHandle(void* handle);

DLLEXPORT int Atrac9InitDecoder(void* handle, unsigned char *pConfigData);
DLLEXPORT int Atrac9Decode(void* handle, const unsigned char *pAtrac9Buffer, short *pPcmBuffer, int *pNBytesUsed);

DLLEXPORT int Atrac9GetCodecInfo(void* handle, Atrac9CodecInfo *pCodecInfo);

#ifdef __cplusplus
}
#endif
