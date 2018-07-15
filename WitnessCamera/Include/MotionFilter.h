#pragma once

#include "RecordFilterBase.h"

namespace Witness{
namespace Camera{

struct MotionFilterData;

class CAMERA_API MotionFilter : public RecordFilterBase<MotionFilterData>
{
public:

	MotionFilter( double MotionThreshold, const char* FilterName );
	virtual ~MotionFilter();

	virtual void FilterFrame( ClassificationResult& Result, cv::Mat& InputFrame, cv::Mat& GrayscaleInputFrame );

private:

	double MotionThreshold;
};

}}
