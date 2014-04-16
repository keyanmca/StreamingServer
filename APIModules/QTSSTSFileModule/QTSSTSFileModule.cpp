/*
 *
 * @APPLE_LICENSE_HEADER_START@
 *
 * Copyright (c) 1999-2008 Apple Inc.  All Rights Reserved.
 *
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 * 
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 * 
 * @APPLE_LICENSE_HEADER_END@
 *
 */
/*
    File:       QTSSFileModule.cpp

    Contains:   Implementation of module described in QTSSFileModule.h. 
                    


*/

#include <string.h>
#include <sys/time.h>
#include <pthread.h>
#include "QTSSTSFileModule.h"
#include "QTRTPFile.h"
#include "QTFile.h"
#include "OSMemory.h"
#include "OSArrayObjectDeleter.h"
#include "QTSSMemoryDeleter.h"
#include "SDPSourceInfo.h"
#include "StringFormatter.h"
#include "QTSSModuleUtils.h"
#include "QTSS3GPPModuleUtils.h"
#include "ResizeableStringFormatter.h"
#include "StringParser.h"
#include "SDPUtils.h"

#include <errno.h>

#include "QTSS.h"
#define	MAXFILETYPE (3)
#define MAXSLICES (500)
#define CBRMETHOD	1
class TSFileSession
{
public:
	TSFileSession(const char *szFilename):m_lFirstPcr(0), m_lSecondPcr(0), m_nFirstPcrSeq(0), m_nSecondPcrSeq(0), m_nBitRate(0),
	m_lSleepTime(0), m_nCurrentRtpPacketSeq(1988), m_nCurrentTsPacketOffset(0), m_nSendRate(0),m_fLastRateFactor(1.0), m_fRateFactor(1.0),
	cookie1(NULL), cookie2(NULL), m_pSSRC(NULL), m_bSliceHasChanged(false), m_nRtpXHeaderLength(12), m_nSendRateRingCursor(0), m_nLastBuffer(0)
	{
		strcpy(m_szFilePath, szFilename);
		//We firstly open 3 files, which have different bitrate
		char fileName[256] = {0};
		for(int i = 0; i < 3; i++)
		{
			sprintf(fileName, "%s%d.ts", szFilename, i);
			m_FileArray[i].file = fopen(fileName, "rb");
			Assert(m_FileArray[i].file != NULL);
			fseek(m_FileArray[i].file, 0, SEEK_END);
			m_FileArray[i].size = ftell(m_FileArray[i].file);
			fseek(m_FileArray[i].file, 0, SEEK_SET);
		}
		//Load slices and I frame infomation
		{
			for(int i = 0; i < MAXFILETYPE; i++)
			{
				for(int j = 0; j < MAXSLICES; j++)
				{
					m_SliceOffset[i][j] = -1;
					m_IFrameOffset[i][j] = -1;
				}
			}
			sprintf(fileName, "%sfileinfo", szFilename);
			FILE *pFileInfo = fopen(fileName, "r");
			Assert(pFileInfo != NULL);
			char id = 0;
			int	type = -1;
			int offset = -1;
			int i = 0, j = 0;
			int lastType = -1;
			while(fscanf(pFileInfo, "%c\t%d\t%d", &id, &type, &offset) != EOF)
			{
				Assert(type != -1 && offset != -1);
				if(lastType != type)
				{
					lastType = type;
					i = 0;
					j = 0;
				}
				if(id == 't')
				{
					//Calculate movie duration and average bitrate
					m_FileArray[0].duration = m_FileArray[1].duration = m_FileArray[2].duration = offset;
					m_FileArray[0].bitrate = m_FileArray[0].size * 8 / m_FileArray[0].duration;
					m_FileArray[1].bitrate = m_FileArray[1].size * 8 / m_FileArray[1].duration;
					m_FileArray[2].bitrate = m_FileArray[2].size * 8 / m_FileArray[2].duration;
				}
				else if(id == 's')
				{
					m_SliceOffset[type][i++] = offset;
				}
				else if(id == 'i')
				{
					m_IFrameOffset[type][j++] = offset;
				}
			}
		}
		pthread_mutex_init(&m_lock, NULL);
		//Load test scripts
		FILE *pscripts = fopen("script", "r");
		char identifier[10] = {0};
		SInt32  tick;
		Float32 value;
		m_Test.m_nCursor = 0;
		while(fscanf(pscripts, "%s\t%d\t%f", identifier, &tick, &value) != EOF)
		{
			if(identifier[0] == '#')
				continue;
			if(strcmp(identifier, "interval") == 0)
			{
				m_nTimeInterval = value;
			}
			else if(strcmp(identifier, "switch") == 0)
			{
				m_Test.m_nTick[m_Test.m_nCursor] = tick;
				m_Test.m_fValue[m_Test.m_nCursor] = value;
				m_Test.m_nCursor++;
			}
			else if(strcmp(identifier, "startmode") == 0)
			{
				int x = (SInt32)value;
				m_nCurrentSliceType = (SLICETYPE)x;
				m_pCurrentFile = m_FileArray[x].file;
				m_nBitRate = m_FileArray[m_nCurrentSliceType].bitrate;
				m_lSleepTime = (SInt64)188 * 7 * 8 * 1000 * 1000 / m_nBitRate;
			}
		}
		m_FileArray[0].correction = 1;
		m_FileArray[1].correction = 1;
		m_FileArray[2].correction = 1;
		for(int i = 0; i < 4; i++)
			m_nSendRateLast4Times[i] = -1;;
		fileBitrate = fopen("bitrate", "w");
		GetTwoPcr(0);
	}
	~TSFileSession()
	{
		for(int i = 0; i < 3; i++)
		{
			if(m_FileArray[i].file)
			{
				fclose(m_FileArray[i].file);
				m_FileArray[i].file = NULL;
			}
		}
		pthread_mutex_destroy(&m_lock);
	}
	struct FileInfo
	{
		FILE	*file;
		SInt32	duration;
		SInt32	size;
		SInt32	bitrate;
		Float32	correction;
	};
	struct TestScript
	{
		SInt32	m_nTimeInterval;
		SInt32	m_nTick[100];
		Float32	m_fValue[100];
		SInt32	m_nCursor;
	};
	struct FeedBack
	{
		UInt32 switchSlice;
		UInt32 newSlice;
		UInt32 expectedRate;
		UInt32 clientassign;
	};
	SInt32 	m_nTimeInterval;
	TestScript	m_Test;
	FeedBack	m_FeedBack;
	enum SLICETYPE{LOW = 0, MEDIUM, HIGH, };
	//pcr
	SInt64				m_lFirstPcr;
	SInt64				m_lSecondPcr;
	SInt32				m_nFirstPcrSeq;
	SInt32				m_nSecondPcrSeq;
	SInt32				m_nBitRate;
	SInt32				m_nSendRate;
	SInt32				m_nSendRateLast4Times[4];
	SInt32				m_nSendRateRingCursor;
	SInt64				m_lSleepTime;
	SInt64				m_lLastTimeWeGetPcr;
	//
	FILE				*m_pCurrentFile;
	FILE				*m_nNewCurrentFile;
	FileInfo			m_FileArray[3];
	char				m_szFilePath[256];
	SInt32				m_nCurrentRtpPacketSeq;
	void 				*cookie1;
	UInt32 				cookie2;
	UInt32				*m_pSSRC;
	SDPSourceInfo		fSDPSource;
	pthread_mutex_t 	m_lock;
	unsigned char 		m_RtpBuffer[1500];
	QTSS_PacketStruct	m_RtpPacket;
	//Slice
	SLICETYPE			m_nCurrentSliceType;
	SLICETYPE 			m_nNewCurrentSliceType;
	SInt32				m_nCurrentSlice;
	SInt32				m_nCurrentTsPacketOffset;
	SInt32	 			m_nNewCurrentTsPacketOffset;
	bool				m_bSliceHasChanged;
	//Store start positions for per slice
	int 	m_SliceOffset[MAXFILETYPE][MAXSLICES];
	//Store start positions for per I frame
	int		m_IFrameOffset[MAXFILETYPE][MAXSLICES];
	//Method
	UInt32	Read7PacketsAndMakeRtp(SInt64 inTimeStamp);
	void	UpdatePcr(UInt64 lNewPcr, UInt32 nSeq);
	void	SetSSRC(UInt32 *inSSRC){m_pSSRC = inSSRC;}
	void 	SetCookies(void *inCookie1, UInt32 inCookie2){cookie1 = inCookie1; cookie2 = inCookie2;}
	Bool16 	GetTwoPcr(int inPacketOffset);
	void 	GetNextPcr(int inPacketStartOffset);
	UInt32 	CreateRtp(SInt32 inLength, SInt32 inPayloadType, SInt64 inTimeStamp, Bool16 inMarker);
	UInt32	ChangeSlice(SLICETYPE inType);
	SInt32	GetCurrentSliceIndex();
	SInt32	GetIFramePosition(SLICETYPE inType, SInt32 inSliceIndex);
	void 	FlowControl(UInt32 inJitter, UInt32 inBuffer, UInt32 inLoss);
	Float32 GetAvgSendRate();
	bool 	CheckAndDoChangeSlice(UInt32 inJitter);
	void	ProbeHigherSendingRate(UInt32 inJitter, UInt32 inBuffer);
	//For debug
	//FILE 	*m_fileSendLog;
	FILE *fileBitrate;
	SInt32	m_nRtpXHeaderLength;
	Float32	m_fRateFactor;
	Float32 m_fLastRateFactor;
	UInt32	m_nLastBuffer;
};



class FileSession
{
    public:
    
        FileSession() : fAdjustedPlayTime(0), fNextPacketLen(0), fLastQualityCheck(0),
                        fAllowNegativeTTs(false), fSpeed(1),
                        fStartTime(-1), fStopTime(-1), fStopTrackID(0), fStopPN(0),
                        fLastRTPTime(0), fLastPauseTime(0),fTotalPauseTime(0), fPaused(true), fAdjustPauseTime(true)
        { 
          fPacketStruct.packetData = NULL; fPacketStruct.packetTransmitTime = -1; fPacketStruct.suggestedWakeupTime=-1;
        }
        
        ~FileSession()
        {

        }

        QTRTPFile           fFile;
        SInt64              fAdjustedPlayTime;
        QTSS_PacketStruct   fPacketStruct;
        int                 fNextPacketLen;
        SInt64              fLastQualityCheck;
        SDPSourceInfo       fSDPSource;
        Bool16              fAllowNegativeTTs;
        Float32             fSpeed;
        Float64             fStartTime;
        Float64             fStopTime;
        
        UInt32              fStopTrackID;
        UInt64              fStopPN;

        UInt32              fLastRTPTime;
        UInt64              fLastPauseTime;
        SInt64              fTotalPauseTime;
        Bool16              fPaused;
        Bool16              fAdjustPauseTime;
};

// ref to the prefs dictionary object

static bool bStopSending = true;
static bool bStart = true;
static TSFileSession *theCurrentTSFileSession;

static QTSS_ModulePrefsObject       sPrefs;
static QTSS_PrefsObject             sServerPrefs;
static QTSS_Object                  sServer;

static StrPtrLen sSDPSuffix(".sdp");
static  StrPtrLen sVersionHeader("v=0");
static  StrPtrLen sSessionNameHeader("s=");
static  StrPtrLen sPermanentTimeHeader("t=0 0");
static  StrPtrLen sConnectionHeader("c=IN IP4 0.0.0.0");
static StrPtrLen  sStaticControlHeader("a=control:*");
static  StrPtrLen  sEmailHeader;
static  StrPtrLen  sURLHeader;
static  StrPtrLen  sEOL("\r\n");
static  StrPtrLen sSDPNotValidMessage("Movie SDP is not valid.");

const   SInt16    sNumSDPVectors = 22;

// ATTRIBUTES IDs

static QTSS_AttributeID sFileSessionAttr                = qtssIllegalAttrID;

static QTSS_AttributeID sSeekToNonexistentTimeErr       = qtssIllegalAttrID;
static QTSS_AttributeID sNoSDPFileFoundErr              = qtssIllegalAttrID;
static QTSS_AttributeID sBadQTFileErr                   = qtssIllegalAttrID;
static QTSS_AttributeID sFileIsNotHintedErr             = qtssIllegalAttrID;
static QTSS_AttributeID sExpectedDigitFilenameErr       = qtssIllegalAttrID;
static QTSS_AttributeID sTrackDoesntExistErr            = qtssIllegalAttrID;

static QTSS_AttributeID sFileSessionPlayCountAttrID     = qtssIllegalAttrID;
static QTSS_AttributeID sFileSessionBufferDelayAttrID   = qtssIllegalAttrID;

static QTSS_AttributeID sRTPStreamLastSentPacketSeqNumAttrID   = qtssIllegalAttrID;

static QTSS_AttributeID sRTPStreamLastPacketSeqNumAttrID   = qtssIllegalAttrID;

// OTHER DATA

static UInt32				sFlowControlProbeInterval	= 10;
static UInt32				sDefaultFlowControlProbeInterval= 10;
static Float32              sMaxAllowedSpeed            = 4;
static Float32              sDefaultMaxAllowedSpeed     = 4;

// File Caching Prefs
static Bool16               sEnableSharedBuffers    = false;
static Bool16               sEnablePrivateBuffers   = false;

static UInt32               sSharedBufferUnitKSize  = 0;
static UInt32               sSharedBufferInc        = 0;
static UInt32               sSharedBufferUnitSize   = 0;
static UInt32               sSharedBufferMaxUnits   = 0;

static UInt32               sPrivateBufferUnitKSize = 0;
static UInt32               sPrivateBufferUnitSize  = 0;
static UInt32               sPrivateBufferMaxUnits  = 0;

static Float32              sAddClientBufferDelaySecs = 0;

static Bool16               sRecordMovieFileSDP = false;
static Bool16               sEnableMovieFileSDP = false;

static Bool16               sPlayerCompatibility = true;
static UInt32               sAdjustMediaBandwidthPercent = 50;
static SInt64               sAdjustRTPStartTimeMilli = 500;

static Bool16               sAllowInvalidHintRefs = false;

// Server preference we respect
static Bool16               sDisableThinning       = false;
static UInt16               sDefaultStreamingQuality = 0;

static const StrPtrLen              kCacheControlHeader("must-revalidate");
static const QTSS_RTSPStatusCode    kNotModifiedStatus          = qtssRedirectNotModified;


const Bool16				kAddPauseTimeToRTPTime = true;
const Bool16				kDontAddPauseTimeToRTPTime = false;

  
// FUNCTIONS

static QTSS_Error QTSSTSFileModuleDispatch(QTSS_Role inRole, QTSS_RoleParamPtr inParamBlock);
static QTSS_Error Register(QTSS_Register_Params* inParams);
static QTSS_Error Initialize(QTSS_Initialize_Params* inParamBlock);
static QTSS_Error RereadPrefs();
static QTSS_Error ProcessRTSPRequest(QTSS_StandardRTSP_Params* inParamBlock);
static QTSS_Error ProcessRTCP(QTSS_RTCPProcess_Params* inParamBlock);
static QTSS_Error DoDescribe(QTSS_StandardRTSP_Params* inParamBlock);
static QTSS_Error CreateQTTSFile(QTSS_StandardRTSP_Params* inParamBlock, char* inPath, TSFileSession** outFile);
static QTSS_Error DoSetup(QTSS_StandardRTSP_Params* inParamBlock);
static QTSS_Error DoPlay(QTSS_StandardRTSP_Params* inParamBlock);
static QTSS_Error SendPackets(QTSS_RTPSendPackets_Params* inParams);
static QTSS_Error DestroySession(QTSS_ClientSessionClosing_Params* inParams);
static void       DeleteFileSession(FileSession* inFileSession);
static UInt32   WriteSDPHeader(FILE* sdpFile, iovec *theSDPVec, SInt16 *ioVectorIndex, StrPtrLen *sdpHeader);
static void     BuildPrefBasedHeaders();

