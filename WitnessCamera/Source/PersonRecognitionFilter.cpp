#include "PersonRecognitionFilter.h"
#include "StreamManager.h"
#include "OutputStream.h"
#include "FFMPEG/Frame.h"

#include <opencv2/core/core.hpp>
#include <opencv2/objdetect/objdetect_c.h>

#include <opencv2/face.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/video.hpp>
#include <opencv2/videoio.hpp>

#include <string.h>

#pragma warning(push)
#pragma warning(disable:4099)
#pragma warning(disable:4267)
#include "package_bgs/bgslibrary.h"
#pragma warning(pop)

#include "FilterData.h"

using namespace cv;
using namespace std;

namespace Witness{
namespace Camera{

using namespace cv::face;

struct PersonRecognitionFilterData : public FilterDataBase
{
	PersonRecognitionFilterData()
	: Frame( 0 )
	{}

	CascadeClassifier FaceCascade;
	CascadeClassifier BodyCascade;

	int Frame;
};

PIMPL_CONSTRUCT(PersonRecognitionFilterData)

PersonRecognitionFilter::PersonRecognitionFilter( const char* FaceCascadeDataFilename, const char* FullBodyCascadeDataFilename )
{
	auto& ID = GetData();

	if (!ID.FaceCascade.load(FaceCascadeDataFilename))
	{
		printf( "Unable to load face cascade file: %s.\n", FaceCascadeDataFilename );
	}

	if (!ID.BodyCascade.load(FullBodyCascadeDataFilename))
	{
		printf( "Unable to load body cascade file: %s.\n", FullBodyCascadeDataFilename );
	}
}

PersonRecognitionFilter::~PersonRecognitionFilter()
{}

void PersonRecognitionFilter::FilterFrame( const AVFrame* Frame, ClassificationResult& Result, cv::Mat& InputFrame, cv::Mat& GrayscaleInputFrame )
{
	auto& ID = GetData();

	//Alternate between body and face recognition
	//bool ChooseFace = (ID.Frame % 2) == 0;
	bool ChooseFace = true;
	++ID.Frame;
	
	std::vector<cv::Rect> faces;

	if( ChooseFace )
	{
		ID.FaceCascade.detectMultiScale( GrayscaleInputFrame, faces );
	}
	else
	{
		ID.BodyCascade.detectMultiScale( GrayscaleInputFrame, faces );
	}
	
	for(int j=0;j<faces.size();j++){
		cv::rectangle(InputFrame, faces[j], cv::Scalar(255,0,255), 4);

		ClassificationResult::RegionOfInterest ROI;
		ROI.Classification = ClassificationResult::Motion_Person;
		ROI.ClassificationGroup = 0;
		ROI.Left = faces[j].x;
		ROI.Top = faces[j].y;
		ROI.Width = faces[j].width;
		ROI.Height = faces[j].height;
		Result.ROI.push_back( ROI );
	}

	if( faces.size() >= 1 )
	{
		printf("%d %s detected.\n", (int)faces.size(), ChooseFace ? "face(s)" : "body(s)" );

		Result.ClassificationSuperset |= ClassificationResult::Motion_Person;
	}
}

}}
