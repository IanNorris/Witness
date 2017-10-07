#pragma once

#include "FFMPEG/Common.h"
#include "FFMPEG/Frame.h"

#include <memory>

#define STREAM_ERROR( X )\
	m_LineNumber = __LINE__;\
	return CameraStreamError::X;

namespace Witness{
namespace Camera{

struct StreamData
{
	StreamData()
	: ConversionContext( nullptr )
	, FormatContext( nullptr )
	, CodecContext( nullptr )
	, StreamOptions( nullptr )
	, StreamIndex( 0 )
	, ChosenStreamIndex( 0 )
	, HasInitialized( false )
	{}

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

	unsigned int		StreamIndex;
	unsigned int		ChosenStreamIndex;

	bool				HasInitialized;
	bool				HasFinalised;
};

}}