//New addition by pengguokan
//3/26/2014
static Bool16 GetPCR(unsigned char *p_ts_packet, SInt64 *p_pcr, SInt32 *p_pcr_pid);


QTSS_Error QTSSTSFileModule_Main(void* inPrivateArgs)
{
    return _stublibrary_main(inPrivateArgs, QTSSTSFileModuleDispatch);
}

inline UInt16 GetPacketSequenceNumber(void * packetDataPtr)
{
    return ntohs( ((UInt16*)packetDataPtr)[1]);
}

inline UInt16 GetLastPacketSeqNum(QTSS_Object stream)
{

    UInt16 lastSeqNum = 0;
    UInt32  theLen = sizeof(lastSeqNum);
    (void) QTSS_GetValue(stream, sRTPStreamLastPacketSeqNumAttrID, 0, (void*)&lastSeqNum, &theLen);

    return lastSeqNum;
}


inline SInt32 GetLastSentSeqNumber(QTSS_Object stream)
{
    UInt16 lastSeqNum = 0;
    UInt32  theLen = sizeof(lastSeqNum);
    QTSS_Error error = QTSS_GetValue(stream, sRTPStreamLastSentPacketSeqNumAttrID, 0, (void*)&lastSeqNum, &theLen);
    if (error == QTSS_ValueNotFound) // first packet
    {    return -1;
    }

    return (SInt32)lastSeqNum; // return UInt16 seq num value or -1.
} 

inline void SetPacketSequenceNumber(UInt16 newSequenceNumber, void * packetDataPtr)
{
    ((UInt16*)packetDataPtr)[1] = htons(newSequenceNumber);
}


inline UInt32 GetPacketTimeStamp(void * packetDataPtr)
{
    return ntohl( ((UInt32*)packetDataPtr)[1]);
}

inline void SetPacketTimeStamp(UInt32 newTimeStamp, void * packetDataPtr)
{
    ((UInt32*)packetDataPtr)[1] = htonl(newTimeStamp);
}

inline UInt32 CalculatePauseTimeStamp(UInt32 timescale, SInt64 totalPauseTime, UInt32 currentTimeStamp)
{
    SInt64 pauseTime = (SInt64) ( (Float64) timescale * ( ( (Float64) totalPauseTime) / 1000.0));     
    UInt32 pauseTimeStamp = (UInt32) (pauseTime + currentTimeStamp);

    return pauseTimeStamp;
}

static UInt32 SetPausetimeTimeStamp(FileSession *fileSessionPtr, QTSS_Object theRTPStream, UInt32 currentTimeStamp)
{ 
    if (false == fileSessionPtr->fAdjustPauseTime || fileSessionPtr->fTotalPauseTime == 0)
        return currentTimeStamp;

    UInt32 timeScale = 0;
    UInt32 theLen = sizeof(timeScale);
    (void) QTSS_GetValue(theRTPStream, qtssRTPStrTimescale, 0, (void*)&timeScale, &theLen);    
    if (theLen != sizeof(timeScale) || timeScale == 0)
        return currentTimeStamp;

    UInt32 pauseTimeStamp = CalculatePauseTimeStamp( timeScale,  fileSessionPtr->fTotalPauseTime, currentTimeStamp);
    if (pauseTimeStamp != currentTimeStamp)
        SetPacketTimeStamp(pauseTimeStamp, fileSessionPtr->fPacketStruct.packetData);

    return pauseTimeStamp;
}


UInt32 WriteSDPHeader(FILE* sdpFile, iovec *theSDPVec, SInt16 *ioVectorIndex, StrPtrLen *sdpHeader)
{

    Assert (ioVectorIndex != NULL);
    Assert (theSDPVec != NULL);
    Assert (sdpHeader != NULL);
    Assert (*ioVectorIndex < sNumSDPVectors); // if adding an sdp param you need to increase sNumSDPVectors
    
    SInt16 theIndex = *ioVectorIndex;
    *ioVectorIndex += 1;

    theSDPVec[theIndex].iov_base =  sdpHeader->Ptr;
    theSDPVec[theIndex].iov_len = sdpHeader->Len;
    
    if (sdpFile !=NULL)
        ::fwrite(theSDPVec[theIndex].iov_base,theSDPVec[theIndex].iov_len,sizeof(char),sdpFile);
    
    return theSDPVec[theIndex].iov_len;
}



QTSS_Error  QTSSTSFileModuleDispatch(QTSS_Role inRole, QTSS_RoleParamPtr inParamBlock)
{
    switch (inRole)
    {
        case QTSS_Register_Role:
            return Register(&inParamBlock->regParams);
        case QTSS_Initialize_Role:
            return Initialize(&inParamBlock->initParams);
        case QTSS_RereadPrefs_Role:
            return RereadPrefs();
        case QTSS_RTSPRequest_Role:
            return ProcessRTSPRequest(&inParamBlock->rtspRequestParams);
        case QTSS_RTPSendPackets_Role:
            return SendPackets(&inParamBlock->rtpSendPacketsParams);
        case QTSS_ClientSessionClosing_Role:
            return DestroySession(&inParamBlock->clientSessionClosingParams);
        case QTSS_RTCPProcess_Role:
        	return ProcessRTCP(&inParamBlock->rtcpProcessParams);
    }
    return QTSS_NoErr;
}

QTSS_Error Register(QTSS_Register_Params* inParams)
{
    // Register for roles
    (void)QTSS_AddRole(QTSS_Initialize_Role);
    (void)QTSS_AddRole(QTSS_RTSPRequest_Role);
    (void)QTSS_AddRole(QTSS_RTPSendPackets_Role);
    (void)QTSS_AddRole(QTSS_RTCPProcess_Role);
    (void)QTSS_AddRole(QTSS_ClientSessionClosing_Role);
    //(void)QTSS_AddRole(QTSS_RereadPrefs_Role);

    // Add text messages attributes
    static char*        sSeekToNonexistentTimeName  = "QTSSFileModuleSeekToNonExistentTime";
    static char*        sNoSDPFileFoundName         = "QTSSFileModuleNoSDPFileFound";
    static char*        sBadQTFileName              = "QTSSFileModuleBadQTFile";
    static char*        sFileIsNotHintedName        = "QTSSFileModuleFileIsNotHinted";
    static char*        sExpectedDigitFilenameName  = "QTSSFileModuleExpectedDigitFilename";
    static char*        sTrackDoesntExistName       = "QTSSFileModuleTrackDoesntExist";
    
    (void)QTSS_AddStaticAttribute(qtssTextMessagesObjectType, sSeekToNonexistentTimeName, NULL, qtssAttrDataTypeCharArray);
    (void)QTSS_IDForAttr(qtssTextMessagesObjectType, sSeekToNonexistentTimeName, &sSeekToNonexistentTimeErr);

    (void)QTSS_AddStaticAttribute(qtssTextMessagesObjectType, sNoSDPFileFoundName, NULL, qtssAttrDataTypeCharArray);
    (void)QTSS_IDForAttr(qtssTextMessagesObjectType, sNoSDPFileFoundName, &sNoSDPFileFoundErr);

    (void)QTSS_AddStaticAttribute(qtssTextMessagesObjectType, sBadQTFileName, NULL, qtssAttrDataTypeCharArray);
    (void)QTSS_IDForAttr(qtssTextMessagesObjectType, sBadQTFileName, &sBadQTFileErr);

    (void)QTSS_AddStaticAttribute(qtssTextMessagesObjectType, sFileIsNotHintedName, NULL, qtssAttrDataTypeCharArray);
    (void)QTSS_IDForAttr(qtssTextMessagesObjectType, sFileIsNotHintedName, &sFileIsNotHintedErr);

    (void)QTSS_AddStaticAttribute(qtssTextMessagesObjectType, sExpectedDigitFilenameName, NULL, qtssAttrDataTypeCharArray);
    (void)QTSS_IDForAttr(qtssTextMessagesObjectType, sExpectedDigitFilenameName, &sExpectedDigitFilenameErr);

    (void)QTSS_AddStaticAttribute(qtssTextMessagesObjectType, sTrackDoesntExistName, NULL, qtssAttrDataTypeCharArray);
    (void)QTSS_IDForAttr(qtssTextMessagesObjectType, sTrackDoesntExistName, &sTrackDoesntExistErr);
    
    // Add an RTP session attribute for tracking FileSession objects
    static char*        sFileSessionName    = "QTSSFileModuleSession";
    (void)QTSS_AddStaticAttribute(qtssClientSessionObjectType, sFileSessionName, NULL, qtssAttrDataTypeVoidPointer);
    (void)QTSS_IDForAttr(qtssClientSessionObjectType, sFileSessionName, &sFileSessionAttr);
    
    static char*        sFileSessionPlayCountName   = "QTSSFileModulePlayCount";
    (void)QTSS_AddStaticAttribute(qtssClientSessionObjectType, sFileSessionPlayCountName, NULL, qtssAttrDataTypeUInt32);
    (void)QTSS_IDForAttr(qtssClientSessionObjectType, sFileSessionPlayCountName, &sFileSessionPlayCountAttrID);
    
    static char*        sFileSessionBufferDelayName = "QTSSFileModuleSDPBufferDelay";
    (void)QTSS_AddStaticAttribute(qtssClientSessionObjectType, sFileSessionBufferDelayName, NULL, qtssAttrDataTypeFloat32);
    (void)QTSS_IDForAttr(qtssClientSessionObjectType, sFileSessionBufferDelayName, &sFileSessionBufferDelayAttrID);
    
     static char*        sRTPStreamLastSentPacketSeqNumName   = "QTSSFileModuleLastSentPacketSeqNum";
    (void)QTSS_AddStaticAttribute(qtssRTPStreamObjectType, sRTPStreamLastSentPacketSeqNumName, NULL, qtssAttrDataTypeUInt16);
    (void)QTSS_IDForAttr(qtssRTPStreamObjectType, sRTPStreamLastSentPacketSeqNumName, &sRTPStreamLastSentPacketSeqNumAttrID);
   

    static char*        sRTPStreamLastPacketSeqNumName   = "QTSSFileModuleLastPacketSeqNum";
    (void)QTSS_AddStaticAttribute(qtssRTPStreamObjectType, sRTPStreamLastPacketSeqNumName, NULL, qtssAttrDataTypeUInt16);
    (void)QTSS_IDForAttr(qtssRTPStreamObjectType, sRTPStreamLastPacketSeqNumName, &sRTPStreamLastPacketSeqNumAttrID);

    // Tell the server our name!
    static char* sModuleName = "QTSSFileModule";
    ::strcpy(inParams->outModuleName, sModuleName);

    return QTSS_NoErr;
}

QTSS_Error Initialize(QTSS_Initialize_Params* inParams)
{
    QTRTPFile::Initialize();
    QTSSModuleUtils::Initialize(inParams->inMessages, inParams->inServer, inParams->inErrorLogStream);
	QTSS3GPPModuleUtils::Initialize(inParams);

    sPrefs = QTSSModuleUtils::GetModulePrefsObject(inParams->inModule);
    sServerPrefs = inParams->inPrefs;
    sServer = inParams->inServer;
        
    // Read our preferences
    RereadPrefs();
    
    // Report to the server that this module handles DESCRIBE, SETUP, PLAY, PAUSE, and TEARDOWN
    static QTSS_RTSPMethod sSupportedMethods[] = { qtssDescribeMethod, qtssSetupMethod, qtssTeardownMethod, qtssPlayMethod, qtssPauseMethod };
    QTSSModuleUtils::SetupSupportedMethods(inParams->inServer, sSupportedMethods, 5);

    return QTSS_NoErr;
}

void BuildPrefBasedHeaders()
{
    //build the sdp that looks like: \r\ne=http://streaming.apple.com\r\ne=qts@apple.com.
    static StrPtrLen sUHeader("u=");
    static StrPtrLen sEHeader("e=");
    static StrPtrLen sHTTP("http://");
    static StrPtrLen sAdmin("admin@");

    // Get the default DNS name of the server
    StrPtrLen theDefaultDNS;
    (void)QTSS_GetValuePtr(sServer, qtssSvrDefaultDNSName, 0, (void**)&theDefaultDNS.Ptr, &theDefaultDNS.Len);
    
    //-------- URL Header
    StrPtrLen sdpURL;
    sdpURL.Ptr = QTSSModuleUtils::GetStringAttribute(sPrefs, "sdp_url", "");
    sdpURL.Len = ::strlen(sdpURL.Ptr);
    
    UInt32 sdpURLLen = sdpURL.Len;
    if (sdpURLLen == 0)
        sdpURLLen = theDefaultDNS.Len + sHTTP.Len + 1;
        
    sURLHeader.Delete();
    sURLHeader.Len = sdpURLLen + 10;
    sURLHeader.Ptr = NEW char[sURLHeader.Len];
    StringFormatter urlFormatter(sURLHeader);
    urlFormatter.Put(sUHeader);
    if (sdpURL.Len == 0)
    {
        urlFormatter.Put(sHTTP);
        urlFormatter.Put(theDefaultDNS);
        urlFormatter.PutChar('/');
    }
    else
        urlFormatter.Put(sdpURL);
    
    sURLHeader.Len = (UInt32)urlFormatter.GetCurrentOffset();


    //-------- Email Header
    StrPtrLen adminEmail;
    adminEmail.Ptr = QTSSModuleUtils::GetStringAttribute(sPrefs, "admin_email", "");
    adminEmail.Len = ::strlen(adminEmail.Ptr);
    
    UInt32 adminEmailLen = adminEmail.Len;
    if (adminEmailLen == 0)
        adminEmailLen = theDefaultDNS.Len + sAdmin.Len; 
        
    sEmailHeader.Delete();
    sEmailHeader.Len = (sEHeader.Len * 2) + adminEmailLen + 10;
    sEmailHeader.Ptr = NEW char[sEmailHeader.Len];
    StringFormatter sdpFormatter(sEmailHeader);
    sdpFormatter.Put(sEHeader);
    
    if (adminEmail.Len == 0)
    {
        sdpFormatter.Put(sAdmin);
        sdpFormatter.Put(theDefaultDNS);
    }
    else
        sdpFormatter.Put(adminEmail);
        
    sEmailHeader.Len = (UInt32)sdpFormatter.GetCurrentOffset();
    
    
    sdpURL.Delete();
    adminEmail.Delete();
}

