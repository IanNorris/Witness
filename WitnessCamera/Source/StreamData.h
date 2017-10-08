#pragma once

#include "FFMPEG/Common.h"
#include "FFMPEG/Frame.h"

#include <memory>

#if defined _WIN32
#include <windows.h>
#define STREAM_ERROR( X )\
	m_LineNumber = __LINE__;\
	if( IsDebuggerPresent() ) __debugbreak();\
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
	, HasOneTimeInitialized( false )
	, HasInitialized( false )
	, HasFinalised( false )
	{
		AspectRatio.den = 0;
		AspectRatio.num = 0;

		Framerate.den = 0;
		Framerate.num = 0;
	}

	~StreamData()
	{}

	std::unique_ptr<FFMPEG::Frame>	Input;
	std::unique_ptr<FFMPEG::Frame>	Output;

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
	bool				HasOneTimeInitialized;
	bool				HasInitialized;
	bool				HasFinalised;
};

}}
