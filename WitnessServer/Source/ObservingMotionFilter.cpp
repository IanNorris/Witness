#include "ObservingMotionFilter.h"

#include "MessageBus.h"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgcodecs/imgcodecs_c.h>
#include <opencv2/core/core_c.h>
#include <opencv2/imgproc.hpp>
#include <opencv2/imgproc/imgproc_c.h>

const int ClipEndGracePeriodInSeconds = 10;
const int TargetLargeThumbnailSize = 1280;
const int TargetThumbnailSize = 300;
const double PreviewTimeout = 0.5;

ObservingMotionFilter::ObservingMotionFilter( const shared_ptr<MotionChainNode>& MotionChain, const int CameraID, const shared_ptr<MessageBus>& MessageBusIn )
: MotionChain( MotionChain )
, MessageBusPtr( MessageBusIn )
, LastLargePreviewTimestamp(0)
, LastSmallPreviewTimestamp(0)
, CameraID( CameraID )
, FrameIndex( 0 )
, LastMotionIndex( INT_MIN )
, SaveNextFrame( false )
, State( MotionState::None )
{
}

ObservingMotionFilter::~ObservingMotionFilter()
{}

void ObservingMotionFilter::ClassifyFrame( FilterFrame& Frame, ClassificationResult& Result )
{
	FrameIndex++;

	const int DefaultQuality = 70;
	const double NanoSecondsToSeconds = 1000.0 * 1000.0 * 1000.0;
	uint64_t Now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
	bool SaveLarge = ((double)(Now - LastLargePreviewTimestamp) / NanoSecondsToSeconds) < PreviewTimeout;
	bool SaveSmall = ((double)(Now - LastSmallPreviewTimestamp) / NanoSecondsToSeconds) < PreviewTimeout;

	SaveNextFrame = SaveLarge | SaveSmall;

	Frame.WantFullSizeOutput |= SaveLarge;
	Frame.WantSmallOutput |= SaveSmall;

	int FilterIndex = 0;

	try 
	{
		MotionChainNode* Next = MotionChain.get();
		while (Next)
		{
			unsigned int ClassificationSuperset = Result.ClassificationSuperset;

			{
				FilterStat Stat = (FilterStat)min<int>( FilterStat_ThirdPassFilter, FilterStat_FirstPassFilter + FilterIndex );
				FilterFrameStatScope Scope( Frame.Stats, Stat );

				Next->Filter->ClassifyFrame( Frame, Result );
			}

			if (	(Result.ClassificationSuperset & Next->InclusiveFilter) != 0
				&&	(Result.ClassificationSuperset & Next->ExclusiveFilter) == 0
				&&	Result.MotionAmount >= Next->MinimumThreshold )
			{
				Next = Next->OnSuccess.get();
			}
			else
			{
				Result.ClassificationSuperset = ClassificationSuperset;
				Next = Next->OnFailure.get();
			}

			FilterIndex++;
		}
	}
	catch (cv::Exception& e)
	{
		printf("OpenCV error: %s\n", e.what());
		abort();
	}

	FilterFrameStatScope Scope( Frame.Stats, FilterStat_ObserverFilter );
	
	uint64_t TimestampNow = datetime::utc_timestamp();

	if (Result.ClassificationSuperset )
	{
		if( State == MotionState::None )
		{
			State = MotionState::Current;

			auto MotionMessage = make_shared<CameraBeginMotionMessage>( CameraID );

			ClipStats.TimestampClipStarted = TimestampNow;
			ClipStats.TimestampMotionStarted = TimestampNow;
			ClipStats.TimestampMotionEnded = INT64_MIN;
			ClipStats.TimestampClipEnded = INT64_MIN;
			ClipStats.LargestMotionDelta = MotionMessage->MotionPercentage = Result.MotionAmount;

			MotionMessage->ClipStats = ClipStats;

			CreateJpegPreview( Frame, MotionMessage->Jpeg, TargetThumbnailSize, DefaultQuality, nullptr );

			MessageBusPtr->SendToClient( nullptr, MotionMessage );
		}
		else
		{
			if( State == MotionState::GracePeriod )
			{
				State = MotionState::Current;
				ClipStats.TimestampMotionEnded = TimestampNow;
				ClipStats.TimestampClipEnded = TimestampNow;
			}

			if( Result.MotionAmount > ClipStats.LargestMotionDelta )
			{
				ClipStats.LargestMotionDelta = Result.MotionAmount;

				auto MotionMessage = make_shared<CameraUpdateMotionMessage>( CameraID );

				MotionMessage->ClipStats = ClipStats;

				CreateJpegPreview( Frame, MotionMessage->Jpeg, TargetThumbnailSize, DefaultQuality, nullptr );
				
				MessageBusPtr->SendToClient( nullptr, MotionMessage );
			}
		}
	}
	else
	{
		if( State == MotionState::Current )
		{
			ClipStats.TimestampMotionEnded = TimestampNow;
			State = MotionState::GracePeriod;
		}
		else if( State == MotionState::GracePeriod )
		{
			if( TimestampNow - ClipStats.TimestampMotionEnded >= ClipEndGracePeriodInSeconds )
			{
				State = MotionState::None;

				auto MotionMessage = make_shared<CameraEndMotionMessage>( CameraID );

				ClipStats.TimestampClipEnded = TimestampNow;
				MotionMessage->ClipStats = ClipStats;
				
				MessageBusPtr->SendToClient( nullptr, MotionMessage );
			}
		}
	}

	if( SaveNextFrame )
	{
		SaveNextFrame = false;

		auto SaveFrameMessage = make_shared<CameraSnapshotMessage>( CameraID );

		const int TargetSize = SaveLarge ? TargetLargeThumbnailSize : TargetThumbnailSize;
		const int Quality = SaveLarge ? 70 : 65;

		CreateJpegPreview( Frame, SaveFrameMessage->Jpeg, TargetSize, Quality, [=](cv::Mat& OutputFrame)
		{
			int X = (int)(5.0f * cos( 0.25f * (float)FrameIndex ));
			int Y = (int)(5.0f * sin( 0.25f * (float)FrameIndex ));

			cv::line( OutputFrame, cv::Point(15 + X,15 + Y), cv::Point(15 - X, 15 - Y), Result.ClassificationSuperset == 0 ? cv::Scalar(0,255,0) : cv::Scalar(0,0,255), 2 );
		} );

		MessageBusPtr->SendToClient( nullptr, SaveFrameMessage );
	}
}