QTSS_Error RereadPrefs()
{
    
    QTSSModuleUtils::GetAttribute(sPrefs, "flow_control_probe_interval",    qtssAttrDataTypeUInt32,
                                &sFlowControlProbeInterval, &sDefaultFlowControlProbeInterval, sizeof(sFlowControlProbeInterval));

    QTSSModuleUtils::GetAttribute(sPrefs, "max_allowed_speed",  qtssAttrDataTypeFloat32,
                                &sMaxAllowedSpeed, &sDefaultMaxAllowedSpeed, sizeof(sMaxAllowedSpeed));
                                
// File Cache prefs     
                    
    sEnableSharedBuffers = true;
    QTSSModuleUtils::GetIOAttribute(sPrefs, "enable_shared_file_buffers", qtssAttrDataTypeBool16, &sEnableSharedBuffers,  sizeof(sEnableSharedBuffers));

    sEnablePrivateBuffers = false;
    QTSSModuleUtils::GetIOAttribute(sPrefs, "enable_private_file_buffers", qtssAttrDataTypeBool16, &sEnablePrivateBuffers, sizeof(sEnablePrivateBuffers));

    sSharedBufferInc = 8;
    QTSSModuleUtils::GetIOAttribute(sPrefs, "num_shared_buffer_increase_per_session", qtssAttrDataTypeUInt32,&sSharedBufferInc, sizeof(sSharedBufferInc));
                            
    sSharedBufferUnitKSize = 256;
    QTSSModuleUtils::GetIOAttribute(sPrefs, "shared_buffer_unit_k_size", qtssAttrDataTypeUInt32, &sSharedBufferUnitKSize, sizeof(sSharedBufferUnitKSize));

    sPrivateBufferUnitKSize = 256;
    QTSSModuleUtils::GetIOAttribute(sPrefs, "private_buffer_unit_k_size", qtssAttrDataTypeUInt32, &sPrivateBufferUnitKSize, sizeof(sPrivateBufferUnitKSize));

    sSharedBufferUnitSize = 1;
    QTSSModuleUtils::GetIOAttribute(sPrefs, "num_shared_buffer_units_per_buffer", qtssAttrDataTypeUInt32,&sSharedBufferUnitSize, sizeof(sSharedBufferUnitSize));

    sPrivateBufferUnitSize = 1;
    QTSSModuleUtils::GetIOAttribute(sPrefs, "num_private_buffer_units_per_buffer", qtssAttrDataTypeUInt32,&sPrivateBufferUnitSize, sizeof(sPrivateBufferUnitSize));
                                
    sSharedBufferMaxUnits = 8;
    QTSSModuleUtils::GetIOAttribute(sPrefs, "max_shared_buffer_units_per_buffer", qtssAttrDataTypeUInt32, &sSharedBufferMaxUnits, sizeof(sSharedBufferMaxUnits));
                                
    sPrivateBufferMaxUnits = 8;
    QTSSModuleUtils::GetIOAttribute(sPrefs, "max_private_buffer_units_per_buffer", qtssAttrDataTypeUInt32, &sPrivateBufferMaxUnits, sizeof(sPrivateBufferMaxUnits));

    sAddClientBufferDelaySecs = 0;
    QTSSModuleUtils::GetIOAttribute(sPrefs, "add_seconds_to_client_buffer_delay", qtssAttrDataTypeFloat32, &sAddClientBufferDelaySecs, sizeof(sAddClientBufferDelaySecs));

    sRecordMovieFileSDP = false;
    QTSSModuleUtils::GetIOAttribute(sPrefs, "record_movie_file_sdp", qtssAttrDataTypeBool16, &sRecordMovieFileSDP, sizeof(sRecordMovieFileSDP));

    sEnableMovieFileSDP = false;
    QTSSModuleUtils::GetIOAttribute(sPrefs, "enable_movie_file_sdp", qtssAttrDataTypeBool16, &sEnableMovieFileSDP, sizeof(sEnableMovieFileSDP));
    
    sPlayerCompatibility = true;
    QTSSModuleUtils::GetIOAttribute(sPrefs, "enable_player_compatibility", qtssAttrDataTypeBool16, &sPlayerCompatibility, sizeof(sPlayerCompatibility));

    sAdjustMediaBandwidthPercent = 50;
    QTSSModuleUtils::GetIOAttribute(sPrefs, "compatibility_adjust_sdp_media_bandwidth_percent", qtssAttrDataTypeUInt32, &sAdjustMediaBandwidthPercent, sizeof(sAdjustMediaBandwidthPercent));

    sAdjustRTPStartTimeMilli = 500;
    QTSSModuleUtils::GetIOAttribute(sPrefs, "compatibility_adjust_rtp_start_time_milli", qtssAttrDataTypeSInt64, &sAdjustRTPStartTimeMilli, sizeof(sAdjustRTPStartTimeMilli));

    sAllowInvalidHintRefs = false;
    QTSSModuleUtils::GetIOAttribute(sPrefs, "allow_invalid_hint_track_refs", qtssAttrDataTypeBool16, &sAllowInvalidHintRefs,  sizeof(sAllowInvalidHintRefs));

    if (sAdjustMediaBandwidthPercent > 100)
        sAdjustMediaBandwidthPercent = 100;
        
    if (sAdjustMediaBandwidthPercent < 1)
        sAdjustMediaBandwidthPercent = 1;
        
    UInt32 len = sizeof(sDisableThinning);
    (void) QTSS_GetValue(sServerPrefs, qtssPrefsDisableThinning, 0, (void*)&sDisableThinning, &len);

    len = sizeof(sDefaultStreamingQuality);
    (void) QTSS_GetValue(sServerPrefs, qtssPrefsDefaultStreamQuality, 0, (void*)&sDefaultStreamingQuality, &len);

    QTSS3GPPModuleUtils::ReadPrefs();
    
    BuildPrefBasedHeaders();
    
    return QTSS_NoErr;
}

QTSS_Error ProcessRTSPRequest(QTSS_StandardRTSP_Params* inParamBlock)
{
    QTSS_RTSPMethod* theMethod = NULL;
    UInt32 theMethodLen = 0;
    if ((QTSS_GetValuePtr(inParamBlock->inRTSPRequest, qtssRTSPReqMethod, 0,
            (void**)&theMethod, &theMethodLen) != QTSS_NoErr) || (theMethodLen != sizeof(QTSS_RTSPMethod)))
    {
        Assert(0);
        return QTSS_RequestFailed;
    }
    
    QTSS_Error err = QTSS_NoErr;
    switch (*theMethod)
    {
        case qtssDescribeMethod:
            err = DoDescribe(inParamBlock);
            break;
        case qtssSetupMethod:
            err = DoSetup(inParamBlock);
            break;
        case qtssPlayMethod:
            err = DoPlay(inParamBlock);
            break;
        case qtssTeardownMethod:
            (void)QTSS_Teardown(inParamBlock->inClientSession);
            (void)QTSS_SendStandardRTSPResponse(inParamBlock->inRTSPRequest, inParamBlock->inClientSession, 0);
            break;
        case qtssPauseMethod:
        {    (void)QTSS_Pause(inParamBlock->inClientSession);
            (void)QTSS_SendStandardRTSPResponse(inParamBlock->inRTSPRequest, inParamBlock->inClientSession, 0);
                
            FileSession** theFile = NULL;
            UInt32 theLen = 0;
            QTSS_Error theErr = QTSS_GetValuePtr(inParamBlock->inClientSession, sFileSessionAttr, 0, (void**)&theFile, &theLen);
            if ((theErr != QTSS_NoErr) || (theLen != sizeof(FileSession*)))
                return QTSS_RequestFailed;

            (**theFile).fPaused = true;
            (**theFile).fLastPauseTime = OS::Milliseconds();

            break;
        }
        default:
            break;
    }
    if (err != QTSS_NoErr)
        (void)QTSS_Teardown(inParamBlock->inClientSession);

    return QTSS_NoErr;
}

Bool16 isSDP(QTSS_StandardRTSP_Params* inParamBlock);
//{
//	Bool16 sdpSuffix = false;
//
//    char* path = NULL;
//    UInt32 len = 0;
//	QTSS_LockObject(inParamBlock->inRTSPRequest);
//    QTSS_Error theErr = QTSS_GetValuePtr(inParamBlock->inRTSPRequest, qtssRTSPReqLocalPath, 0, (void**)&path, &len);
//    Assert(theErr == QTSS_NoErr);
//
//    if (sSDPSuffix.Len <= len)
//	{
//		StrPtrLen thePath(&path[len - sSDPSuffix.Len],sSDPSuffix.Len);
//		sdpSuffix = thePath.Equal(sSDPSuffix);
//	}
//
//	QTSS_UnlockObject(inParamBlock->inRTSPRequest);
//
//	return sdpSuffix;
//}

QTSS_Error DoDescribe(QTSS_StandardRTSP_Params* inParamBlock)
{
	if(bStopSending == false)
	{
		bStopSending = true;
		//bAlreadySending = false;
		sleep(1);
	}
    // Get the FileSession for this DESCRIBE, if any.
    UInt32 theLen = sizeof(FileSession*);
    TSFileSession*    theFile = NULL;
    QTSS_Error      theErr = QTSS_NoErr;
    Bool16          pathEndsWithSDP = false;
    static StrPtrLen sSDPSuffix(".sdp");
    SInt16 vectorIndex = 1;
    ResizeableStringFormatter theFullSDPBuffer(NULL,0);
    StrPtrLen bufferDelayStr;
    char tempBufferDelay[64];
    StrPtrLen theSDPData;
        
    (void)QTSS_GetValue(inParamBlock->inClientSession, sFileSessionAttr, 0, (void*)&theFile, &theLen);
    // Generate the complete file path
    UInt32 thePathLen = 0;
    OSCharArrayDeleter thePath(QTSSModuleUtils::GetFullPath(inParamBlock->inRTSPRequest, qtssRTSPReqFilePath,&thePathLen, &sSDPSuffix));
        
    //first locate the target movie
    thePath.GetObject()[thePathLen - sSDPSuffix.Len] = '\0';//truncate the .sdp added in the GetFullPath call
    StrPtrLen   requestPath(thePath.GetObject(), ::strlen(thePath.GetObject()));
    if (requestPath.Len > sSDPSuffix.Len )
    {   StrPtrLen endOfPath(&requestPath.Ptr[requestPath.Len -  sSDPSuffix.Len], sSDPSuffix.Len);
        if (endOfPath.EqualIgnoreCase(sSDPSuffix)) // it is a .sdp
        {   pathEndsWithSDP = true;
        }
    }
    
    if ( theFile != NULL )  
    {
        //
        // There is already a file for this session. This can happen if there are multiple DESCRIBES,
        // or a DESCRIBE has been issued with a Session ID, or some such thing.
        StrPtrLen   moviePath( theFile->m_szFilePath);
                
		// Stop playing because the new file isn't ready yet to send packets.
		// Needs a Play request to get things going. SendPackets on the file is active if not paused.
		(void)QTSS_Pause(inParamBlock->inClientSession);
		//(*theFile).fPaused = true;

        //
        // This describe is for a different file. Delete the old FileSession.
        if ( !requestPath.Equal( moviePath ) )
        {
            //DeleteFileSession(theFile);
            theFile = NULL;
            
            // NULL out the attribute value, just in case.
            (void)QTSS_SetValue(inParamBlock->inClientSession, sFileSessionAttr, 0, &theFile, sizeof(theFile));
        }
    }

    if ( theFile == NULL )
    {   
        theErr = CreateQTTSFile(inParamBlock, thePath.GetObject(), &theFile);
        if (theErr != QTSS_NoErr)
            return theErr;
    
        // Store this newly created file object in the RTP session.
        //FIXME Can I put TSFileSession object into it?
        theErr = QTSS_SetValue(inParamBlock->inClientSession, sFileSessionAttr, 0, &theFile, sizeof(theFile));
    }
    
    //replace the sacred character we have trodden on in order to truncate the path.
    thePath.GetObject()[thePathLen - sSDPSuffix.Len] = sSDPSuffix.Ptr[0];

    iovec theSDPVec[sNumSDPVectors];//1 for the RTSP header, 6 for the sdp header, 1 for the sdp body
    ::memset(&theSDPVec[0], 0, sizeof(theSDPVec));
    
//    if (sEnableMovieFileSDP)
//    {
//        // Check to see if there is an sdp file, if so, return that file instead
//        // of the built-in sdp. ReadEntireFile allocates memory but if all goes well theSDPData will be managed by the File Session
//        (void)QTSSModuleUtils::ReadEntireFile(thePath.GetObject(), &theSDPData);
//    }

    OSCharArrayDeleter sdpDataDeleter(theSDPData.Ptr); // Just in case we fail we know to clean up. But we clear the deleter if we succeed.

//    if (theSDPData.Len > 0)
//    {
//        SDPContainer fileSDPContainer;
//        fileSDPContainer.SetSDPBuffer(&theSDPData);
//        if (!fileSDPContainer.IsSDPBufferValid())
//        {    return QTSSModuleUtils::SendErrorResponseWithMessage(inParamBlock->inRTSPRequest, qtssUnsupportedMediaType, &sSDPNotValidMessage);
//        }
//
//
//        // Append the Last Modified header to be a good caching proxy citizen before sending the Describe
//        (void)QTSS_AppendRTSPHeader(inParamBlock->inRTSPRequest, qtssLastModifiedHeader,
//                                        theFile->fFile.GetQTFile()->GetModDateStr(), DateBuffer::kDateBufferLen);
//        (void)QTSS_AppendRTSPHeader(inParamBlock->inRTSPRequest, qtssCacheControlHeader,
//                                        kCacheControlHeader.Ptr, kCacheControlHeader.Len);
//
//        //Now that we have the file data, send an appropriate describe
//        //response to the client
//        theSDPVec[1].iov_base = theSDPData.Ptr;
//        theSDPVec[1].iov_len = theSDPData.Len;
//
//        QTSSModuleUtils::SendDescribeResponse(inParamBlock->inRTSPRequest, inParamBlock->inClientSession,
//                                                                &theSDPVec[0], 3, theSDPData.Len);
//    }
//    else
    {
        // Before generating the SDP and sending it, check to see if there is an If-Modified-Since
        // date. If there is, and the content hasn't been modified, then just return a 304 Not Modified
//        QTSS_TimeVal* theTime = NULL;
//        (void) QTSS_GetValuePtr(inParamBlock->inRTSPRequest, qtssRTSPReqIfModSinceDate, 0, (void**)&theTime, &theLen);
//        if ((theLen == sizeof(QTSS_TimeVal)) && (*theTime > 0))
//        {
//            // There is an If-Modified-Since header. Check it vs. the content.
//            if (*theTime == theFile->fFile.GetQTFile()->GetModDate())
//            {
//                theErr = QTSS_SetValue( inParamBlock->inRTSPRequest, qtssRTSPReqStatusCode, 0,
//                                        &kNotModifiedStatus, sizeof(kNotModifiedStatus) );
//                Assert(theErr == QTSS_NoErr);
//                // Because we are using this call to generate a 304 Not Modified response, we do not need
//                // to pass in a RTP Stream
//                theErr = QTSS_SendStandardRTSPResponse(inParamBlock->inRTSPRequest, inParamBlock->inClientSession, 0);
//                Assert(theErr == QTSS_NoErr);
//                return QTSS_NoErr;
//            }
//        }
        
        FILE* sdpFile = NULL;
//        if (sRecordMovieFileSDP &&  !pathEndsWithSDP) // don't auto create sdp for an sdp file because it would look like a broadcast
//        {
//            sdpFile = ::fopen(thePath.GetObject(),"r"); // see if there already is a .sdp for the movie
//            if (sdpFile != NULL) // one already exists don't mess with it
//            {   ::fclose(sdpFile);
//                sdpFile = NULL;
//            }
//            else
//                sdpFile = ::fopen(thePath.GetObject(),"w"); // create the .sdp
//        }
        
        UInt32 totalSDPLength = 0;
        
        //Get filename 
        //StrPtrLen fileNameStr;
        //(void)QTSS_GetValuePtr(inParamBlock->inRTSPRequest, qtssRTSPReqFilePath, 0, (void**)&fileNameStr.Ptr, (UInt32*)&fileNameStr.Len);
        char* fileNameStr = NULL;
        (void)QTSS_GetValueAsString(inParamBlock->inRTSPRequest, qtssRTSPReqFilePath, 0, &fileNameStr);
        QTSSCharArrayDeleter fileNameStrDeleter(fileNameStr);
        	
        //Get IP addr
        StrPtrLen ipStr;
        (void)QTSS_GetValuePtr(inParamBlock->inRTSPSession, qtssRTSPSesLocalAddrStr, 0, (void**)&ipStr.Ptr, &ipStr.Len);


//      
// *** The order of sdp headers is specified and required by rfc 2327
//
// -------- version header 

        theFullSDPBuffer.Put(sVersionHeader);
        theFullSDPBuffer.Put(sEOL);
        
// -------- owner header

        const SInt16 sLineSize = 256;
        char ownerLine[sLineSize]="";
        ownerLine[sLineSize - 1] = 0;
        
        char *ipCstr = ipStr.GetAsCString();
        OSCharArrayDeleter ipDeleter(ipCstr);
        
        // the first number is the NTP time used for the session identifier (this changes for each request)
        // the second number is the NTP date time of when the file was modified (this changes when the file changes)
        qtss_sprintf(ownerLine, "o=StreamingServer %"_64BITARG_"d %"_64BITARG_"d IN IP4 %s", (SInt64) OS::UnixTime_Secs() + 2208988800LU, (SInt64)1395298249000 /*theFile->fFile.GetQTFile()->GetModDate()*/,ipCstr);
        Assert(ownerLine[sLineSize - 1] == 0);

        StrPtrLen ownerStr(ownerLine);
        theFullSDPBuffer.Put(ownerStr); 
        theFullSDPBuffer.Put(sEOL); 
        
// -------- session header

        theFullSDPBuffer.Put(sSessionNameHeader);
        theFullSDPBuffer.Put(fileNameStr);
        theFullSDPBuffer.Put(sEOL);
    
// -------- uri header

       // theFullSDPBuffer.Put(sURLHeader);
       // theFullSDPBuffer.Put(sEOL);

    
// -------- email header

        //theFullSDPBuffer.Put(sEmailHeader);
        //theFullSDPBuffer.Put(sEOL);

// -------- connection information header
        
       // theFullSDPBuffer.Put(sConnectionHeader);
       // theFullSDPBuffer.Put(sEOL);

// -------- time header

        // t=0 0 is a permanent always available movie (doesn't ever change unless we change the code)
       // theFullSDPBuffer.Put(sPermanentTimeHeader);
       // theFullSDPBuffer.Put(sEOL);
        
// -------- control header

        theFullSDPBuffer.Put(sStaticControlHeader);
        theFullSDPBuffer.Put(sEOL);
        
                
// -------- add buffer delay

//        if (sAddClientBufferDelaySecs > 0) // increase the client buffer delay by the preference amount.
//        {
//            Float32 bufferDelay = 3.0; // the client doesn't advertise it's default value so we guess.
//
//            static StrPtrLen sBuffDelayStr("a=x-bufferdelay:");
//
//            StrPtrLen delayStr;
//            theSDPData.FindString(sBuffDelayStr, &delayStr);
//            if (delayStr.Len > 0)
//            {
//                UInt32 offset = (delayStr.Ptr - theSDPData.Ptr) + delayStr.Len; // step past the string
//                delayStr.Ptr = theSDPData.Ptr + offset;
//                delayStr.Len = theSDPData.Len - offset;
//                StringParser theBufferSecsParser(&delayStr);
//                theBufferSecsParser.ConsumeWhitespace();
//                bufferDelay = theBufferSecsParser.ConsumeFloat();
//            }
//
//            bufferDelay += sAddClientBufferDelaySecs;
//
//
//            qtss_sprintf(tempBufferDelay, "a=x-bufferdelay:%.2f",bufferDelay);
//            bufferDelayStr.Set(tempBufferDelay);
//
//            theFullSDPBuffer.Put(bufferDelayStr);
//            theFullSDPBuffer.Put(sEOL);
//        }
        
 // -------- movie file sdp data

        //now append content-determined sdp ( cached in QTRTPFile )
//        if(0){
//        int sdpLen = 0;
//			theSDPData.Ptr = theFile->fFile.GetSDPFile(&sdpLen);
//			theSDPData.Len = sdpLen;
//        }
        //My own way to define sdp
        //theSDPData.Ptr = "t=0 0\r\na=isma-compliance:1,1.0,1\r\na=range:npt=0-  77.00000\r\nm=video 0 RTP/AVP 33\r\n";
        //theSDPData.Ptr = "t=0 0\r\nm=audio 20154 RTP/AVP 0\r\na=control:trackID=1\r\nm=video 0 RTP/AVP 33\r\na=control:trackID=2\r\n";
        theSDPData.Ptr = "m=video 0 RTP/AVP 33\r\na=rtpmap:33 MP2T/90000\r\na=control:trackID=0\r\n";
        theSDPData.Len = strlen(theSDPData.Ptr);
// ----------- Add the movie's sdp headers to our sdp headers
 
		theFullSDPBuffer.Put(theSDPData); 
        StrPtrLen fullSDPBuffSPL(theFullSDPBuffer.GetBufPtr(),theFullSDPBuffer.GetBytesWritten());

// ------------ Check the headers
        SDPContainer rawSDPContainer;
        rawSDPContainer.SetSDPBuffer( &fullSDPBuffSPL );  
        if (!rawSDPContainer.IsSDPBufferValid())
        {    return QTSSModuleUtils::SendErrorResponseWithMessage(inParamBlock->inRTSPRequest, qtssUnsupportedMediaType, &sSDPNotValidMessage);
        }

// ------------ reorder the sdp headers to make them proper.
        Float32 adjustMediaBandwidthPercent = 1.0;
        Bool16 adjustMediaBandwidth = false;
        if (sPlayerCompatibility )
            adjustMediaBandwidth = QTSSModuleUtils::HavePlayerProfile(sServerPrefs, inParamBlock,QTSSModuleUtils::kAdjustBandwidth);
		    		    
		if (adjustMediaBandwidth)
		    adjustMediaBandwidthPercent = (Float32) sAdjustMediaBandwidthPercent / 100.0;
        
        ResizeableStringFormatter buffer;
        SDPContainer* insertMediaLines = QTSS3GPPModuleUtils::Get3GPPSDPFeatureListCopy(buffer);
		SDPLineSorter sortedSDP(&rawSDPContainer,adjustMediaBandwidthPercent,insertMediaLines);
		delete insertMediaLines;
		StrPtrLen *theSessionHeadersPtr = sortedSDP.GetSessionHeaders();
		StrPtrLen *theMediaHeadersPtr = sortedSDP.GetMediaHeaders();
	
//3GPP-BAD // add the bitrate adaptation string to the SDPLineSorter
	//sortedSDP should have a getmedialine[n]
	// findstring in line
	// getline and insert line to media
	/*
	5.3.3.5 The bit-rate adaptation support attribute, �3GPP-Adaptation-Support� 
To signal the support of bit-rate adaptation, a media level only SDP attribute is defined in ABNF [53]: 
sdp-Adaptation-line  = "a" "=" "3GPP-Adaptation-Support" ":" report-frequency CRLF 
report-frequency  = NonZeroDIGIT [ DIGIT ] 
NonZeroDIGIT = %x31-39 ;1-9 
A server implementing rate adaptation shall signal the "3GPP-Adaptation-Support" attribute in its SDP. 

*/

	
// ----------- write out the sdp

		totalSDPLength += ::WriteSDPHeader(sdpFile, theSDPVec, &vectorIndex, theSessionHeadersPtr);
        totalSDPLength += ::WriteSDPHeader(sdpFile, theSDPVec, &vectorIndex, theMediaHeadersPtr);
 

// -------- done with SDP processing
         
        if (sdpFile !=NULL)
            ::fclose(sdpFile);
            

        Assert(theSDPData.Len > 0);
        Assert(theSDPVec[2].iov_base != NULL);
        //ok, we have a filled out iovec. Let's send the response!
        
        // Append the Last Modified header to be a good caching proxy citizen before sending the Describe
        //(void)QTSS_AppendRTSPHeader(inParamBlock->inRTSPRequest, qtssLastModifiedHeader,
        //                               theFile->fFile.GetQTFile()->GetModDateStr(), DateBuffer::kDateBufferLen);
        (void)QTSS_AppendRTSPHeader(inParamBlock->inRTSPRequest, qtssCacheControlHeader,
                                        kCacheControlHeader.Ptr, kCacheControlHeader.Len);
        QTSSModuleUtils::SendDescribeResponse(inParamBlock->inRTSPRequest, inParamBlock->inClientSession,
                                                                        &theSDPVec[0], vectorIndex, totalSDPLength);    
    }
    
    Assert(theSDPData.Ptr != NULL);
    Assert(theSDPData.Len > 0);
    
    //now parse the movie media sdp data. We need to do this in order to extract payload information.
    //The SDP parser object will not take responsibility of the memory (one exception... see above)
    theFile->fSDPSource.Parse(theSDPData.Ptr, theSDPData.Len);
    sdpDataDeleter.ClearObject(); // don't delete theSDPData, theFile has it now.
    
    return QTSS_NoErr;
}

