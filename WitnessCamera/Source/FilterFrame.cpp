#include "OutputStream.h"
#include "FFMPEG/Common.h"
#include "FFMPEG/Frame.h"
#include "RecordFilter.h"

#include <libavformat/avformat.h>

#include <opencv2/core/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/imgproc/imgproc_c.h>

namespace Witness{
namespace Camera{

cv::Mat& FilterFrame::GetOrDecodeFrame()
{
	FilterFrameStatScope Scope( Stats, FilterStat_Scale );

	if( DecodedFrame.rows > 0 )
	{
		return DecodedFrame;
	}

	AVPixelFormat OutputPixelFormat = AV_PIX_FMT_BGR24;

	unsigned int OutputWidth = InputFrame->GetWidth();
	unsigned int OutputHeight = InputFrame->GetHeight();

	AVPixelFormat InputPixelFormat = (AVPixelFormat)InputFrame->GetFormat();

	//Remap deprecated formats to avoid the warning output.
	switch(InputPixelFormat)
	{
	case AV_PIX_FMT_YUVJ420P:
		InputPixelFormat = AV_PIX_FMT_YUV420P;
		break;

	case AV_PIX_FMT_YUVJ422P:
		InputPixelFormat = AV_PIX_FMT_YUV422P;
		break;

	case AV_PIX_FMT_YUVJ444P:
		InputPixelFormat = AV_PIX_FMT_YUV444P;
		break;

	case AV_PIX_FMT_YUVJ440P:
		InputPixelFormat = AV_PIX_FMT_YUV440P;
		break;
	}


	OutputFrame = std::make_shared<FFMPEG::Frame>( OutputWidth, OutputHeight, OutputPixelFormat );
	OutputFrame->Prepare();

	bool SameDimensions = OutputWidth == InputFrame->GetWidth() && OutputHeight == InputFrame->GetHeight();

	//Input and output are the same
	int ScaleMethod = SameDimensions ? SWS_POINT : SWS_BILINEAR;

	ConversionContext = sws_getCachedContext(
		ConversionContext,
		InputFrame->GetWidth(),
		InputFrame->GetHeight(),
		InputPixelFormat,
		OutputWidth,
		OutputHeight,
		OutputPixelFormat,
		ScaleMethod,
		NULL,
		NULL,
		NULL );

	sws_scale( ConversionContext, InputFrame->GetFrame()->data, InputFrame->GetFrame()->linesize, 0, InputFrame->GetHeight(), OutputFrame->GetFrame()->data, OutputFrame->GetFrame()->linesize );

	DecodedFrame = cv::Mat( cv::Size( OutputWidth, OutputHeight ), CV_8UC3, OutputFrame->GetFrame()->data[0] );

	return DecodedFrame;
}

cv::Mat& FilterFrame::GetOrDecodeGrayscaleInputFrame()
{
	if( GrayscaleDecodedFrame.rows > 0 )
	{
		return GrayscaleDecodedFrame;
	}

	cv::Mat& RegularFrame = GetOrDecodeFrame();
	
	FilterFrameStatScope Scope( Stats, FilterStat_Scale );

	cvtColor( RegularFrame, GrayscaleDecodedFrame, CV_RGB2GRAY );

	return GrayscaleDecodedFrame;
}

}
}