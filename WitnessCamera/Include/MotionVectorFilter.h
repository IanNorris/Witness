#pragma once

#include "RecordFilterBase.h"

namespace Witness{
namespace Camera{

struct MotionVectorFilterData;

class CAMERA_API MotionVectorFilter : public RecordFilterBase<MotionVectorFilterData>
{
public:

	MotionVectorFilter( const wchar_t* BlackoutMaskPath, const wchar_t* FocusMaskPath );
	virtual ~MotionVectorFilter();

	void UpdateMasks( unsigned int Width, unsigned int Height );

	virtual void FilterFrame( const AVFrame* Frame, ClassificationResult& Result, cv::Mat& InputFrame, cv::Mat& GrayscaleInputFrame );
	virtual void ClearState() override;
};

}}