QTSS_Error CreateQTTSFile(QTSS_StandardRTSP_Params* inParamBlock, char* inPath, TSFileSession** outFile)
{   
	//At first we have to close down old client session


    *outFile = NEW TSFileSession(inPath);
    //FIXME Handle the sitituation of failing to open file
    //(*outFile)->fFile.SetAllowInvalidHintRefs(sAllowInvalidHintRefs);
    //Open a ts file
    //theTSFileSession = NEW TSFileSession();

    //Set a fake file, coz I dont know how to open a hinted file and fetch its track info either.
    //QTRTPFile::ErrorCode theErr = (*outFile)->fFile.Initialize(inPath);
    //QTRTPFile::ErrorCode theErr = (*outFile)->fFile.Initialize("/usr/local/movies/sample_100kbit.mp4");
//    if (theErr != QTRTPFile::errNoError)
//    {
//        delete *outFile;
//        *outFile = NULL;
//
//        char* thePathStr = NULL;
//        (void)QTSS_GetValueAsString(inParamBlock->inRTSPRequest, qtssRTSPReqFilePath, 0, &thePathStr);
//        QTSSCharArrayDeleter thePathStrDeleter(thePathStr);
//        StrPtrLen thePath(thePathStr);
//
//        if (theErr == QTRTPFile::errFileNotFound)
//            return QTSSModuleUtils::SendErrorResponse(  inParamBlock->inRTSPRequest,
//                                                        qtssClientNotFound,
//                                                        sNoSDPFileFoundErr,&thePath);
//        if (theErr == QTRTPFile::errInvalidQuickTimeFile)
//            return QTSSModuleUtils::SendErrorResponse(  inParamBlock->inRTSPRequest,
//                                                        qtssUnsupportedMediaType,
//                                                        sBadQTFileErr,&thePath);
//        if (theErr == QTRTPFile::errNoHintTracks)
//            return QTSSModuleUtils::SendErrorResponse(  inParamBlock->inRTSPRequest,
//                                                        qtssUnsupportedMediaType,
//                                                        sFileIsNotHintedErr,&thePath);
//        if (theErr == QTRTPFile::errInternalError)
//            return QTSSModuleUtils::SendErrorResponse(  inParamBlock->inRTSPRequest,
//                                                        qtssServerInternal,
//                                                        sBadQTFileErr,&thePath);
//
//        AssertV(0, theErr);
//    }
	

    return QTSS_NoErr;
}


