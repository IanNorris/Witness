#include "ObservingMotionFilter.h"

#include "MessageBus.h"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#ifdef _WIN32
#include <windows.h>
#endif

const int ClipEndGracePeriodInSeconds = 10;
const int TargetLargeThumbnailSize = 1280;
const int TargetThumbnailSize = 300;
const double PreviewTimeout = 0.5;

int DrawObjectLabels = 0;


int ObserverFirstCameraOnly = 0;

ObservingMotionFilter::ObservingMotionFilter( const MotionChainNode& Chain, const int CameraID, const std::shared_ptr<MessageBus>& MessageBusIn )
: IRecordFilter( Chain )
, MessageBusPtr( MessageBusIn )
, LastLargePreviewTimestamp(0)
, LastSmallPreviewTimestamp(0)
, LastPresentedTimestamp( 0 )
, CameraID( CameraID )
, FrameIndex( 0 )
, LastMotionIndex( INT_MIN )
, SaveNextFrame( false )
, WantManualThumbnail( false )
, State( MotionState::None )
, DB_DrawObjectLabels( ObserverFirstCameraOnly == 0 ? TargetDebugConsole : nullptr, "Observer Draw Object Labels", &DrawObjectLabels )
{
	ObserverFirstCameraOnly++;
}

ObservingMotionFilter::~ObservingMotionFilter()
{}

AIResultObserverFilter::AIResultObserverFilter( const std::shared_ptr<ObservingMotionFilter>& ObserverIn )
: IRecordFilter( MotionChainNode{} )
, Observer( ObserverIn )
{
}

bool AIResultObserverFilter::ProcessFrame( SharedClassificationTask TaskData )
{
	if( auto Target = Observer.lock() )
		return Target->ProcessFrame( TaskData );

	TaskData->Next = nullptr;
	TaskData->InsertToQueue( TaskData, true );
	return false;
}

