#pragma once

#include "RecordFilterBase.h"

namespace Witness{
namespace Camera{

struct PersonRecognitionFilterData;

class CAMERA_API PersonRecognitionFilter : public RecordFilterBase<PersonRecognitionFilterData>
{
public:

	PersonRecognitionFilter( const char* FaceCascadeDataFilename, const char* FullBodyCascadeDataFilename );
	virtual ~PersonRecognitionFilter();

	virtual void FilterFrame( const AVFrame* Frame, ClassificationResult& Result, cv::Mat& InputFrame, cv::Mat& GrayscaleInputFrame );
};

}}