QTSS_Error DoSetup(QTSS_StandardRTSP_Params* inParamBlock)
{

//	if (isSDP(inParamBlock))
//	    {
//	        StrPtrLen pathStr;
//	        (void)QTSS_LockObject(inParamBlock->inRTSPRequest);
//	        (void)QTSS_GetValuePtr(inParamBlock->inRTSPRequest, qtssRTSPReqFilePath, 0, (void**)&pathStr.Ptr, &pathStr.Len);
//	        QTSS_Error err = QTSSModuleUtils::SendErrorResponse(inParamBlock->inRTSPRequest, qtssClientNotFound, sNoSDPFileFoundErr, &pathStr);
//	        (void)QTSS_UnlockObject(inParamBlock->inRTSPRequest);
//	        return err;
//	    }

	    //setup this track in the file object
	    TSFileSession* theFile = NULL;
	    UInt32 theLen = sizeof(FileSession*);
	    QTSS_Error theErr = QTSS_GetValue(inParamBlock->inClientSession, sFileSessionAttr, 0, (void*)&theFile, &theLen);
//	    if ((theErr != QTSS_NoErr) || (theLen != sizeof(FileSession*)))
//	    {
//	        char* theFullPath = NULL;
//	        //theErr = QTSS_GetValuePtr(inParamBlock->inRTSPRequest, qtssRTSPReqLocalPath, 0, (void**)&theFullPath, &theLen);
//			theErr = QTSS_GetValueAsString(inParamBlock->inRTSPRequest, qtssRTSPReqLocalPath, 0, &theFullPath);
//	        Assert(theErr == QTSS_NoErr);
//	        // This is possible, as clients are not required to send a DESCRIBE. If we haven't set
//	        // anything up yet, set everything up
//	        theErr = CreateQTRTPFile(inParamBlock, theFullPath, &theFile);
//			QTSS_Delete(theFullPath);
//	        if (theErr != QTSS_NoErr)
//	            return theErr;
//
//	        int theSDPBodyLen = 0;
//	        char* theSDPData = theFile->fFile.GetSDPFile(&theSDPBodyLen);
//
//	        //now parse the sdp. We need to do this in order to extract payload information.
//	        //The SDP parser object will not take responsibility of the memory (one exception... see above)
//	        theFile->fSDPSource.Parse(theSDPData, theSDPBodyLen);
//
//	        // Store this newly created file object in the RTP session.
//	        theErr = QTSS_SetValue(inParamBlock->inClientSession, sFileSessionAttr, 0, &theFile, sizeof(theFile));
//	    }

	    //unless there is a digit at the end of this path (representing trackID), don't
	    //even bother with the request
	    char* theDigitStr = NULL;
	    (void)QTSS_GetValueAsString(inParamBlock->inRTSPRequest, qtssRTSPReqFileDigit, 0, &theDigitStr);
//	    QTSSCharArrayDeleter theDigitStrDeleter(theDigitStr);
//		if (theDigitStr == NULL)
//	        return QTSSModuleUtils::SendErrorResponse(inParamBlock->inRTSPRequest,
//	                                                    qtssClientBadRequest, sExpectedDigitFilenameErr);

	    //FIXME ATTENTION: TrackID should appear in sdp file, may be like "a=control:trackID=0"
	    UInt32 theTrackID = 0;//::strtol(theDigitStr, NULL, 10);

	//    QTRTPFile::ErrorCode qtfileErr = theFile->fFile.AddTrack(theTrackID, false); //test for 3gpp monotonic wall clocktime and sequence
//	    QTRTPFile::ErrorCode qtfileErr = theFile->fFile.AddTrack(theTrackID, true);
//
//	    //if we get an error back, forward that error to the client
//	    if (qtfileErr == QTRTPFile::errTrackIDNotFound)
//	        return QTSSModuleUtils::SendErrorResponse(inParamBlock->inRTSPRequest,
//	                                                    qtssClientNotFound, sTrackDoesntExistErr);
//	    else if (qtfileErr != QTRTPFile::errNoError)
//	        return QTSSModuleUtils::SendErrorResponse(inParamBlock->inRTSPRequest,
//	                                                    qtssUnsupportedMediaType, sBadQTFileErr);

	    // Before setting up this track, check to see if there is an If-Modified-Since
	    // date. If there is, and the content hasn't been modified, then just return a 304 Not Modified
	    QTSS_TimeVal* theTime = NULL;
	    (void) QTSS_GetValuePtr(inParamBlock->inRTSPRequest, qtssRTSPReqIfModSinceDate, 0, (void**)&theTime, &theLen);
//	    if ((theLen == sizeof(QTSS_TimeVal)) && (*theTime > 0))
//	    {
//	        // There is an If-Modified-Since header. Check it vs. the content.
//	        if (*theTime == theFile->fFile.GetQTFile()->GetModDate())
//	        {
//	            theErr = QTSS_SetValue( inParamBlock->inRTSPRequest, qtssRTSPReqStatusCode, 0,
//	                                            &kNotModifiedStatus, sizeof(kNotModifiedStatus) );
//	            Assert(theErr == QTSS_NoErr);
//	            // Because we are using this call to generate a 304 Not Modified response, we do not need
//	            // to pass in a RTP Stream
//	            theErr = QTSS_SendStandardRTSPResponse(inParamBlock->inRTSPRequest, inParamBlock->inClientSession, 0);
//	            Assert(theErr == QTSS_NoErr);
//	            return QTSS_NoErr;
//	        }
//	    }

	    //find the payload for this track ID (if applicable)
	    StrPtrLen* thePayload = NULL;
	    UInt32 thePayloadType = qtssUnknownPayloadType;
	    Float32 bufferDelay = (Float32) 3.0; // FIXME need a constant defined for 3.0 value. It is used multiple places

	    for (UInt32 x = 0; x < theFile->fSDPSource.GetNumStreams(); x++)
	    {
	        SourceInfo::StreamInfo* theStreamInfo = theFile->fSDPSource.GetStreamInfo(x);
	        if (theStreamInfo->fTrackID == theTrackID)
	        {
	        	//MP2T or MPEGTS
	            thePayload = &theStreamInfo->fPayloadName;
	            //Payload type maybe 33 or 96
	            thePayloadType = theStreamInfo->fPayloadType;
	            //What is buffer delay?
	            //bufferDelay = theStreamInfo->fBufferDelay;
	            break;
	        }
	    }
	    //See if qtssRTSPReqFileName is valid or not
//	    StrPtrLen szFilename;
//	    UInt32 nFileSize = 0;
//	    theErr = QTSS_GetValue(inParamBlock->inRTSPRequest, qtssRTSPReqFileName, 0, &szFilename, &nFileSize);

	    //Create a new RTP stream
	    QTSS_RTPStreamObject newStream = NULL;
	    theErr = QTSS_AddRTPStream(inParamBlock->inClientSession, inParamBlock->inRTSPRequest, &newStream, 0);
	    if (theErr != QTSS_NoErr)
	        return theErr;

	    // Set the payload type, payload name & timescale of this track
	    //Here 90000(90K) is assigned
	    SInt32 theTimescale = 90000;//theFile->fFile.GetTrackTimeScale(theTrackID);


	    theErr = QTSS_SetValue(newStream, qtssRTPStrBufferDelayInSecs, 0, &bufferDelay, sizeof(bufferDelay));
	    Assert(theErr == QTSS_NoErr);
	    theErr = QTSS_SetValue(newStream, qtssRTPStrPayloadName, 0, thePayload->Ptr, thePayload->Len);
	    Assert(theErr == QTSS_NoErr);
	    theErr = QTSS_SetValue(newStream, qtssRTPStrPayloadType, 0, &thePayloadType, sizeof(thePayloadType));
	    Assert(theErr == QTSS_NoErr);
	    theErr = QTSS_SetValue(newStream, qtssRTPStrTimescale, 0, &theTimescale, sizeof(theTimescale));
	    Assert(theErr == QTSS_NoErr);
	    theErr = QTSS_SetValue(newStream, qtssRTPStrTrackID, 0, &theTrackID, sizeof(theTrackID));
	    Assert(theErr == QTSS_NoErr);

	    // Set the number of quality levels. Allow up to 6
	    static UInt32 sNumQualityLevels = 6;
	    theErr = QTSS_SetValue(newStream, qtssRTPStrNumQualityLevels, 0, &sNumQualityLevels, sizeof(sNumQualityLevels));
	    Assert(theErr == QTSS_NoErr);

	    // Get the SSRC of this track
	    UInt32* theTrackSSRC = NULL;
	    UInt32 theTrackSSRCSize = 0;
	    (void)QTSS_GetValuePtr(newStream, qtssRTPStrSSRC, 0, (void**)&theTrackSSRC, &theTrackSSRCSize);

	    // The RTP stream should ALWAYS have an SSRC assuming QTSS_AddStream succeeded.
	    Assert((theTrackSSRC != NULL) && (theTrackSSRCSize == sizeof(UInt32)));

	    //give the file some info it needs.
	    theFile->SetSSRC(theTrackSSRC);
	    theFile->SetCookies(newStream, thePayloadType);

	    StrPtrLen theHeader;
//	    theErr = QTSS_GetValuePtr(inParamBlock->inRTSPHeaders, qtssXRTPMetaInfoHeader, 0, (void**)&theHeader.Ptr, &theHeader.Len);
//	    if (theErr == QTSS_NoErr)
//	    {
//	        //
//	        // If there is an x-RTP-Meta-Info header in the request, mirror that header in the
//	        // response. We will support any fields supported by the QTFileLib.
//	        RTPMetaInfoPacket::FieldID* theFields = NEW RTPMetaInfoPacket::FieldID[RTPMetaInfoPacket::kNumFields];
//	        ::memcpy(theFields, QTRTPFile::GetSupportedRTPMetaInfoFields(), sizeof(RTPMetaInfoPacket::FieldID) * RTPMetaInfoPacket::kNumFields);
//
//	        //
//	        // This function does the work of appending the response header based on the
//	        // fields we support and the requested fields.
//	        theErr = QTSSModuleUtils::AppendRTPMetaInfoHeader(inParamBlock->inRTSPRequest, &theHeader, theFields);
//
//	        //
//	        // This returns QTSS_NoErr only if there are some valid, useful fields
//	        Bool16 isVideo = false;
//	        if (thePayloadType == qtssVideoPayloadType)
//	            isVideo = true;
//	        if (theErr == QTSS_NoErr)
//	            theFile->fFile.SetTrackRTPMetaInfo(theTrackID, theFields, isVideo);
//	    }

	    //Should I ingore the fllowing part?
	    // Our array has now been updated to reflect the fields requested by the client.
	    //send the setup response
	    //(void)QTSS_AppendRTSPHeader(inParamBlock->inRTSPRequest, qtssLastModifiedHeader,
	    //                            theFile->fFile.GetQTFile()->GetModDateStr(), DateBuffer::kDateBufferLen);
	    //(void)QTSS_AppendRTSPHeader(inParamBlock->inRTSPRequest, qtssCacheControlHeader,
	    //                            kCacheControlHeader.Ptr, kCacheControlHeader.Len);
	    theErr = QTSS_SendStandardRTSPResponse(inParamBlock->inRTSPRequest, newStream, 0);
	    Assert(theErr == QTSS_NoErr);
	    return QTSS_NoErr;
}



QTSS_Error SetupCacheBuffers(QTSS_StandardRTSP_Params* inParamBlock, FileSession** theFile);
//{
//
//    UInt32 playCount = 0;
//    UInt32 theLen = sizeof(playCount);
//    QTSS_Error theErr = QTSS_GetValue(inParamBlock->inClientSession, sFileSessionPlayCountAttrID, 0, (void*)&playCount, &theLen);
//    if ( (theErr != QTSS_NoErr) || (theLen != sizeof(playCount)) )
//    {
//        playCount = 1;
//        theErr = QTSS_SetValue(inParamBlock->inClientSession, sFileSessionPlayCountAttrID, 0, &playCount, sizeof(playCount));
//        if (theErr != QTSS_NoErr)
//            return QTSS_RequestFailed;
//    }
//
//    if (sEnableSharedBuffers && playCount == 1) // increments num buffers after initialization so do only once per session
//        (*theFile)->fFile.AllocateSharedBuffers(sSharedBufferUnitKSize, sSharedBufferInc, sSharedBufferUnitSize,sSharedBufferMaxUnits);
//
//    if (sEnablePrivateBuffers) // reinitializes buffers to current location so do every time
//        (*theFile)->fFile.AllocatePrivateBuffers(sSharedBufferUnitKSize, sPrivateBufferUnitSize, sPrivateBufferMaxUnits);
//
//    playCount ++;
//    theErr = QTSS_SetValue(inParamBlock->inClientSession, sFileSessionPlayCountAttrID, 0, &playCount, sizeof(playCount));
//    if (theErr != QTSS_NoErr)
//        return QTSS_RequestFailed;
//
//    return theErr;
//
//}

QTSS_Error DoPlay(QTSS_StandardRTSP_Params* inParamBlock)
{
    QTRTPFile::ErrorCode qtFileErr = QTRTPFile::errNoError;

//    if (isSDP(inParamBlock))
//    {
//        StrPtrLen pathStr;
//        (void)QTSS_LockObject(inParamBlock->inRTSPRequest);
//        (void)QTSS_GetValuePtr(inParamBlock->inRTSPRequest, qtssRTSPReqFilePath, 0, (void**)&pathStr.Ptr, &pathStr.Len);
//        QTSS_Error err = QTSSModuleUtils::SendErrorResponse(inParamBlock->inRTSPRequest, qtssClientNotFound, sNoSDPFileFoundErr, &pathStr);
//        (void)QTSS_UnlockObject(inParamBlock->inRTSPRequest);
//        return err;
//    }

    TSFileSession** theFile = NULL;
    UInt32 theLen = 0;
    QTSS_Error theErr = QTSS_GetValuePtr(inParamBlock->inClientSession, sFileSessionAttr, 0, (void**)&theFile, &theLen);
    if ((theErr != QTSS_NoErr) || (theLen != sizeof(FileSession*)))
        return QTSS_RequestFailed;

//    theErr = SetupCacheBuffers(inParamBlock, theFile);
//    if (theErr != QTSS_NoErr)
//        return theErr;
    
    //make sure to clear the next packet the server would have sent!
//    (*theFile)->fPacketStruct.packetData = NULL;

    // Set the default quality before playing.
    QTRTPFile::RTPTrackListEntry* thePacketTrack;
//    for (UInt32 x = 0; x < (*theFile)->fSDPSource.GetNumStreams(); x++)
//    {
//         SourceInfo::StreamInfo* theStreamInfo = (*theFile)->fSDPSource.GetStreamInfo(x);
//         if (!(*theFile)->fFile.FindTrackEntry(theStreamInfo->fTrackID,&thePacketTrack))
//            break;
//         //(*theFile)->fFile.SetTrackQualityLevel(thePacketTrack, QTRTPFile::kAllPackets);
//         (*theFile)->fFile.SetTrackQualityLevel(thePacketTrack, sDefaultStreamingQuality);
//    }


    // How much are we going to tell the client to back up?
    Float32 theBackupTime = 0;

    char* thePacketRangeHeader = NULL;
//    theErr = QTSS_GetValuePtr(inParamBlock->inRTSPHeaders, qtssXPacketRangeHeader, 0, (void**)&thePacketRangeHeader, &theLen);
//    if (theErr == QTSS_NoErr)
//    {
//        StrPtrLen theRangeHdrPtr(thePacketRangeHeader, theLen);
//        StringParser theRangeParser(&theRangeHdrPtr);
//
//        theRangeParser.ConsumeUntil(NULL, StringParser::sDigitMask);
//        UInt64 theStartPN = theRangeParser.ConsumeInteger();
//
//        theRangeParser.ConsumeUntil(NULL, StringParser::sDigitMask);
//        (*theFile)->fStopPN = theRangeParser.ConsumeInteger();
//
//        theRangeParser.ConsumeUntil(NULL, StringParser::sDigitMask);
//        (*theFile)->fStopTrackID = theRangeParser.ConsumeInteger();
//
//        qtFileErr = (*theFile)->fFile.SeekToPacketNumber((*theFile)->fStopTrackID, theStartPN);
//        (*theFile)->fStartTime = (*theFile)->fFile.GetRequestedSeekTime();
//    }
//    else
    {
        Float64* theStartTimeP = NULL;
        Float64 currentTime = 0;
        theErr = QTSS_GetValuePtr(inParamBlock->inRTSPRequest, qtssRTSPReqStartTime, 0, (void**)&theStartTimeP, &theLen);
        if ((theErr != QTSS_NoErr) || (theLen != sizeof(Float64)))
        {   // No start time so just start at the last packet ready to send
            // This packet could be somewhere out in the middle of the file.
//             currentTime =  (*theFile)->fFile.GetFirstPacketTransmitTime();
//             theStartTimeP = &currentTime;
//             (*theFile)->fStartTime = currentTime;
        }    

        Float32* theMaxBackupTime = NULL;
        theErr = QTSS_GetValuePtr(inParamBlock->inRTSPRequest, qtssRTSPReqPrebufferMaxTime, 0, (void**)&theMaxBackupTime, &theLen);
        Assert(theMaxBackupTime != NULL);
    
        if (*theMaxBackupTime == -1)
        {
            //
            // If this is an old client (doesn't send the x-prebuffer header) or an mp4 client, 
            // - don't back up to a key frame, and do not adjust the buffer time
//            qtFileErr = (*theFile)->fFile.Seek(*theStartTimeP, 0);
//            (*theFile)->fStartTime = *theStartTimeP;
           
            //
            // burst out -transmit time packets
//            (*theFile)->fAllowNegativeTTs = false;
        }
        else
        {
//            qtFileErr = (*theFile)->fFile.Seek(*theStartTimeP, *theMaxBackupTime);
//            Float64 theFirstPacketTransmitTime = (*theFile)->fFile.GetFirstPacketTransmitTime();
//            theBackupTime = (Float32) ( *theStartTimeP - theFirstPacketTransmitTime);
//
//            //
//            // For oddly authored movies, there are situations in which the packet
//            // transmit time can be before the sample time. In that case, the backup
//            // time may exceed the max backup time. In that case, just make the backup
//            // time the max backup time.
//            if (theBackupTime > *theMaxBackupTime)
//                theBackupTime = *theMaxBackupTime;
//            //
//            // If client specifies that it can do extra buffering (new client), use the first
//            // packet transmit time as the start time for this play burst. We don't need to
//            // burst any packets because the client can do the extra buffering
//			Bool16* overBufferEnabledPtr = NULL;
//			theLen = 0;
//			theErr = QTSS_GetValuePtr(inParamBlock->inClientSession, qtssCliSesOverBufferEnabled, 0, (void**)&overBufferEnabledPtr, &theLen);
//			if ((theErr == QTSS_NoErr) && (theLen == sizeof(Bool16)) && *overBufferEnabledPtr)
//				(*theFile)->fStartTime = *theStartTimeP;
//			else
//                (*theFile)->fStartTime = *theStartTimeP - theBackupTime;
//
//
//            (*theFile)->fAllowNegativeTTs = true;
        }
    }
    
    if (qtFileErr == QTRTPFile::errCallAgain)
    {
        //
        // If we are doing RTP-Meta-Info stuff, we might be asked to get called again here.
        // This is simply because seeking might be a long operation and we don't want to
        // monopolize the CPU, but there is no other reason to wait, so just set a timeout of 0
        theErr = QTSS_SetIdleTimer(1);
        Assert(theErr == QTSS_NoErr);
        return theErr;
    }
    else if (qtFileErr != QTRTPFile::errNoError)
        return QTSSModuleUtils::SendErrorResponse(  inParamBlock->inRTSPRequest,
                                                    qtssClientBadRequest, sSeekToNonexistentTimeErr);
                                                        
    //make sure to clear the next packet the server would have sent!
//    (*theFile)->fPacketStruct.packetData = NULL;
    
    // Set the movie duration and size parameters
    Float64 movieDuration = 77.000000;//(*theFile)->fFile.GetMovieDuration();
    (void)QTSS_SetValue(inParamBlock->inClientSession, qtssCliSesMovieDurationInSecs, 0, &movieDuration, sizeof(movieDuration));
    
    UInt64 movieSize = 11610692;//(*theFile)->fFile.GetAddedTracksRTPBytes();
    (void)QTSS_SetValue(inParamBlock->inClientSession, qtssCliSesMovieSizeInBytes, 0, &movieSize, sizeof(movieSize));
    
    UInt32 bitsPerSecond =  1191000;//(*theFile)->fFile.GetBytesPerSecond() * 8;
    (void)QTSS_SetValue(inParamBlock->inClientSession, qtssCliSesMovieAverageBitRate, 0, &bitsPerSecond, sizeof(bitsPerSecond));

    Bool16 adjustPauseTime = kAddPauseTimeToRTPTime; //keep rtp time stamps monotonically increasing
    if ( true == QTSSModuleUtils::HavePlayerProfile( sServerPrefs, inParamBlock,QTSSModuleUtils::kDisablePauseAdjustedRTPTime) )
    	adjustPauseTime = kDontAddPauseTimeToRTPTime;
    
//	if (sPlayerCompatibility )  // don't change adjust setting if compatibility is off.
//		(**theFile).fAdjustPauseTime = adjustPauseTime;
	
//    if ( (**theFile).fLastPauseTime > 0 )
//        (**theFile).fTotalPauseTime += OS::Milliseconds() - (**theFile).fLastPauseTime;

    //
    // For the purposes of the speed header, check to make sure all tracks are
    // over a reliable transport
    Bool16 allTracksReliable = true;
    
    // Set the timestamp & sequence number parameters for each track.
    QTSS_RTPStreamObject* theRef = NULL;
//    for (   UInt32 theStreamIndex = 0;
//            QTSS_GetValuePtr(inParamBlock->inClientSession, qtssCliSesStreamObjects, theStreamIndex, (void**)&theRef, &theLen) == QTSS_NoErr;
//            theStreamIndex++)
//    {
//        UInt32* theTrackID = NULL;
//        theErr = QTSS_GetValuePtr(*theRef, qtssRTPStrTrackID, 0, (void**)&theTrackID, &theLen);
//        Assert(theErr == QTSS_NoErr);
//        Assert(theTrackID != NULL);
//        Assert(theLen == sizeof(UInt32));
//
//        UInt16 theSeqNum = 0;
//        UInt32 theTimestamp = (*theFile)->fFile.GetSeekTimestamp(*theTrackID); // this is the base timestamp need to add in paused time.
//
//        Assert(theRef != NULL);
//
//        if ((**theFile).fAdjustPauseTime)
//        {
//            UInt32* theTimescale = NULL;
//            QTSS_GetValuePtr(*theRef, qtssRTPStrTimescale, 0,  (void**)&theTimescale, &theLen);
//            if (theLen != 0) // adjust the timestamps to reflect paused time else leave it alone we can't calculate the timestamp without a timescale.
//            {
//                UInt32 pauseTimeStamp = CalculatePauseTimeStamp( *theTimescale,  (*theFile)->fTotalPauseTime, (UInt32) theTimestamp);
//                if (pauseTimeStamp != theTimestamp)
//                      theTimestamp = pauseTimeStamp;
//            }
//        }
//
//	    theSeqNum = (*theFile)->fFile.GetNextTrackSequenceNumber(*theTrackID);
//        theErr = QTSS_SetValue(*theRef, qtssRTPStrFirstSeqNumber, 0, &theSeqNum, sizeof(theSeqNum));
//        Assert(theErr == QTSS_NoErr);
//        theErr = QTSS_SetValue(*theRef, qtssRTPStrFirstTimestamp, 0, &theTimestamp, sizeof(theTimestamp));
//        Assert(theErr == QTSS_NoErr);
//
//        if (allTracksReliable)
//        {
//            QTSS_RTPTransportType theTransportType = qtssRTPTransportTypeUDP;
//            theLen = sizeof(theTransportType);
//            theErr = QTSS_GetValue(*theRef, qtssRTPStrTransportType, 0, &theTransportType, &theLen);
//            Assert(theErr == QTSS_NoErr);
//
//            if (theTransportType == qtssRTPTransportTypeUDP)
//                allTracksReliable = false;
//        }
//    }
    
    //Tell the QTRTPFile whether repeat packets are wanted based on the transport
    // we don't care if it doesn't set (i.e. this is a meta info session)
//     (void)  (*theFile)->fFile.SetDropRepeatPackets(allTracksReliable);// if alltracks are reliable then drop repeat packets.
        
    //
    // This module supports the Speed header if the client wants the stream faster than normal.
    Float32 theSpeed = 1;
//    theLen = sizeof(theSpeed);
//    theErr = QTSS_GetValue(inParamBlock->inRTSPRequest, qtssRTSPReqSpeed, 0, &theSpeed, &theLen);
//    Assert(theErr != QTSS_BadArgument);
//    Assert(theErr != QTSS_NotEnoughSpace);
//
//    if (theErr == QTSS_NoErr)
//    {
//        if (theSpeed > sMaxAllowedSpeed)
//            theSpeed = sMaxAllowedSpeed;
//        if ((theSpeed <= 0) || (!allTracksReliable))
//            theSpeed = 1;
//    }
//
//    (*theFile)->fSpeed = theSpeed;
    
//    if (theSpeed != 1)
//    {
//        //
//        // If our speed is not 1, append the RTSP speed header in the response
//        char speedBuf[32];
//        qtss_sprintf(speedBuf, "%10.5f", theSpeed);
//        StrPtrLen speedBufPtr(speedBuf);
//        (void)QTSS_AppendRTSPHeader(inParamBlock->inRTSPRequest, qtssSpeedHeader,
//                                    speedBufPtr.Ptr, speedBufPtr.Len);
//    }
    
    //
    // Record the requested stop time, if there is one
//    (*theFile)->fStopTime = -1;
//    theLen = sizeof((*theFile)->fStopTime);
//    theErr = QTSS_GetValue(inParamBlock->inRTSPRequest, qtssRTSPReqStopTime, 0, &(*theFile)->fStopTime, &theLen);
//
//    //
//    // Append x-Prebuffer header if provided & nonzero prebuffer needed
//    if (theBackupTime > 0)
//    {
//        char prebufferBuf[32];
//        qtss_sprintf(prebufferBuf, "time=%.5f", theBackupTime);
//        StrPtrLen backupTimePtr(prebufferBuf);
//        (void)QTSS_AppendRTSPHeader(inParamBlock->inRTSPRequest, qtssXPreBufferHeader,
//                                    backupTimePtr.Ptr, backupTimePtr.Len);
//
//    }

    // add the range header.
//    {
//        char rangeHeader[64];
//        if (-1 == (*theFile)->fStopTime)
//           (*theFile)->fStopTime = (*theFile)->fFile.GetMovieDuration();
//
//        qtss_snprintf(rangeHeader,sizeof(rangeHeader) -1, "npt=%.5f-%.5f", (*theFile)->fStartTime, (*theFile)->fStopTime);
//        rangeHeader[sizeof(rangeHeader) -1] = 0;
//
//        StrPtrLen rangeHeaderPtr(rangeHeader);
//        (void)QTSS_AppendRTSPHeader(inParamBlock->inRTSPRequest, qtssRangeHeader,
//                                    rangeHeaderPtr.Ptr, rangeHeaderPtr.Len);
//
//    }
    (void)QTSS_SendStandardRTSPResponse(inParamBlock->inRTSPRequest, inParamBlock->inClientSession, qtssPlayRespWriteTrackInfo);

    SInt64 adjustRTPStreamStartTimeMilli = 0;
    if (sPlayerCompatibility && QTSSModuleUtils::HavePlayerProfile(sServerPrefs, inParamBlock,QTSSModuleUtils::kDelayRTPStreamsUntilAfterRTSPResponse))
        adjustRTPStreamStartTimeMilli = sAdjustRTPStartTimeMilli;

   //Tell the server to start playing this movie. We do want it to send RTCP SRs, but
    //we DON'T want it to write the RTP header
   // (*theFile)->fPaused = false;
    theErr = QTSS_Play(inParamBlock->inClientSession, inParamBlock->inRTSPRequest, qtssPlayFlagsSendRTCP);
    if (theErr != QTSS_NoErr)
       return theErr;

    // Set the adjusted play time. SendPackets can get called between QTSS_Play and
    // setting fAdjustedPlayTime below. 
    SInt64* thePlayTime = NULL;
    theErr = QTSS_GetValuePtr(inParamBlock->inClientSession, qtssCliSesPlayTimeInMsec, 0, (void**)&thePlayTime, &theLen);
    Assert(theErr == QTSS_NoErr);
    Assert(thePlayTime != NULL);
    Assert(theLen == sizeof(SInt64));
//    if (thePlayTime != NULL)
//        (*theFile)->fAdjustedPlayTime = adjustRTPStreamStartTimeMilli + *thePlayTime - ((SInt64)((*theFile)->fStartTime * 1000) );
 
    bStart = true;
    return QTSS_NoErr;
}

