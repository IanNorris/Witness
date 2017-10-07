#include "MotionFilter.h"

#include <opencv2/core/core.hpp>           // cv::Mat
#include <opencv2/imgproc/imgproc.hpp>     // cv::Canny()

#include "FilterData.h"

namespace Witness{
namespace Camera{

struct MotionFilterData : public FilterDataBase
{};

PIMPL_CONSTRUCT(MotionFilterData)
FILTER_BASE_CONSTRUCT(MotionFilterData)

MotionFilter::MotionFilter()
{}

MotionFilter::~MotionFilter()
{}

const char* MotionFilter::CalculateRecordingClassification( unsigned int Width, unsigned int Height, void* Data )
{
	cv::Mat img( cv::Size( Width, Height ), CV_8UC3, Data );

	cv::Mat out;
	cv::Canny( img, out, 30, 300);

	//unsigned char* ViewData = (unsigned char*)ID.Output->GetFrame()->data[0];

	return nullptr;
}

}}
