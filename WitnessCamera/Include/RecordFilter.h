#pragma once

#include "Export.h"
#include "Pimpl.h"

#include <opencv2/core/mat.hpp>

#include <memory>
#include <type_traits>

namespace Witness{
namespace Camera{

struct FilterData;
class StreamManager;

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
	
	virtual void FilterFrame( ClassificationResult& Result, cv::Mat& InputFrame, cv::Mat& GrayscaleInputFrame ) = 0;
};

}}