QTSS_Error DestroySession(QTSS_ClientSessionClosing_Params* inParams)
{
    TSFileSession** theFile = NULL;
    UInt32 theLen = 0;
    QTSS_Error theErr = QTSS_GetValuePtr(inParams->inClientSession, sFileSessionAttr, 0, (void**)&theFile, &theLen);
    if ((theErr != QTSS_NoErr) || (theLen != sizeof(TSFileSession*)) || (theFile == NULL))
        return QTSS_RequestFailed;

    bStopSending = true;
    //
    // Tell the ClientSession how many samples we skipped because of stream thinning
   // UInt32 theNumSkippedSamples = (*theFile)->fFile.GetNumSkippedSamples();
   // (void)QTSS_SetValue(inParams->inClientSession, qtssCliSesFramesSkipped, 0, &theNumSkippedSamples, sizeof(theNumSkippedSamples));
   // delete *theFile;
   // DeleteFileSession(*theFile);
    return QTSS_NoErr;
}

void    DeleteFileSession(FileSession* inFileSession)
{
    delete inFileSession;
}

Float32 flowcontrol = 1.0;

//In my thought, sending thread should and must only do sending work without concerning about whether to change slice or others
void *SendPacketThread(void *inArg)
{
#ifdef CBRMETHOD
	TSFileSession *theFile =(TSFileSession *)inArg;
	QTSS_Object currentStream = (QTSS_Object)(theFile->cookie1);
	if(currentStream)
	{
		bStopSending = false;
	}
	SInt32 small = 0;
	SInt32 large = 0;
	SInt64 sleepTime = 0;
	SInt64 timeStart = 0;
	SInt32 timePeriod = theFile->m_nTimeInterval;
	SInt32 sizeSent = 0;
	SInt32 counter = 0;

	//Send packets

	while(bStopSending == false)
	{
		//When sending data, I won't be bothered by anyting
		pthread_mutex_lock(&(theFile->m_lock));
		SInt64 currentTime = OS::Milliseconds();
		//Caclulate average sending bit rate
		if(timeStart == 0)
			timeStart = currentTime;
		if(currentTime - timeStart >= timePeriod)
		{

			SInt64 n = 8000 * (SInt64)sizeSent / (currentTime - timeStart);
			theFile->m_nSendRate = n;
			theFile->m_nSendRateLast4Times[theFile->m_nSendRateRingCursor] = theFile->m_nSendRate;
			if(theFile->m_nSendRate < 0)
			{
				printf("  \n");
			}
			theFile->m_nSendRateRingCursor = (theFile->m_nSendRateRingCursor + 1) % 4;
			//printf("%d %dkbps\n", counter++, theFile->m_nSendRate / 1000);
			sizeSent = 0;
			timeStart = 0;
			for(int i = 0; i < theFile->m_Test.m_nCursor; i++)
			{
				if(counter == theFile->m_Test.m_nTick[i])
				{
					flowcontrol = theFile->m_Test.m_fValue[i];
					break;
				}
			}
		}
		//Read packets and make an rtp packet
		SInt32 rtpLength = theFile->Read7PacketsAndMakeRtp(currentTime);
		sleepTime = theFile->m_lSleepTime / flowcontrol / theFile->m_fRateFactor;
		sizeSent += (rtpLength - 24);
		//Maybe file has been sent out
		if(rtpLength == 0)
		{
			pthread_mutex_unlock(&(theFile->m_lock));
			break;
		}
		//Send new slice
		else if(rtpLength == -1)
		{
			pthread_mutex_unlock(&(theFile->m_lock));
			continue;
		}
		theFile->m_RtpPacket.packetTransmitTime = currentTime;
		//Send it
		UInt32 err = QTSS_Write(currentStream, &(theFile->m_RtpPacket), rtpLength, NULL, qtssWriteFlagsIsRTP);
		pthread_mutex_unlock(&(theFile->m_lock));
		//FIXME Acutally I am not quite sure how to set a proper sleeping time.
		//usleep() sleeps for #usecond
		usleep(sleepTime);
	}
	printf("We are done sending\n");
#else
		TSFileSession *theFile =(TSFileSession *)inArg;
			QTSS_Object currentStream = (QTSS_Object)(theFile->cookie1);
			if(currentStream)
			{
				bStopSending = false;
			}
			SInt32 small = 0;
			SInt32 large = 0;
			SInt64 sleepTime = 0;
			SInt64 timeInterval = 0;
			SInt32 sizeSent = 0;
			SInt32 counter = 0;
			Float32 flowcontrol = 1;
			//Send packets
			try{
				while(bStopSending == false)
				{
					//When sending data, I won't be bothered by anyting
					pthread_mutex_lock(&(theFile->m_lock));
					SInt64 currentTime = OS::Milliseconds();
					//Caclulate average sending bit rate
					if(timeInterval == 0)
						timeInterval = currentTime;
					if(currentTime - timeInterval >= 1000)
					{
						printf("%2.0fkbit/s\n", (float)8 * sizeSent / (currentTime - timeInterval));
						sizeSent = 0;
						timeInterval = 0;
						counter++;
						if( counter == 5)
						{
							printf("Half sending speed\n");
							flowcontrol = 2;
						}
						else if(counter == 10)
						{
							printf("Double sending speed\n");
							flowcontrol = 0.5;
						}
						else if(counter == 13)
						{
							printf("Normal sending speed\n");
							flowcontrol = 1;
						}
					}
					//Read packets and make an rtp packet
					sleepTime = theFile->m_lSleepTime * flowcontrol;
					SInt32 rtpLength = theFile->Read7PacketsAndMakeRtp(currentTime);
					sizeSent += rtpLength;
					//Maybe file has been sent out
					if(rtpLength == 0)
					{
						pthread_mutex_unlock(&(theFile->m_lock));
						break;
					}
					//Send new slice
					else if(rtpLength == -1)
					{
						pthread_mutex_unlock(&(theFile->m_lock));
						continue;
					}
					theFile->m_RtpPacket.packetTransmitTime = currentTime;
					//Send it
					UInt32 err = QTSS_Write(currentStream, &(theFile->m_RtpPacket), rtpLength, NULL, qtssWriteFlagsIsRTP);
					pthread_mutex_unlock(&(theFile->m_lock));
					//FIXME Acutally I am not quite sure how to set a proper sleeping time.
					//usleep() sleeps for #usecond
					usleep(sleepTime);
				}
				//delete theFile;
			}
			catch(...)
			{
				printf("Something wrong is happening\n");
			}
			printf("We are done sending\n");
#endif
}

QTSS_Error SendPackets(QTSS_RTPSendPackets_Params* inParams)
{
	if(bStart == true)
	{
		bStart = false;
		bStopSending = false;
		TSFileSession** theFile = NULL;
		UInt32 theLen = 0;
		QTSS_Error theErr = QTSS_GetValuePtr(inParams->inClientSession, sFileSessionAttr, 0, (void**)&theFile, &theLen);
		theCurrentTSFileSession = *theFile;
		if(theErr != QTSS_NoErr)
		{
			return theErr;
		}
		pthread_t tid;
		pthread_create(&tid, NULL, SendPacketThread, (void*)(*theFile));
	}
	//Sleep time
	inParams->outNextPacketTime = 500;
	return QTSS_NoErr;
}

