#pragma once

extern "C"
{
#include <libavcodec\avcodec.h>
#include <libavformat\avformat.h>
#include <libavformat\avio.h>
#include <libavutil\dict.h>
#include <libavutil\imgutils.h>
#include <libswscale\swscale.h>
}

namespace Witness{
namespace Camera{
namespace Internal{

struct StreamData
{
	StreamData()
	: ConversionContext( nullptr )
	, FormatContext( nullptr )
	, CodecContext( nullptr )
	, StreamOptions( nullptr )
	{
	}

	~StreamData()
	{
		avformat_free_context(FormatContext);
		FormatContext = nullptr;
	}

	SwsContext* ConversionContext;
	AVFormatContext* FormatContext;
	AVCodecContext* CodecContext;
	AVDictionary* StreamOptions;
};

}}}