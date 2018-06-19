#pragma once

#include "FFMPEG/Common.h"
#include "FFMPEG/Frame.h"

#include <memory>
#include <vector>
#include <string>

void FFMPEGErrorToString(int ErrorCode, char* Buffer, size_t BufferSize);

#if defined _WIN32
#include <windows.h>
#define STREAM_ERROR( X, Result )\
	FFMPEGErrorToString(Result, m_ErrorMessage, sizeof(m_ErrorMessage)/sizeof(m_ErrorMessage[0]));\
	m_LineNumber = __LINE__;\
	/*if( IsDebuggerPresent() ) __debugbreak();*/\
	return CameraStreamError::X;
#else
#define STREAM_ERROR( X, Result )\
	FFMPEGErrorToString(Result, m_ErrorMessage, sizeof(m_ErrorMessage)/sizeof(m_ErrorMessage[0]));\
	m_LineNumber = __LINE__;\
	return CameraStreamError::X;
#endif

namespace Witness{
namespace Camera{

struct StreamData
{
	StreamData()
	: ConversionContext( nullptr )
	, FormatContext( nullptr )
	, CodecContext( nullptr )
	, StreamOptions( nullptr )
	, PixelFormat()
	, Width( 0 )
	, Height( 0 )
	, CodecID( AV_CODEC_ID_H264 )
	, CodecTag( 0 )
	, StreamIndex( 0 )
	, ChosenStreamIndex( 0 )
	, IsVideo( true )
	, IsFirstFrame( true )
	, HasOneTimeInitialized( false )
	, HasInitialized( false )
	, HasFinalised( false )
	, DTS( 0 )
	, PTS( 0 )
	{
		AspectRatio.den = 0;
		AspectRatio.num = 0;

		Framerate.den = 0;
		Framerate.num = 0;
	}

	~StreamData()
	{
		FreeAllQueuedPackets();
	}

	void FreeAllQueuedPackets()
	{
		for (auto PacketIter = PacketsBacklog.begin(); PacketIter != PacketsBacklog.end(); ++PacketIter )
		{
			av_packet_unref( &(*PacketIter) );
		}

		PacketsBacklog.clear();
		PacketsPerKeyframe.clear();
		PacketsPerKeyframe.push_back(0);
	}

	void FreeToMinimumBacklog(size_t MinFrames)
	{
		for (auto FrameIter = PacketsPerKeyframe.begin(); FrameIter != PacketsPerKeyframe.end(); ++FrameIter )
		{
			FreePacketsFromBacklog(*FrameIter);

			if (MinFrames == 0)
			{
				break;
			}

			MinFrames--;
		}
	}

	void FreePacketsFromBacklog(size_t PacketsToDelete)
	{
		//Can delete the old data now
		auto EndOfList = PacketsBacklog.begin();
		for (auto PacketIter = PacketsBacklog.begin(); PacketIter != PacketsBacklog.end(); ++PacketIter )
		{
			if (PacketsToDelete == 0)
			{
				break;
			}

			av_packet_unref( &(*PacketIter) );

			PacketsToDelete--;
			++EndOfList;
		}

		PacketsBacklog.erase(PacketsBacklog.begin(), EndOfList);
	}

	void DeleteOldestKeyframe( size_t MaxFrames )
	{
		if (PacketsBacklog.size() > MaxFrames )
		{
			FreePacketsFromBacklog(PacketsPerKeyframe[0]);
			PacketsPerKeyframe.erase(PacketsPerKeyframe.begin());
		}
		PacketsPerKeyframe.push_back(0);
	}

	std::shared_ptr<FFMPEG::Frame>	Input;
	std::shared_ptr<FFMPEG::Frame>	Output;

	std::vector<AVPacket>			PacketsBacklog;
	std::vector<int>				PacketsPerKeyframe;

	std::string Path;

	AVPacket			Packet;

	SwsContext*			ConversionContext;
	AVFormatContext*	FormatContext;
	AVCodecContext*		CodecContext;
	AVDictionary*		StreamOptions;

	AVRational			AspectRatio;
	AVRational			Framerate;
	AVPixelFormat		PixelFormat;
	AVCodecID			CodecID;
	int					CodecTag;

	unsigned int		StreamIndex;
	unsigned int		ChosenStreamIndex;

	unsigned int		Width;
	unsigned int		Height;

	bool				IsVideo;
	bool				IsFirstFrame;
	bool				HasOneTimeInitialized;
	bool				HasInitialized;
	bool				HasFinalised;

	int64_t				DTS;
	int64_t				PTS;
};

}}
