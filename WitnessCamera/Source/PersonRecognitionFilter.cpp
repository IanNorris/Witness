#include "PersonRecognitionFilter.h"
#include "StreamManager.h"
#include "OutputStream.h"
#include "FFMPEG/Frame.h"

#include <opencv2/core/core.hpp>
#include <opencv2/objdetect.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>

#include <string.h>

#include <Log.h>
#include "FilterData.h"

using namespace cv;

namespace Witness{
namespace Camera{

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

PersonRecognitionFilter::PersonRecognitionFilter( const MotionChainNode& Chain, const char* FaceCascadeDataFilename, const char* FullBodyCascadeDataFilename )
: RecordFilterBase( Chain )
{
	auto& ID = GetData();

	if (!ID.FaceCascade.load(FaceCascadeDataFilename))
	{
			LOG_ERROR( "Unable to load face cascade file: %s.", FaceCascadeDataFilename );
	}

	if (!ID.BodyCascade.load(FullBodyCascadeDataFilename))
	{
			LOG_ERROR( "Unable to load body cascade file: %s.", FullBodyCascadeDataFilename );
	}
}

PersonRecognitionFilter::~PersonRecognitionFilter()
{}

bool PersonRecognitionFilter::ProcessFrame( SharedClassificationTask TaskData )
{
	auto& ID = GetData();

	//Alternate between body and face recognition
	//bool ChooseFace = (ID.Frame % 2) == 0;
	bool ChooseFace = true;
	++ID.Frame;
	
	std::vector<cv::Rect> faces;

	if( ChooseFace )
	{
		ID.FaceCascade.detectMultiScale( TaskData->Frame.GetOrDecodeGrayscaleInputFrame(), faces );
	}
	else
	{
		ID.BodyCascade.detectMultiScale( TaskData->Frame.GetOrDecodeGrayscaleInputFrame(), faces );
	}
	
	for(int j=0;j<faces.size();j++){
		cv::rectangle( TaskData->Frame.GetOrDecodeFrame(), faces[j], cv::Scalar(255,0,255), 4);

		ClassificationResult::RegionOfInterest ROI;
		ROI.Classification = ClassificationResult::Motion_Person;
		ROI.ClassificationGroup = 0;
		ROI.Left = faces[j].x;
		ROI.Top = faces[j].y;
		ROI.Width = faces[j].width;
		ROI.Height = faces[j].height;
		TaskData->Result.ROI.push_back( ROI );
	}

	if( faces.size() >= 1 )
	{
			LOG_DEBUG("%d %s detected.", (int)faces.size(), ChooseFace ? "face(s)" : "body(s)" );

		TaskData->Result.ClassificationSuperset |= ClassificationResult::Motion_Person;

		return true;
	}

	return false;
}

}}
