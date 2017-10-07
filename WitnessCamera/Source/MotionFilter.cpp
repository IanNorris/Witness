#include "MotionFilter.h"

#include <opencv2/core/core.hpp>           // cv::Mat

#include <opencv2/objdetect.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/video.hpp>
#include <opencv2/videoio.hpp>

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
	{	
		//BackgroundFilter = new FrameDifference;
		//BackgroundFilter = new StaticFrameDifference;
		//BackgroundFilter = new WeightedMovingMean;
		//BackgroundFilter = new WeightedMovingVariance;
		//BackgroundFilter = new MixtureOfGaussianV1; // only on OpenCV 2.x
		//BackgroundFilter = new MixtureOfGaussianV2;
		//BackgroundFilter = new AdaptiveBackgroundLearning;
		//BackgroundFilter = new AdaptiveSelectiveBackgroundLearning;
		//BackgroundFilter = new GMG; // only on OpenCV 2.x
		//BackgroundFilter = new KNN; // only on OpenCV 3.x
		//BackgroundFilter = new DPAdaptiveMedian;
		//BackgroundFilter = new DPGrimsonGMM;
		//BackgroundFilter = new DPZivkovicAGMM;
		//BackgroundFilter = new DPMean;
		//BackgroundFilter = new DPWrenGA;
		//BackgroundFilter = new DPPratiMediod;
		//BackgroundFilter = new DPEigenbackground;
		//BackgroundFilter = new DPTexture;
		//BackgroundFilter = new T2FGMM_UM;
		//BackgroundFilter = new T2FGMM_UV;
		//BackgroundFilter = new T2FMRF_UM;
		//BackgroundFilter = new T2FMRF_UV;
		//BackgroundFilter = new FuzzySugenoIntegral;
		//BackgroundFilter = new FuzzyChoquetIntegral;
		//BackgroundFilter = new MultiLayer;
		//BackgroundFilter = new PixelBasedAdaptiveSegmenter;
		//BackgroundFilter = new LBSimpleGaussian;
		//BackgroundFilter = new LBFuzzyGaussian;
		//BackgroundFilter = new LBMixtureOfGaussians;
		//BackgroundFilter = new LBAdaptiveSOM;
		//BackgroundFilter = new LBFuzzyAdaptiveSOM;
		//BackgroundFilter = new LBP_MRF;
		BackgroundFilter = new VuMeter;
		//BackgroundFilter = new KDE;
		//BackgroundFilter = new IndependentMultimodal;
		//BackgroundFilter = new MultiCue;
		//BackgroundFilter = new SigmaDelta;
		//BackgroundFilter = new SuBSENSE;
		//BackgroundFilter = new LOBSTER;
		//BackgroundFilter = new PAWCS;
		//BackgroundFilter = new TwoPoints;
		//BackgroundFilter = new ViBe;
		//BackgroundFilter = new CodeBook;
	}

	vector<vector<Point>> Contours;
	vector<Point> ContoursPoly;

	IBGS* BackgroundFilter;
	Mat ForegroundMask;
	Mat Background;

	int InitialFrameFilter;
};

PIMPL_CONSTRUCT(MotionFilterData)
FILTER_BASE_CONSTRUCT(MotionFilterData)

MotionFilter::MotionFilter()
{}

MotionFilter::~MotionFilter()
{}

const char* MotionFilter::CalculateRecordingClassification( unsigned int Width, unsigned int Height, void* Data )
{
	auto& ID = GetData();

	Mat InputFrame( Size( Width, Height ), CV_8UC3, Data );

	ID.BackgroundFilter->process( InputFrame, ID.ForegroundMask, ID.Background );

	if( ID.InitialFrameFilter > 0 )
	{
		ID.InitialFrameFilter--;
		return nullptr;
	}

	int SumResult = countNonZero( ID.ForegroundMask );

	double ComparisonResult = Width * Height;
	double Threshold = 0.025 * ComparisonResult;
	double BBThreshold = 0.025 * ComparisonResult;

	if( SumResult > Threshold )
	{
		Mat annotated;
		InputFrame.copyTo( annotated );

		ID.Contours.clear();
		findContours( ID.ForegroundMask, ID.Contours, CV_RETR_EXTERNAL, CV_CHAIN_APPROX_SIMPLE  );

		for( auto& Contour : ID.Contours )
		{
			ID.ContoursPoly.clear();
			approxPolyDP( Mat(Contour), ID.ContoursPoly, Height/20, true );
			Rect Bounds = boundingRect( ID.ContoursPoly );

			if( Bounds.area() > BBThreshold )
			{
				rectangle( annotated, Bounds, Scalar(0,255,0), 1 );
			}
		}
		return "Motion";
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
	}


	return found_filtered.empty() ? nullptr : "People";*/

	/*if( Found )
	{
		return "Motion";
	}
	else*/
	{
		return nullptr;
	}
}

}}
