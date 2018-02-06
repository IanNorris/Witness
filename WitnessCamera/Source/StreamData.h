#pragma once

#include "FFMPEG/Common.h"
#include "FFMPEG/Frame.h"

#include <memory>
#include <vector>

#if defined _WIN32
#include <windows.h>
#define STREAM_ERROR( X )\
	m_LineNumber = __LINE__;\
	/*if( IsDebuggerPresent() ) __debugbreak();*/\
	return CameraStreamError::X;
#else
#define STREAM_ERROR( X )\
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
		FreeQueuedPackets();
	}

	void FreeQueuedPackets()
	{
		//Can delete the old data now
		for (auto& PrevPacket : PacketsSinceKeyframe)
		{
			av_packet_unref( &PrevPacket );
		}
		PacketsSinceKeyframe.clear();
	}

	std::unique_ptr<FFMPEG::Frame>	Input;
	std::unique_ptr<FFMPEG::Frame>	Output;

	std::vector<AVPacket>			PacketsSinceKeyframe;

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