Bool16 GetPCR(unsigned char *p_ts_packet, SInt64 *p_pcr, SInt32 *p_pcr_pid)
{
	Assert(p_ts_packet != NULL);
	SInt64 	lPcrBase = 0;
	SInt32	nPcrExt = 0;
	UInt32	nPsiPcr = 0;
	*p_pcr = 0;
	*p_pcr_pid = 0;
	//FIXME
	//nPsiPcr = ((p_ts_packet[1] >> 6) & 0x01);
	if((p_ts_packet[0] == 0x47) && (p_ts_packet[3] & 0x20) && (p_ts_packet[5] & 0x10) && (p_ts_packet[4] >= 7))
	{
		*p_pcr_pid = ((SInt32)p_ts_packet[1] & 0x1F) << 8 | p_ts_packet[2];
		lPcrBase =((SInt64)p_ts_packet[6] << 25)|((SInt64)p_ts_packet[7] << 17)|((SInt64)p_ts_packet[8] << 9)|((SInt64)p_ts_packet[9] << 1)|((SInt64)p_ts_packet[10] >> 7);
		nPcrExt =  ((SInt32)p_ts_packet[10] & 0x0001) << 8| p_ts_packet[11];
		*p_pcr = lPcrBase * 300 + nPcrExt;
		return true;
	}
	return false;
}
void 	TSFileSession::GetNextPcr(int inPacketStartOffset)
{
#if 1
	SInt64	lPcr = 0;
	SInt32	nPid = 0;
	SInt32	nReadSize;
	SInt32	nSeq = inPacketStartOffset;
	unsigned char pBufferForTsPacket[188];
	do
	{
		nReadSize = fread(pBufferForTsPacket, 1, 188, m_pCurrentFile);
		if(nReadSize == 0)
			return;
		if(GetPCR(pBufferForTsPacket, &lPcr, &nPid) == true)
		{
			UpdatePcr(lPcr, nSeq);
			break;
		}
		nSeq++;
	}while(1);
	//Reset file pointer
	fseek(m_pCurrentFile, inPacketStartOffset * 188, SEEK_SET);
#endif
}
//FILE *fileBitrate = fopen("filebitrate", "w");
Bool16 	TSFileSession::GetTwoPcr(int inPacketOffset)
{
#if 0
	//m_nBitRate = m_FileArray[m_nCurrentSliceType].bitrate;
	//m_lSleepTime = 188 * 7 * 8 * 1000 * 1000/ m_nBitRate;
	return true;
#else


	SInt64	lPcr = 0;
	SInt32	nPid = 0;
	SInt32	nReadSize;
	SInt32	nSeq = inPacketOffset;
	m_lFirstPcr = 0;
	m_lSecondPcr = 0;
	unsigned char pBufferForTsPacket[188];
	//FILE *pfile = fopen("/usr/local/movies/test2.ts", "rb");
	//nReadSize = fread(pBufferForTsPacket, 1, 188, pfile);
	do
	{
		nReadSize = fread(pBufferForTsPacket, 1, 188, m_pCurrentFile);
		if(nReadSize == 0)
			return false;
		Assert(nReadSize == 188);
		if(GetPCR(pBufferForTsPacket, &lPcr, &nPid) == true)
		{
			if(m_lFirstPcr == 0)
			{
				m_lLastTimeWeGetPcr = OS::Milliseconds();
				m_lFirstPcr = lPcr;
				m_nFirstPcrSeq = nSeq;
			}
			else if(m_lSecondPcr == 0)
			{
				m_lLastTimeWeGetPcr = OS::Milliseconds();
				m_lSecondPcr = lPcr;
				m_nSecondPcrSeq = nSeq;
			}
			//Coz we get both first and second pcr, if we find they both are zero, that means
			//we are done searching
			else
			{
				break;
			}
		}
		nSeq++;
	}while(1);
	//Reset file pointer
	int ret =fseek(m_pCurrentFile, inPacketOffset * 188, SEEK_SET);
	//Caculate bit rate and sleep time
	m_nBitRate = (Float64)(m_nSecondPcrSeq - m_nFirstPcrSeq)*188*8*27000000/(m_lSecondPcr - m_lFirstPcr);
	//fprintf(fileBitrate, "%d\t%d\t%d\n", m_nBitRate, m_nFirstPcrSeq, m_nSecondPcrSeq);
	m_lSleepTime = (Float32)(m_lSecondPcr - m_lFirstPcr) * 7 / ((m_nSecondPcrSeq - m_nFirstPcrSeq) * 27);
	if(m_lFirstPcr && m_lSecondPcr)
	{
		return true;
	}
	return false;
#endif
}

void TSFileSession::UpdatePcr(UInt64 lNewPcr, UInt32 nSeq)
{
#if 0
	return ;
	m_nBitRate = m_FileArray[m_nCurrentSliceType].bitrate;
	m_lSleepTime = (SInt64)188 * 7 * 8 * 1000 * 1000 / m_nBitRate;
#else
	if(nSeq == m_nFirstPcrSeq || nSeq == m_nSecondPcrSeq)
		return;
	m_lFirstPcr = m_lSecondPcr;
	m_nFirstPcrSeq = m_nSecondPcrSeq;
	m_lSecondPcr = lNewPcr;
	m_nSecondPcrSeq = nSeq;
	m_lLastTimeWeGetPcr = OS::Milliseconds();
	//Calculate the average bit rate
	m_nBitRate = (Float64)(m_nSecondPcrSeq - m_nFirstPcrSeq)*188*8*27000000/(m_lSecondPcr - m_lFirstPcr);
	//fprintf(fileBitrate, "%d\t%d\t%d\n", m_nBitRate, m_nFirstPcrSeq, m_nSecondPcrSeq);
	//New method to calculate sleep time(every 7 ts packets)
	m_lSleepTime = (Float32)(m_lSecondPcr - m_lFirstPcr) * 7 / ((m_nSecondPcrSeq - m_nFirstPcrSeq) * 27);
#endif
}
UInt32	TSFileSession::Read7PacketsAndMakeRtp(SInt64 inTimeStamp)
{
#if 0
	SInt32 	readSize = 0;
	UInt32 	rtpPayloadSize = 0;
	//First # bytes are reserved for rtp header
	unsigned char *pBufferFor7Packets = m_RtpBuffer + 12 + m_nRtpXHeaderLength;
	for(int i = 0; i < 7; i++)
	{
		readSize = fread(pBufferFor7Packets + rtpPayloadSize, 1, 188, m_pCurrentFile);
		if(readSize == 0)
			break;
		Assert(readSize == 188);
		//Assure that when switching slice is triggered, we should send out our complete frame in last slice
		//We can see if payload_unit_start == 1 or not
		//If yes, we switch it to new slice
		if(m_bSliceHasChanged == true && ((pBufferFor7Packets + rtpPayloadSize)[1] >> 6 & 0x01 == 1))
		{
			m_bSliceHasChanged = false;
			m_nCurrentTsPacketOffset = m_nNewCurrentTsPacketOffset;
			m_pCurrentFile = m_nNewCurrentFile;
			m_nCurrentSliceType = m_nNewCurrentSliceType;
			m_nBitRate = m_FileArray[m_nCurrentSliceType].bitrate;
			m_lSleepTime = (SInt64)188 * 7 * 8 * 1000 * 1000 / m_nBitRate;
			//Return size so as to inform caller skip this sending
			if(rtpPayloadSize == 0)
				return -1;
			else
				return CreateRtp(rtpPayloadSize, 33, inTimeStamp, true);
		}
		//If not, we still send current slice

		//Record our reading track
		rtpPayloadSize += readSize;
		m_nCurrentTsPacketOffset++;
	}
	//Create rtp and return rtp length
	return CreateRtp(rtpPayloadSize, 33, inTimeStamp, true);
#else
	SInt32 	readSize = 0;
	SInt64	lPcr = 0;
	SInt32	nPcrPid = 0;
	UInt32 	rtpPayloadSize = 0;
	//First # bytes are reserved for rtp header
	unsigned char *pBufferFor7Packets = m_RtpBuffer + 12 + m_nRtpXHeaderLength;
	for(int i = 0; i < 7; i++)
	{
		readSize = fread(pBufferFor7Packets + rtpPayloadSize, 1, 188, m_pCurrentFile);
		if(readSize == 0)
			break;
		//Assure that when switching slice is triggered, we should send out our complete frame in last slice
		//We can see if payload_unit_start == 1 or not
		//If yes, we switch it to new slice
		if(m_bSliceHasChanged == true && ((pBufferFor7Packets + rtpPayloadSize)[1] >> 6 & 0x01 == 1))
		{
			//Debug
			//printf("slice type %d\tts packet %d\tpackets read %d\tbytes %2x %2x %2x %2x\n", m_nCurrentSliceType, m_nCurrentTsPacketOffset, i, pBufferFor7Packets[5], pBufferFor7Packets[6], pBufferFor7Packets[7], pBufferFor7Packets[8]);
			m_bSliceHasChanged = false;
			m_nCurrentTsPacketOffset = m_nNewCurrentTsPacketOffset;
			m_pCurrentFile = m_nNewCurrentFile;
			m_nCurrentSliceType = m_nNewCurrentSliceType;
			//Get new bitrate and sleep-time
			if(GetTwoPcr(m_nCurrentTsPacketOffset) == false)
			{
				bStopSending = true;
				return -2;
			}
			//Return size so as to inform caller skip this sending
			if(rtpPayloadSize == 0)
				return -1;
			else
				return CreateRtp(rtpPayloadSize, 33, inTimeStamp, true);
		}
		//If not, we still send current slice

		//Check whether we have a pcr
		if(GetPCR(pBufferFor7Packets + rtpPayloadSize, &lPcr, &nPcrPid))
		{
			UpdatePcr(lPcr, m_nCurrentTsPacketOffset);
		}
		//Record our reading track
		rtpPayloadSize += readSize;
		m_nCurrentTsPacketOffset++;
	}
	if(m_nCurrentTsPacketOffset >= m_nSecondPcrSeq)
	{
		GetNextPcr(m_nCurrentTsPacketOffset);
	}
	//Create rtp and return rtp length
	return CreateRtp(rtpPayloadSize, 33, inTimeStamp, true);
#endif

}

UInt32 TSFileSession::CreateRtp(SInt32 inLength, SInt32 inPayloadType, SInt64 inTimeStamp, Bool16 inMarker)
{
	Assert(inLength > 0);
	if(inLength == 0)
		return 0;
	//Extend rtp
	m_RtpBuffer[0] = 0x90;
	m_RtpBuffer[1] = (inMarker ? 0x80 : 0x00) | inPayloadType;

	m_RtpBuffer[2] = ((m_nCurrentRtpPacketSeq) >> 8) & 0xff;
	m_RtpBuffer[3] = (m_nCurrentRtpPacketSeq) & 0xff;

	//int a = int((m_RtpBuffer[2]) << 8 | (m_RtpBuffer[3]));

	m_RtpBuffer[4] = (unsigned char)(inTimeStamp >> 24) & 0xff;
	m_RtpBuffer[5] = (unsigned char)(inTimeStamp >> 16) & 0xff;
	m_RtpBuffer[6] = (unsigned char)(inTimeStamp >>  8) & 0xff;
	m_RtpBuffer[7] = (unsigned char)inTimeStamp & 0xff;

	//int *t = ((int*)(m_RtpBuffer + 4));

	m_RtpBuffer[ 8] = (*m_pSSRC >> 24) & 0xff;
	m_RtpBuffer[ 9] = (*m_pSSRC >> 16) & 0xff;
	m_RtpBuffer[10] = (*m_pSSRC >>  8) & 0xff;
	m_RtpBuffer[11] = *m_pSSRC & 0xff;

	m_RtpBuffer[12] = 0x00;
	m_RtpBuffer[13] = 0x00;

	//Extended rtp header length
	m_RtpBuffer[14] = 0x00;
	m_RtpBuffer[15] = 2;

	//m_RtpBuffer[16 17 18 19]contains current average bit rate
	//# kbps
	SInt32 nBitrate = m_FileArray[m_nCurrentSliceType].bitrate;
	m_RtpBuffer[19] = (nBitrate >> 24) & 0xFF;
	m_RtpBuffer[18] = (nBitrate >> 16) & 0xFF;
	m_RtpBuffer[17] = (nBitrate >>  8) & 0XFF;
	m_RtpBuffer[16] = nBitrate & 0xFF;

	SInt32 nSendRate = m_nSendRate;
	m_RtpBuffer[23] = (nSendRate >> 24) & 0xFF;
	m_RtpBuffer[22] = (nSendRate >> 16) & 0xFF;
	m_RtpBuffer[21] = (nSendRate >>  8) & 0XFF;
	m_RtpBuffer[20] = nSendRate & 0xFF;

	m_nCurrentRtpPacketSeq++;

	m_RtpPacket.packetData = m_RtpBuffer;
	return inLength + 12 + m_nRtpXHeaderLength;
}

QTSS_Error ProcessRTCP(QTSS_RTCPProcess_Params* inParamBlock)
{
	//return QTSS_NoErr;
	UInt32* uint32Ptr = NULL;
	UInt32 theLen = 0;
	(void)QTSS_GetValuePtr(inParamBlock->inRTPStream, qtssRTPStrJitter, 0, (void**)&uint32Ptr, &theLen);

	TSFileSession** theFile = NULL;
	QTSS_Error theErr = QTSS_GetValuePtr(inParamBlock->inClientSession, sFileSessionAttr, 0, (void**)&theFile, &theLen);


	UInt32 jitter = (*uint32Ptr) & 0xFFFF;
	UInt32 buffer = (((*uint32Ptr) >> 16) & 0X3FF) / 10.0;

	UInt32 loss = ((*uint32Ptr) >> 26) & 0X3;
	UInt32 id = ((*uint32Ptr) >> 31) & 0X1;
	static int fc_Outband = 0;
	static int fc_JitterGreater = 0;
	if(id == 1)
	{
		(*theFile)->FlowControl(jitter, buffer, loss);
	}
	(*theFile)->m_nLastBuffer = buffer;
	(*theFile)->m_fLastRateFactor = (*theFile)->m_fRateFactor;
#if 0
	(*theFile)->m_FeedBack.switchSlice = ((*uint32Ptr) >> 30) & 0x01;
	(*theFile)->m_FeedBack.newSlice = ((*uint32Ptr) >> 24) & 0x0F;
	(*theFile)->m_FeedBack.expectedRate = (*uint32Ptr) & 0x00FFFFFF;
	if((*theFile)->m_FeedBack.expectedRate > 0)
	{
		(*theFile)->m_FeedBack.clientassign = 1;
		(*theFile)->m_nBitRate = (*theFile)->m_FeedBack.expectedRate;
		(*theFile)->m_lSleepTime = (SInt64)188 * 7 * 8 * 1000 * 1000 / (*theFile)->m_nBitRate;
	}
	else
	{
		(*theFile)->m_FeedBack.clientassign = 0;
	}
	printf("Switch %d New slice %d Expected rate %2.0fkbps\n", (*theFile)->m_FeedBack.switchSlice, (*theFile)->m_FeedBack.newSlice, (Float64)(*theFile)->m_FeedBack.expectedRate / 1000);
	if((*theFile)->m_FeedBack.switchSlice == 1)
	{
		(*theFile)->ChangeSlice((TSFileSession::SLICETYPE)(*theFile)->m_FeedBack.newSlice);
	}
#endif

	return QTSS_NoErr;
#if 0
	static int counter = 0;
	counter++;
	if(counter == 4)
	{
		printf("Switch slice to LOW\n");
		TSFileSession** theFile = NULL;
		UInt32 theLen = 0;
		QTSS_Error theErr = QTSS_GetValuePtr(inParamBlock->inClientSession, sFileSessionAttr, 0, (void**)&theFile, &theLen);
		(*theFile)->ChangeSlice((*theFile)->LOW);
	}
	else if(counter == 6)
	{
		printf("Switch slice HIGH\n");
		TSFileSession** theFile = NULL;
		UInt32 theLen = 0;
		QTSS_Error theErr = QTSS_GetValuePtr(inParamBlock->inClientSession, sFileSessionAttr, 0, (void**)&theFile, &theLen);
		(*theFile)->ChangeSlice((*theFile)->HIGH);
	}
	else if(counter == 8)
	{
		printf("Switch slice MEDIUM\n");
		TSFileSession** theFile = NULL;
		UInt32 theLen = 0;
		QTSS_Error theErr = QTSS_GetValuePtr(inParamBlock->inClientSession, sFileSessionAttr, 0, (void**)&theFile, &theLen);
		(*theFile)->ChangeSlice((*theFile)->MEDIUM);
	}
	//printf("Jitter %d\n", jitter);
	return QTSS_NoErr;
#endif
}
UInt32	TSFileSession::ChangeSlice(SLICETYPE inType)
{
	if(inType == m_nCurrentSliceType)
		return -1;
	printf("\nChanging slice...\n");
	//To avoid sending thread running at moment, we should lock it
	pthread_mutex_lock(&m_lock);
	if(m_nCurrentSliceType > inType)
		m_fLastRateFactor = m_fRateFactor = 1.2;
	else
		m_fLastRateFactor = m_fRateFactor = 0.5;

	m_bSliceHasChanged = true;
	/*m_pCurrentFile*/m_nNewCurrentFile = m_FileArray[inType].file;
	//Get next slice that we wanna send
	int nextSlice = GetCurrentSliceIndex() + 1;
	//printf("nextSlice %d\n", nextSlice);
	//Attention We have to replace slice type with new type after above statement
	/*m_nCurrentSliceType*/m_nNewCurrentSliceType = inType;
	int	nextIFramePosition = GetIFramePosition(m_nNewCurrentSliceType, nextSlice);
	//printf("nextIFrame %d\n", nextIFramePosition);
	if(nextIFramePosition == -1)
	{
		bStopSending = true;
		return -1;
	}
	fseek(m_nNewCurrentFile, nextIFramePosition * 188, SEEK_SET);
	/*m_nCurrentTsPacketOffset*/m_nNewCurrentTsPacketOffset = nextIFramePosition;
	pthread_mutex_unlock(&m_lock);
	return 0;
}
//Return current slice index using current ts packet offset
SInt32	TSFileSession::GetCurrentSliceIndex()
{
	for(int i = 1;  m_SliceOffset[m_nCurrentSliceType][i] != -1; i++)
	{
		if(m_nCurrentTsPacketOffset > m_SliceOffset[m_nCurrentSliceType][i])
			continue;
		return i - 1;
	}
	return -1;
}

