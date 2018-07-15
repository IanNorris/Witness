#include "MotionFilter.h"
#include "StreamManager.h"
#include "OutputStream.h"
#include "FFMPEG/Frame.h"

#include <opencv2/core/core.hpp>           // cv::Mat

#include <opencv2/objdetect.hpp>
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

struct MotionFilterData : public FilterDataBase
{
	const int IgnoreInitialFrames = 10;

	MotionFilterData()
	: BackgroundFilter( nullptr )
	, InitialFrameFilter( IgnoreInitialFrames )
	{}

	void CreateFilter( const char* Name );

	vector<vector<Point>> Contours;
	vector<Point> ContoursPoly;
	shared_ptr<FFMPEG::Frame> DiagFrame;

	unique_ptr<IBGS> BackgroundFilter;
	Mat ForegroundMask;
	Mat ForegroundMask1Channel;
	Mat Background;
	Mat PreviousMask;

	int InitialFrameFilter;
};

PIMPL_CONSTRUCT(MotionFilterData)

void MotionFilterData::CreateFilter( const char* Name )
{
#define CREATE_FILTER( N, Prefix )			\
		if( _stricmp( Name, Prefix #N ) == 0 )	\
		{ BackgroundFilter = make_unique<N>(); return; }

	CREATE_FILTER( FrameDifference, "BGS_" )
	CREATE_FILTER( StaticFrameDifference, "BGS_" )
	CREATE_FILTER( WeightedMovingMean, "BGS_" )
	CREATE_FILTER( WeightedMovingVariance, "BGS_" )
	CREATE_FILTER( MixtureOfGaussianV2, "BGS_" )
	CREATE_FILTER( AdaptiveBackgroundLearning, "BGS_" )
	CREATE_FILTER( AdaptiveSelectiveBackgroundLearning, "BGS_" )
	CREATE_FILTER( KNN, "BGS_" )
	CREATE_FILTER( DPAdaptiveMedian, "BGS_" )
	CREATE_FILTER( DPGrimsonGMM, "BGS_" )
	CREATE_FILTER( DPZivkovicAGMM, "BGS_" ) 
	CREATE_FILTER( DPMean, "BGS_" )
	CREATE_FILTER( DPWrenGA, "BGS_" )
	CREATE_FILTER( DPPratiMediod, "BGS_" )
	CREATE_FILTER( DPEigenbackground, "BGS_" )
	CREATE_FILTER( DPTexture, "BGS_" )
	CREATE_FILTER( T2FGMM_UM, "BGS_" )
	CREATE_FILTER( T2FGMM_UV, "BGS_" )
	CREATE_FILTER( T2FMRF_UM, "BGS_" )
	CREATE_FILTER( T2FMRF_UV, "BGS_" )
	CREATE_FILTER( FuzzySugenoIntegral, "BGS_" )
	CREATE_FILTER( FuzzyChoquetIntegral, "BGS_" )
	CREATE_FILTER( MultiLayer, "BGS_" )
	CREATE_FILTER( PixelBasedAdaptiveSegmenter, "BGS_" )
	CREATE_FILTER( LBSimpleGaussian, "BGS_" )
	CREATE_FILTER( LBFuzzyGaussian, "BGS_" )
	CREATE_FILTER( LBMixtureOfGaussians, "BGS_" )
	CREATE_FILTER( LBAdaptiveSOM, "BGS_" )
	CREATE_FILTER( LBFuzzyAdaptiveSOM, "BGS_" )
	CREATE_FILTER( LBP_MRF, "BGS_" )
	CREATE_FILTER( VuMeter, "BGS_" )
	CREATE_FILTER( KDE, "BGS_" )
	CREATE_FILTER( IndependentMultimodal, "BGS_" )
	CREATE_FILTER( MultiCue, "BGS_" )
	CREATE_FILTER( SigmaDelta, "BGS_" )
	CREATE_FILTER( SuBSENSE, "BGS_" )
	CREATE_FILTER( LOBSTER, "BGS_" )
	CREATE_FILTER( PAWCS, "BGS_" )
	CREATE_FILTER( TwoPoints, "BGS_" )
	CREATE_FILTER( ViBe, "BGS_" )
	CREATE_FILTER( CodeBook, "BGS_" )

	printf( "Unable to create filter for %s\n", Name );

#undef CREATE_FILTER
}

MotionFilter::MotionFilter( double MotionThreshold, const char* FilterName )
: MotionThreshold( MotionThreshold )
{
 auto& ID = GetData();
 ID.CreateFilter( FilterName );
}

MotionFilter::~MotionFilter()
{}

void MotionFilter::FilterFrame( ClassificationResult& Result, cv::Mat& InputFrame, cv::Mat& GrayscaleInputFrame )
{
	auto& ID = GetData();

	if (!ID.BackgroundFilter)
	{
		return;
	}

	/*if( !ID.DiagFrame )
	{
		ID.DiagFrame = make_shared<FFMPEG::Frame>( Width, Height, AV_PIX_FMT_BGR24, 1 );
		ID.DiagFrame->Prepare();
	}*/
	
	ID.BackgroundFilter->process( InputFrame, ID.ForegroundMask, ID.Background );

	if( ID.InitialFrameFilter > 0 )
	{
		ID.InitialFrameFilter--;
		return;
	}

	/*(if( ID.PreviousMask.rows != Height && ID.PreviousMask.cols!= Width )
	{
		ID.PreviousMask = Mat( ID.DiagFrame->GetHeight(), ID.DiagFrame->GetWidth(), CV_8UC1 );
		ID.PreviousMask = Scalar(0,0,0);
	}

	float PrevAlpha = 0.4f;
	addWeighted( ID.PreviousMask, PrevAlpha, ID.ForegroundMask, 1.0f, 0, ID.PreviousMask );*/

	int SumResult = 0;

	if (ID.ForegroundMask.channels() > 1)
	{
		cv::extractChannel( ID.ForegroundMask, ID.ForegroundMask1Channel, 0 );
		SumResult = countNonZero( ID.ForegroundMask1Channel );
	}
	else
	{
		SumResult = countNonZero( ID.ForegroundMask );
	}

	

	double ComparisonResult = InputFrame.cols * InputFrame.rows;

	double Percentage = (double)SumResult / ComparisonResult;

	if( Percentage > MotionThreshold )
	{
		//OutputStream* DiagOutput = StreamManager->GetDiagnosticStream( Width, Height );

		//ID.DiagFrame->Prepare();
		
		//Mat annotatedFinal( ID.DiagFrame->GetHeight(), ID.DiagFrame->GetWidth(), CV_8UC3, ID.DiagFrame->GetFrame()->data[0] );
		//Mat annotated( ID.DiagFrame->GetHeight(), ID.DiagFrame->GetWidth(), CV_8UC3 );

		//annotatedFinal = Scalar(0,0,0);
		//InputFrame.copyTo( annotatedFinal, ID.PreviousMask );

		//ID.ForegroundMask.copyTo( annotatedFinal );

		/*Mat overlayColour( ID.DiagFrame->GetHeight(), ID.DiagFrame->GetWidth(), CV_8UC3 );
		overlayColour = Scalar(0,255,0);

		Mat colouredMask;
		overlayColour.copyTo( colouredMask, previousMaskTrail );

		Mat colouredMaskBlurred;
		GaussianBlur( colouredMask, colouredMaskBlurred, Size(9,9), 0, 0 );

		float alpha = 0.05f;
		add( InputFrame, colouredMaskBlurred, annotatedFinal );*/


		/*ID.Contours.clear();
		findContours( ID.PreviousMask, ID.Contours, CV_RETR_EXTERNAL, CV_CHAIN_APPROX_SIMPLE  );

		for( auto& Contour : ID.Contours )
		{
			ID.ContoursPoly.clear();
			approxPolyDP( Mat(Contour), ID.ContoursPoly, Height/10, true );
			Rect Bounds = boundingRect( ID.ContoursPoly );

			if( Bounds.area() > Threshold )
			{
				rectangle( annotatedFinal, Bounds, Scalar(0,255,0), 1 );
			}
		}*/

		//add( InputFrame, annotated, annotatedFinal );


		//ID.ForegroundMask.copyTo( ID.PreviousMask );


		//DiagOutput->WriteFrame( ID.DiagFrame.get() );

		Result.ClassificationSuperset |= ClassificationResult::Motion_Motion;
		Result.MotionAmount = (float)Percentage;
	}

	

	/*cv::Mat img( cv::Size( Width, Height ), CV_8UC3, Data );

	cv::Mat imgGrey;

	int maxCorners = 10;

	ID.Corners.clear();

	cvtColor( img, imgGrey, COLOR_BGR2GRAY );

	goodFeaturesToTrack( imgGrey, ID.Corners, ID.MaxCorners, 0.01, 10, Mat(), 3, false, 0.04 );

	bool Found = false;

	if( !ID.FirstFrame )
	{
		if( ID.Corners.size() == ID.PreviousCorners.size() )
		{
			for( int CornerIndex = 0; CornerIndex < ID.Corners.size(); CornerIndex++ )
			{
				auto& New = ID.Corners[CornerIndex];
				auto& Old = ID.PreviousCorners[CornerIndex];

				cv::absdiff( ID.Corners, ID.PreviousCorners,  )
			}
		}
		else
		{
			Found = true;
		}
	}

	ID.FirstFrame = false;
	ID.PreviousCorners = ID.Corners;

	Mat copy;
	copy = img.clone();

	 int r = 4;
	for( size_t i = 0; i < ID.Corners.size(); i++ )
	 { circle( copy, ID.Corners[i], r, Scalar(0, 255,0), -1, 8, 0 ); }

	cv::Mat out;*/

	/* std::vector<cv::Rect> found, found_filtered;
	
	cv::HOGDescriptor hog;
	hog.setSVMDetector(cv::HOGDescriptor::getDefaultPeopleDetector());

	hog.detectMultiScale(img, found, 0, cv::Size(8,8), cv::Size(32,32), 1.05, 2);

	for(size_t i = 0; i < found.size(); i++ )
	{
		Rect r = found[i];

		size_t j;
		// Do not add small detections inside a bigger detection.
		for ( j = 0; j < found.size(); j++ )
			if ( j != i && (r & found[j]) == r )
				break;

		if ( j == found.size() )
			found_filtered.push_back(r);
	}

	for (size_t i = 0; i < found_filtered.size(); i++)
	{
		Rect r = found_filtered[i];

		// The HOG detector returns slightly larger rectangles than the real objects,
		// so we slightly shrink the rectangles to get a nicer output.
		r.x += cvRound(r.width*0.1);
		r.width = cvRound(r.width*0.8);
		r.y += cvRound(r.height*0.07);
		r.height = cvRound(r.height*0.8);
		rectangle(img, r.tl(), r.br(), cv::Scalar(0,255,0), 3);
	}*/
}

}}