void ObservingMotionFilter::ClearState()
{
	ClearState( MotionChain.get());
}

void ObservingMotionFilter::ClearState( MotionChainNode* Node )
{
	MotionChainNode* Next = MotionChain.get();
	if (Next)
	{
		Next->Filter->ClearState();

		MotionChainNode* S = Next->OnSuccess.get();
		if(S)
		{
			ClearState(S);
		}

		MotionChainNode* F = Next->OnFailure.get();
		if(F)
		{
			ClearState(F);
		}
	}
}

void ObservingMotionFilter::CreateJpegPreview( FilterFrame& Frame, vector<unsigned char>& OutputBuffer, unsigned int OutputWidth, int OutputQuality, std::function<void(cv::Mat&)> Action )
{
	FilterFrameStatScope Scope( Frame.Stats, FilterStat_JpegEncoding );

	cv::Mat& InputFrame = Frame.GetOrDecodeFrame();

	float Aspect = (float)InputFrame.cols / (float)InputFrame.rows;

	cv::Mat ResizedImage;
	resize( InputFrame, ResizedImage, cv::Size(OutputWidth,(int)((float)OutputWidth/Aspect)), 0, 0 );

	if( Action )
	{
		Action( ResizedImage );
	}

	cv::imencode( ".jpg", ResizedImage, OutputBuffer, std::vector<int>{ CV_IMWRITE_JPEG_QUALITY, OutputQuality } );
}