bool ObservingMotionFilter::ProcessFrame( SharedClassificationTask TaskData )
{
	auto IsCurrentTask = [&]()
	{
		return !TaskData->IsCurrentGeneration || TaskData->IsCurrentGeneration( TaskData );
	};

	if( !IsCurrentTask() )
	{
		TaskData->Next = nullptr;
		TaskData->InsertToQueue( TaskData, true );
		return false;
	}

	const bool PublishingAIResults = TaskData->EssentialObservationComplete;
	if( !PublishingAIResults )
	{
		if( TaskData->Frame.Timestamp < LastPresentedTimestamp )
		{
			TaskData->Next = nullptr;
			TaskData->InsertToQueue( TaskData, true );
			return TaskData->Result.ClassificationSuperset != 0;
		}

		LastPresentedTimestamp = TaskData->Frame.Timestamp;
		FrameIndex++;
	}

	int FilterIndex = 0;

	const int DefaultQuality = 70;
	const double NanoSecondsToSeconds = 1000.0 * 1000.0 * 1000.0;

	uint64_t Now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
	bool SaveLarge = !PublishingAIResults && ((double)(Now - LastLargePreviewTimestamp) / NanoSecondsToSeconds) < PreviewTimeout;
	bool SaveSmall = !PublishingAIResults && ((double)(Now - LastSmallPreviewTimestamp) / NanoSecondsToSeconds) < PreviewTimeout;

	SaveNextFrame = SaveLarge | SaveSmall;

	if( !PublishingAIResults )
	{
		// These are current demand signals, not sticky capabilities. Assignment
		// lets the next essential frame stop doing expensive conversion after a
		// viewer disconnects or the preview request expires.
		TaskData->Frame.WantFullSizeOutput = SaveLarge;
		TaskData->Frame.WantSmallOutput = SaveSmall;
	}

	FilterFrameStatScope Scope( TaskData->Frame.Stats, FilterStat_ObserverFilter );
	
	uint64_t TimestampNow = GetUnixTimestamp();

	if (TaskData->Result.ClassificationSuperset )
	{
		if( PublishingAIResults && SaveNextFrame && DrawObjectLabels )
		{
			for( auto& ROI : TaskData->Result.ROI )
			{
				cv::Rect DrawBounds;
				DrawBounds.x = ROI.Left;
				DrawBounds.y = ROI.Top;
				DrawBounds.width = ROI.Width;
				DrawBounds.height = ROI.Height;

				std::string Label = ROI.CustomLabel;
				if( Label.empty() )
				{
					if( (ROI.Classification & ClassificationResult::Motion_Person) != 0 )
					{
						if( (ROI.Classification & ClassificationResult::Motion_Person_Recognized) != 0 )
						{
							Label = "Known Person";
						}
						else if( (ROI.Classification & ClassificationResult::Motion_Person_HighRisk) != 0 )
						{
							Label = "PERSON";
						}
						else
						{
							Label = "Person";
						}
					}
					else if( (ROI.Classification & ClassificationResult::Motion_Animal) != 0 )
					{
						if( (ROI.Classification & ClassificationResult::Motion_Animal_Cat) != 0 )
						{
							Label = "Cat";
						}
						else if( (ROI.Classification & ClassificationResult::Motion_Animal_Dog) != 0 )
						{
							Label = "Dog";
						}
						else
						{
							Label = "Animal";
						}
					}
					else if( (ROI.Classification & ClassificationResult::Motion_Vehicle) != 0 )
					{
						if( (ROI.Classification & ClassificationResult::Motion_Vehicle_Recognized) != 0 )
						{
							Label = "Known Vehicle";
						}
						else if( (ROI.Classification & ClassificationResult::Motion_Vehicle_HighRisk) != 0 )
						{
							Label = "VEHICLE";
						}
						else
						{
							Label = "Vehicle";
						}
					}
				}

				//if( Label.size() )
				{
					char Buffer[128];
					if( Label.size() )
					{
						sprintf_s( Buffer, "TID=%d Class=%s (%.0f%%)", ROI.TrackingID, Label.c_str(), ROI.ClassificationConfidence * 100.0f );
					}
					else
					{
						sprintf_s( Buffer, "TID=%d", ROI.TrackingID );
					}

					cv::Point2i LabelPos = DrawBounds.tl();

					if( LabelPos.y > 50 )
					{
						LabelPos.y -= 50;
					}
					
					cv::putText( TaskData->Frame.GetOrDecodeFrame(), Buffer, LabelPos, cv::FONT_HERSHEY_PLAIN, 3.0, cv::Scalar(0,0,0), 5 );
					LabelPos.x += 2;
					LabelPos.y += 2;
					cv::putText( TaskData->Frame.GetOrDecodeFrame(), Buffer, LabelPos, cv::FONT_HERSHEY_PLAIN, 3.0, cv::Scalar(255,255,255), 2 );
				}
			}
		}

		// Fire detection callback with normalized coordinates for overlay storage/broadcast
		// With AI enabled, publish once after its enriched result returns. Without
		// AI, preserve detections produced by upstream MV/Reolink filters.
		if( (PublishingAIResults || !AITarget) && DetectionCallback && !TaskData->Result.ROI.empty() )
		{
			DetectionFrameData frameData;
			frameData.CameraID = CameraID;
			// Preserve capture time when queued inference completes later.
			frameData.Timestamp = static_cast<double>( TaskData->Frame.Timestamp );
			auto& decodedFrame = TaskData->Frame.GetOrDecodeFrame();
			frameData.FrameWidth = decodedFrame.cols;
			frameData.FrameHeight = decodedFrame.rows;
			frameData.DecodedFrame = decodedFrame.clone();
			frameData.IsMotion = ( TaskData->Result.ClassificationSuperset & ClassificationResult::Motion_Motion ) != 0;

			for( auto& ROI : TaskData->Result.ROI )
			{
				if( frameData.FrameWidth <= 0 || frameData.FrameHeight <= 0 )
					continue;

				DetectionFrameData::Box box;
				box.TrackingID = ROI.TrackingID;
				box.ClassID = static_cast<int>( ROI.Classification );
				box.ClassName = ROI.Tags.empty() ? "" : ROI.Tags[0];
				box.Confidence = ROI.ClassificationConfidence;
				box.X = static_cast<float>( ROI.Left ) / frameData.FrameWidth;
				box.Y = static_cast<float>( ROI.Top ) / frameData.FrameHeight;
				box.W = static_cast<float>( ROI.Width ) / frameData.FrameWidth;
				box.H = static_cast<float>( ROI.Height ) / frameData.FrameHeight;

				// Pass through face landmarks if present
				box.HasLandmarks = ROI.HasLandmarks;
				if( ROI.HasLandmarks )
				{
					for( int lm = 0; lm < 5; lm++ )
					{
						box.LandmarkX[lm] = ROI.LandmarkX[lm] / frameData.FrameWidth;
						box.LandmarkY[lm] = ROI.LandmarkY[lm] / frameData.FrameHeight;
					}
				}
				else
				{
					memset( box.LandmarkX, 0, sizeof( box.LandmarkX ) );
					memset( box.LandmarkY, 0, sizeof( box.LandmarkY ) );
				}

				frameData.Boxes.push_back( std::move( box ) );
			}

			if( !frameData.Boxes.empty() && IsCurrentTask() )
			{
				DetectionCallback( frameData );
			}
		}

		if( !PublishingAIResults && IsCurrentTask() && State == MotionState::None )
		{
			State = MotionState::Current;

			auto MotionMessage = std::make_shared<CameraBeginMotionMessage>( CameraID );

			{
				std::lock_guard<std::mutex> Lock(Mutex);
				Result = TaskData->Result;
			}

			ClipStats.TimestampClipStarted = TimestampNow;
			ClipStats.TimestampMotionStarted = TimestampNow;
			ClipStats.TimestampMotionEnded = INT64_MIN;
			ClipStats.TimestampClipEnded = INT64_MIN;
			ClipStats.LargestMotionDelta = MotionMessage->MotionPercentage = TaskData->Result.MotionAmount;

			MotionMessage->ClipStats = ClipStats;
			MotionMessage->Result = TaskData->Result;

			CreateJpegPreview( TaskData->Frame, MotionMessage->Jpeg, TargetThumbnailSize, DefaultQuality, nullptr );

			if( IsCurrentTask() )
				MessageBusPtr->SendToClient( nullptr, MotionMessage );
		}
		else if( !PublishingAIResults && IsCurrentTask() )
		{
			if( State == MotionState::GracePeriod )
			{
				State = MotionState::Current;
				ClipStats.TimestampMotionEnded = TimestampNow;
				ClipStats.TimestampClipEnded = TimestampNow;
			}

			if( TaskData->Result.MotionAmount > ClipStats.LargestMotionDelta )
			{
				ClipStats.LargestMotionDelta = TaskData->Result.MotionAmount;

				auto MotionMessage = std::make_shared<CameraUpdateMotionMessage>( CameraID );

				MotionMessage->ClipStats = ClipStats;
				MotionMessage->Result = TaskData->Result;

				CreateJpegPreview( TaskData->Frame, MotionMessage->Jpeg, TargetThumbnailSize, DefaultQuality, nullptr );
				
				if( IsCurrentTask() )
					MessageBusPtr->SendToClient( nullptr, MotionMessage );
			}
		}
	}
	else if( !PublishingAIResults && IsCurrentTask() )
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

				auto MotionMessage = std::make_shared<CameraEndMotionMessage>( CameraID );

				ClipStats.TimestampClipEnded = TimestampNow;
				MotionMessage->ClipStats = ClipStats;
				MotionMessage->Result = TaskData->Result;
				
				if( IsCurrentTask() )
					MessageBusPtr->SendToClient( nullptr, MotionMessage );
			}
		}
	}

	if( IsCurrentTask() )
	{
		std::lock_guard<std::mutex> Lock(Mutex);

		if( PublishingAIResults )
		{
			Result.ClassificationSuperset |= TaskData->Result.ClassificationSuperset;
			Result.MotionAmount = std::max( Result.MotionAmount, TaskData->Result.MotionAmount );
			Result.ROI = TaskData->Result.ROI;
		}

		for( auto& Tag : TaskData->Result.Tags )
		{
			bool AlreadyExists = false;
			for( auto& TagExisting : Result.Tags )
			{
				if( TagExisting.compare( Tag ) == 0 )
				{
					AlreadyExists = true;
					break;
				}
			}

			if( !AlreadyExists )
			{
				Result.Tags.push_back(Tag);
			}
		}
	}

	if( !PublishingAIResults && SaveNextFrame && IsCurrentTask() )
	{
		SaveNextFrame = false;

		auto SaveFrameMessage = std::make_shared<CameraSnapshotMessage>( CameraID );

		const int TargetSize = SaveLarge ? TargetLargeThumbnailSize : TargetThumbnailSize;
		const int Quality = SaveLarge ? 70 : 65;

		CreateJpegPreview( TaskData->Frame, SaveFrameMessage->Jpeg, TargetSize, Quality, [=](cv::Mat& OutputFrame)
		{
			int X = (int)(5.0f * cos( 0.25f * (float)FrameIndex ));
			int Y = (int)(5.0f * sin( 0.25f * (float)FrameIndex ));

			cv::line( OutputFrame, cv::Point(15 + X,15 + Y), cv::Point(15 - X, 15 - Y), TaskData->Result.ClassificationSuperset == 0 ? cv::Scalar(0,255,0) : cv::Scalar(0,0,255), 2 );
		} );

		if( IsCurrentTask() )
			MessageBusPtr->SendToClient( nullptr, SaveFrameMessage );
	}

	if ( !PublishingAIResults && WantManualThumbnail && IsCurrentTask() )
	{
		WantManualThumbnail = false;

		auto SnapshotMessage = std::make_shared<CameraSnapshotMessage>( CameraID );
		CreateJpegPreview( TaskData->Frame, SnapshotMessage->Jpeg, TargetThumbnailSize, DefaultQuality, nullptr );
		if( IsCurrentTask() )
			MessageBusPtr->SendToClient( nullptr, SnapshotMessage );
	}

	TaskData->EssentialObservationComplete = true;
	TaskData->Next = PublishingAIResults ? nullptr : AITarget;
	TaskData->InsertToQueue( TaskData, TaskData->Next == nullptr );

	return TaskData->Result.ClassificationSuperset != 0;
}

void ObservingMotionFilter::CreateJpegPreview( FilterFrame& Frame, std::vector<unsigned char>& OutputBuffer, unsigned int OutputWidth, int OutputQuality, std::function<void(cv::Mat&)> Action )
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

	cv::imencode( ".jpg", ResizedImage, OutputBuffer, std::vector<int>{ cv::IMWRITE_JPEG_QUALITY, OutputQuality } );
}
