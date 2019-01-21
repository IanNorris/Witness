#pragma once

#include "Export.h"
#include "Pimpl.h"
#include "FFMPEG/Frame.h"

#include "SourceStats.h"

#include <opencv2/core/mat.hpp>

#include <memory>
#include <type_traits>
#include <memory>

struct AVFrame;
struct SwsContext;

namespace Witness{
namespace Camera{

struct FilterData;
class StreamManager;

class CAMERA_API FilterFrame
{
public:

	FilterFrame( 
		FilterFrameStats& StatsIn, 
		std::shared_ptr<FFMPEG::Frame>& InputFrameIn,
		std::shared_ptr<FFMPEG::Frame>& OutputFrameIn,
		cv::Mat& DecodedFrameIn,
		cv::Mat& GrayscaleDecodedFrameIn,
		SwsContext*& ConversionContextIn )
	: Stats( StatsIn )
	, InputFrame( InputFrameIn )
	, WantFullSizeOutput( false )
	, WantSmallOutput( false )
	, OutputFrame( OutputFrameIn )
	, DecodedFrame( DecodedFrameIn )
	, GrayscaleDecodedFrame( GrayscaleDecodedFrameIn )
	, ConversionContext( ConversionContextIn )
	{}

	cv::Mat& GetOrDecodeFrame();
	cv::Mat& GetOrDecodeGrayscaleInputFrame();

	FilterFrameStats& Stats;

	std::shared_ptr<FFMPEG::Frame>& InputFrame;

	bool WantFullSizeOutput;
	bool WantSmallOutput;

private:

	std::shared_ptr<FFMPEG::Frame>& OutputFrame;

	cv::Mat& DecodedFrame;
	cv::Mat& GrayscaleDecodedFrame;

	SwsContext*& ConversionContext;
};

class FilterFrameOwner
{
public:

	FilterFrameOwner( const std::shared_ptr<FFMPEG::Frame>& InputFrameIn, SwsContext* ConversionContextIn )
	: InputFrame( InputFrameIn )
	, ConversionContext( ConversionContextIn )
	{}

	FilterFrame GetFilterFrame()
	{
		return FilterFrame( Stats, InputFrame, OutputFrame, DecodedFrame, GrayscaleDecodedFrame, ConversionContext );
	}

	std::shared_ptr<FFMPEG::Frame> InputFrame;
	std::shared_ptr<FFMPEG::Frame> OutputFrame;

	cv::Mat DecodedFrame;
	cv::Mat GrayscaleDecodedFrame;

	FilterFrameStats Stats;

	SwsContext* ConversionContext;
};

struct ClassificationResult
{
	enum ClassificationFlag
	{
		Motion_None					= 0,
		Motion_Motion				= 1 << 0,

		Motion_Animal				= 1 << 1,
		Motion_Animal_Cat			= 1 << 2,
		Motion_Animal_Dog			= 1 << 3,
		Motion_Animal_Other			= 1 << 4,

		Motion_Vehicle				= 1 << 8,
		Motion_Vehicle_Unrecognized	= 1 << 9,
		Motion_Vehicle_Recognized	= 1 << 10,

		Motion_Person				= 1 << 15,
		Motion_Person_Unrecognized	= 1 << 16,
		Motion_Person_Recognized	= 1 << 17,
		Motion_Person_HighRisk		= 1 << 18,
	};

	struct RegionOfInterest
	{
		RegionOfInterest()
		: Left( 0 )
		, Top( 0 )
		, Width( 0 )
		, Height( 0 )
		, Classification( 0 )
		, ClassificationGroup( 0 )
		{}

		unsigned int Left;
		unsigned int Top;
		unsigned int Width;
		unsigned int Height;

		unsigned int Classification;
		unsigned int ClassificationGroup;
	};
	
	ClassificationResult()
	: ClassificationSuperset( Motion_None )
	, MotionAmount( 0.0f )
	{}

	std::vector<RegionOfInterest> ROI;

	unsigned int ClassificationSuperset;
	float MotionAmount;
};

class CAMERA_API IRecordFilter
{
public:

	virtual ~IRecordFilter(){}
	
	virtual void ClassifyFrame( FilterFrame& Frame, ClassificationResult& Result ) = 0;
	virtual void ClearState() {};
};

}}