SInt32	TSFileSession::GetIFramePosition(SLICETYPE inType, SInt32 inSliceIndex)
{
	int sliceOffset = m_SliceOffset[inType][inSliceIndex];
	for(int i = 0; m_IFrameOffset[inType][i] != -1; i++)
	{
		if(sliceOffset > m_IFrameOffset[inType][i])
			continue;
		return m_IFrameOffset[inType][i];
	}
	return -1;
}
Float32	AdjustRateFactorByJitter(UInt32 inJitter)
{
	switch(inJitter)
	{
	case 0:
		return 1;
	case 1:
		return 0.9;
	case 2:
		return 0.85;
	case 3:
		return 0.8;
	default:
		return 1;
		break;
	}
}
Float32 TSFileSession::GetAvgSendRate()
{
	float Sum = 0;
	int i = 0;
	for(; i < 4; i++)
	{
		if(m_nSendRateLast4Times[i] == -1)
			break;
		Sum += m_nSendRateLast4Times[i];
	}
	if(i == 0)
		return m_FileArray[m_nCurrentSliceType].bitrate / 1000;
	else
		return Sum / i;
}
static float lastRateWhenProbing = 0;
void 	TSFileSession::FlowControl(UInt32 inJitter, UInt32 inBuffer, UInt32 inLoss)
{
	printf("---------------RTCP j(%d) b(%d) l(%d)---------------\n", inJitter, inBuffer, inLoss);
	//@1 Low buffer utility
	//@2 High buffer utility
	//@3 Normal buffer utility
	//@4 High or many jitter
	//@5 Normal jitter
	static int nextProbeSendRate = 0;
	static int sliceChangeOkAndDownToNormal = 0;
	if(CheckAndDoChangeSlice(inJitter) == false)
	{
		ProbeHigherSendingRate(inJitter, inBuffer);
	}
	else
	{
		//Clear
		lastRateWhenProbing = 0;
	}
	printf("\n");
#if 0
	printf("---------------RTCP j(%d) b(%d) l(%d)---------------\n", inJitter, inBuffer, inLoss);
	//@1 Low buffer utility
	//@2 High buffer utility
	//@3 Normal buffer utility
	//@4 High or many jitter
	//@5 Normal jitter
	static int nextProbeSendRate = 0;
	static int sliceChangeOkAndDownToNormal = 0;
	m_fLastRateFactor = m_fRateFactor;
	if(inBuffer <= 20)
	{
		printf("buffer < 20  ");
		if(inJitter == 0 && m_nCurrentSliceType != HIGH)
		{
			m_fRateFactor = 3.0;
			printf("jitter = 0 rf %f  ", m_fRateFactor);
		}
		else if(m_nCurrentSliceType == HIGH)
		{
			m_fRateFactor = 1.8;
		}
		else
		{
			CheckAndDoChangeSlice(GetAvgSendRate(), inJitter);
		}
		printf("jitter == %d rf %f  ", inJitter, m_fRateFactor);
	}
	else if(inBuffer <= 60)
	{
		printf("buffer <= 60  ");
		if(sliceChangeOkAndDownToNormal > 0)
		{
			if(inBuffer < 50)
				m_fRateFactor = 1.0;
			sliceChangeOkAndDownToNormal--;
			printf("  sliceChangeOkAndDownToNormal %d \n", sliceChangeOkAndDownToNormal);
			return;
		}

		//If network is good
		if(inJitter == 0 && m_nCurrentSliceType != HIGH)
		{
			nextProbeSendRate++;
		}
		//After a continous time, maybe we need to probe a higher sending rate again
		//If we did not encounter bad network in # seconds, then we probe a higher rate
		if(m_nLastBuffer >= 80)
		{
			nextProbeSendRate = 0;
		}
		else if(inBuffer <= 45 && nextProbeSendRate >= 3)
		{
			printf("  ***** trying to probe higher rate ");
			m_fRateFactor = 3.0;
			//nextProbeSendRate = 0;
		}
		else
		{
			m_fRateFactor = 1.0;
		}
		if(CheckAndDoChangeSlice(GetAvgSendRate(), inJitter) == true)
		{
			nextProbeSendRate = 0;
			m_fRateFactor = 0.1;
			sliceChangeOkAndDownToNormal = 2;
		}
		printf("jitter = %d rf %f  ", inJitter, m_fRateFactor);
	}
	else if(inBuffer <= 80)
	{
		printf("buffer < 80  ");
		if(sliceChangeOkAndDownToNormal > 0)
		{
			sliceChangeOkAndDownToNormal--;
			printf("  sliceChangeOkAndDownToNormal %d \n", sliceChangeOkAndDownToNormal);
			return;
		}
		//If we know this time we just wanna probe a higher rate
		if(nextProbeSendRate >= 3)
		{

		}
		else
		{
			if(inBuffer < 65)
				m_fRateFactor = 1.0;
			else
				m_fRateFactor = 0.3;
		}
		if(CheckAndDoChangeSlice(GetAvgSendRate(), inJitter) == true || inJitter > 2)
		{
			m_fRateFactor = 0.1;
			if(inJitter == 0)
				sliceChangeOkAndDownToNormal = 2;
			nextProbeSendRate = 0;
		}
		printf("jitter = %drf %f  ", inJitter, m_fRateFactor);

	}
	else
	{
		printf("buffer > 80  ");
		//We will stop probing here
		nextProbeSendRate = 0;
		CheckAndDoChangeSlice(GetAvgSendRate(), inJitter);
		m_fRateFactor = 0.1;
		printf("jitter = %drf %f  ", inJitter, m_fRateFactor);
	}
	printf("\n");
#endif
}
bool 	TSFileSession::CheckAndDoChangeSlice(UInt32 inJitter)
{
	//@1 Low  	-> 	Medium
	//@2 Medium -> 	High
	//@3 High 	->	Medium
	//@4 Medium	->	Low
	static int HighDfCounter = 0;
	static int LastHighDf = 0;
	int CurrentSendRate = GetAvgSendRate() / 1000;
	printf("Current send rate %d kbps ", CurrentSendRate);
	int ret = -1;
	//Switch up
	if(inJitter <= 60)
	{
		switch(m_nCurrentSliceType)
		{
		case LOW:
			if(CurrentSendRate >= m_FileArray[MEDIUM].bitrate / 1000)
			{
				ret = ChangeSlice(MEDIUM);
				printf("  #Change slice# LOW->MEDIUM  ");
			}
			break;
		case MEDIUM:
			if(CurrentSendRate >= m_FileArray[HIGH].bitrate / 1000)
			{
				ret = ChangeSlice(HIGH);
				printf("  #Change slice# MEDIUM->HIGH  ");
			}
			break;
		}
	}
	//Switch down
	else if(inJitter >= 300)
	{
		//If last time jitter is smaller than this one
		if(LastHighDf < inJitter)
		{
			printf("Jitter is higher!\n");
			switch(m_nCurrentSliceType)
			{
			case HIGH:
				ret = ChangeSlice(MEDIUM);
				printf("  #Change slice# HIGH->MEDIUM  ");
				break;
			case MEDIUM:
				ret = ChangeSlice(LOW);
				printf("  #Change slice# MEDIUM->LOW  ");
				break;
			}
		}
	}
	LastHighDf = inJitter;
	return ret != -1;


#if 0
	int CurrentSendRate = GetAvgSendRate() / 1000;
	printf("Current send rate %d kbps ", CurrentSendRate);
	int ret = -1;
	switch(m_nCurrentSliceType)
	{
	case LOW:
		if(CurrentSendRate >= m_FileArray[MEDIUM].bitrate / 1000 * 0.8 && inJitter == 0)
		{
			ret = ChangeSlice(MEDIUM);
			printf("  #Change slice# LOW->MEDIUM  ");
			return ret == 0;
		}
		break;
	case MEDIUM:
		if(CurrentSendRate >= m_FileArray[HIGH].bitrate / 1000 * 0.8 && inJitter == 0)
		{
			ret = ChangeSlice(HIGH);
			printf("  #Change slice# MEDIUM->HIGH  ");
			return ret == 0;
		}
		else if(CurrentSendRate <= m_FileArray[LOW].bitrate / 1000 * 1.5 && inJitter >= 2)
		{
			ret = ChangeSlice(LOW);
			printf("  #Change slice# MEDIUM->LOW  ");
			return ret == 0;
		}
		break;

	case HIGH:
		if(CurrentSendRate <= m_FileArray[LOW].bitrate / 1000 * 1.5 && inJitter >= 2)
		{
			ret = ChangeSlice(LOW);
			printf("  #Change slice# HIGH->LOW  ");
			return ret == 0;
		}
		else if(CurrentSendRate <= m_FileArray[MEDIUM].bitrate / 1000 * 1.5  && inJitter >= 2)
		{
			ret = ChangeSlice(MEDIUM);
			printf("  #Change slice# HIGH->MEDIUM  ");
			return ret == 0;
		}
		break;
	}
	return false;
	printf("  No Change Slice  ");
#endif
}

void	TSFileSession::ProbeHigherSendingRate(UInt32 inJitter, UInt32 inBuffer)
{
	static int lastHighJitter = 0;
	static bool stillTryProbing = false;
	static bool	DropDownBufferToSpeedUp = false;
	static enum{UNDERFLOW = 0, SPEEDUP, STAINABLE, OVERFLOW}lastArea = UNDERFLOW;
	static bool firstTimeToPull = true;
	static bool tryPullUpBuffer = false;
	static bool tryFindHigherRate = false;
	stillTryProbing = (m_nCurrentSliceType != HIGH);
	static int lastTimeSliceType = 0;
	if(lastTimeSliceType == 0)
		lastTimeSliceType = m_nCurrentSliceType;
	if(lastTimeSliceType == m_nCurrentSliceType)
	{
		tryFindHigherRate = true;
	}
	else
	{
		tryFindHigherRate = false;
	}
	if(inJitter >= 100)
	{
		m_fRateFactor *= 0.8;
		printf("Jitter too larege\n");
	}
	else
	{
		if(inBuffer <= 10)
		{
			if(firstTimeToPull == true)
			{
				firstTimeToPull = false;
				tryFindHigherRate = true;
			}
			else
			{
				tryPullUpBuffer = true;
				m_fRateFactor = 1.5;
			}
			lastArea = UNDERFLOW;
		}
		else if(inBuffer > 10 && inBuffer <= 40)
		{
			if(tryFindHigherRate == true)
			{
				tryPullUpBuffer = false;
				m_fRateFactor *= 1.5;
			}
			else if(tryPullUpBuffer == true)
			{
				tryFindHigherRate = false;
				m_fRateFactor *= 1.1;
			}
			else
			{
				m_fRateFactor = 1.1;
			}
			lastArea = SPEEDUP;
		}
		else if(inBuffer > 40 && inBuffer <= 80)
		{
			if(tryPullUpBuffer == true)
			{
				tryPullUpBuffer = false;
				m_fRateFactor = 1.0;
			}
			else if(tryFindHigherRate == true && lastArea != STAINABLE)
			{
				if(GetAvgSendRate() / 1000 > 1000)
				{
					m_fRateFactor *= 0.8;
				}
			}
			else
			{
				if(stillTryProbing == true)
				{
					tryFindHigherRate = true;
					tryPullUpBuffer = false;
					m_fRateFactor = 0.6;
				}
				else
				{
					if(inBuffer < 60)
						m_fRateFactor = 1.0;
					else
						m_fRateFactor = 0.7;
					if(GetAvgSendRate() / 1000 > 1000)
					{
						m_fRateFactor *= 0.8;
					}
				}
			}
			//Does other case exist?
			lastArea = STAINABLE;
		}
		else
		{
			tryFindHigherRate = false;
			m_fRateFactor = 0.1;
			lastArea = OVERFLOW;
		}
	}
	lastHighJitter = inJitter;
	printf("\nRR %2.2f  ", m_fRateFactor);
#if 0
	//If we try to probe #times, and still encounter big jitter when speed up, then
	//we have to keep current speed
	static int networkLimitTimes = 0;
	static int weEncounterNetWorkLimit = 0;
	//Hey, it tells us network is getting bad
	if(inJitter >= 100)
	{
		//weEncounterNetWorkLimit = 1;
		networkLimitTimes++;
		m_fRateFactor *= 0.8;
		weEncounterNetWorkLimit = 1;
		printf(" Bad network %d |", networkLimitTimes);
		//printf("RR %2.2f  ", m_fRateFactor);
		//return;
	}
	else
	{
		//weEncounterNetWorkLimit = 0;
		//See if we encounter bad network before
		if(networkLimitTimes != 0)
			weEncounterNetWorkLimit = 1;
		else
			weEncounterNetWorkLimit = 0;
		printf(" Good network %d weEncounterNetWorkLimit %d |", networkLimitTimes, weEncounterNetWorkLimit);
	}

	//With good network and adequete remaining buffer, why not speed up, man?
	if(inBuffer < 50 && inJitter < 60)
	{
		//Still try to probe if network is good(and before)
		if(networkLimitTimes == 0 && weEncounterNetWorkLimit == 0)
		{
			printf(" Still try to probe coz' good network and no limit before |");
			if(m_fRateFactor < 1.0)
				m_fRateFactor = 1.0;
			else
				m_fRateFactor *= 1.5;
		}
		//But if we met bad network, keep here
		else if(weEncounterNetWorkLimit == 1)
		{
			printf(" Coz' we met limit before, we keep rate here|");
			m_fRateFactor *= 1.0;
		}
//		if(inBuffer > 40 && m_nCurrentSliceType == HIGH)
//			m_fRateFactor *= 1.0;
	}
	//Overflow warning
	if(inBuffer > 90)
	{
		m_fRateFactor = 0.1;
	}
	//Overflow buffer
	else if(inBuffer > 80)
	{
		m_fRateFactor = 0.2;
	}

	if(inBuffer >= 50 && inBuffer <= 70)
	{
		//Good network, so we keep norminal rate here
		if(networkLimitTimes == 0 && weEncounterNetWorkLimit == 0)
		{
			printf(" Good network and we met no limit before so we keep norminal rate |");
			m_fRateFactor *= 1.0;
		}
		else if(weEncounterNetWorkLimit == 1)
		{
			printf(" We met bad network before so keep last rate here |");
			m_fRateFactor *= 1.0;
		}

//		if(m_nCurrentSliceType == HIGH)
//			m_fRateFactor *= 1.0;
	}
#endif

}



