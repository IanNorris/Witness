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
FILTER_BASE_CONSTRUCT(PersonRecognitionFilterData)

PersonRecognitionFilter::PersonRecognitionFilter( const char* FaceCascadeDataFilename, const char* FullBodyCascadeDataFilename, const char* FilterName )
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

ClassificationResult PersonRecognitionFilter::FilterFrame( unsigned int Width, unsigned int Height, void* Data, StreamManager* StreamManager )
{
	auto& ID = GetData();

	//Alternate between body and face recognition
	bool ChooseFace = (ID.Frame % 2) == 0;
	++ID.Frame;

	Mat InputFrame( Size( Width, Height ), CV_8UC3, Data );

	Mat GrayscaleFrame;
	cvtColor( InputFrame, GrayscaleFrame, CV_RGB2GRAY );

	std::vector<cv::Rect> faces;

	if( ChooseFace )
	{
		ID.FaceCascade.detectMultiScale( GrayscaleFrame, faces );
	}
	else
	{
		ID.BodyCascade.detectMultiScale( GrayscaleFrame, faces );
	}
	


	for(int j=0;j<faces.size();j++){
		cv::rectangle(InputFrame, faces[j], cv::Scalar(255,0,255));
	}

	if( faces.size() >= 1 )
	{
		printf("%d %s detected.\n", (int)faces.size(), ChooseFace ? "face(s)" : "body(s)" );
	}

	return ClassificationResult( ClassificationImportance::Person_Unknown, "Person", 1.0 );
}

}}
