#include "MotionVectorFilter.h"
#include "OutputStream.h"
#include "FFMPEG/Frame.h"
#include "FFMPEG/Common.h"

#include <libavformat/avformat.h>
#include <libavutil/motion_vector.h>

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

#define BUCKET_SHIFT 5
#define BUCKET_DIMENSION (1 << BUCKET_SHIFT)

int FirstCameraOnly = 0;

int BucketDistanceSquared = 3;
float MinSummaryPrintout = 1000.0f;
float MinRatioOfBounds = 0.3f;
float ClusterBoundaryGrowth = 0.5f;

int DrawClusters = false;
int DrawTrackedObjects = true;
int DrawPreTrackedObjects = true;
int DrawStats = false;
int DrawMask = false;
int DrawVectors = false;
int DrawSummaryVectors = false;
int BucketRefValue = 16;
int MinBlockMoveDistance = 4;
int MaxBlockMoveDistance = 512000;
int MinVectorCount = 2;
int MinClusterPoints = 2;
int LostTrackFrames = 8;
int MinTrackingFrames = 2;

namespace Witness{
namespace Camera{

struct MotionVectorFilterData : public FilterDataBase
{
	DebugBind<int> DB_BucketDistance;
	DebugBind<int> DB_DrawTrackedObjects;
	DebugBind<int> DB_DrawPreTrackedObjects;
	DebugBind<int> DB_DrawClusters;
	DebugBind<int> DB_DrawStats;
	DebugBind<int> DB_DrawMask;
	DebugBind<int> DB_DrawVectors;
	DebugBind<int> DB_DrawSummaryVectors;
	DebugBind<int> DB_BucketRefValue;
	DebugBind<float> DB_ClusterBoundaryGrowth;
	DebugBind<float> DB_MinSummaryPrintout;
	DebugBind<int> DB_MinBlockMoveDistance;
	DebugBind<int> DB_MaxBlockMoveDistance;
	DebugBind<int> DB_MinVectorCount;
	DebugBind<float> DB_MinRatioOfBounds;
	DebugBind<int> DB_MinClusterPoints;
	DebugBind<int> DB_LostTrackFrames;
	DebugBind<int> DB_MinTrackingFrames;

	MotionVectorFilterData()
	: DB_BucketDistance( FirstCameraOnly == 0 ? TargetDebugConsole : nullptr, "MV Bucket Distance Squared", &BucketDistanceSquared )
	, DB_DrawTrackedObjects( FirstCameraOnly == 0 ? TargetDebugConsole : nullptr, "MV Draw Tracked Objects", &DrawTrackedObjects )
	, DB_DrawPreTrackedObjects( FirstCameraOnly == 0 ? TargetDebugConsole : nullptr, "MV Draw Pre-Tracked Objects", &DrawPreTrackedObjects )
	, DB_DrawClusters( FirstCameraOnly == 0 ? TargetDebugConsole : nullptr, "MV Draw Clusters", &DrawClusters )
	, DB_DrawStats( FirstCameraOnly == 0 ? TargetDebugConsole : nullptr, "MV Draw Stats", &DrawStats )
	, DB_DrawMask( FirstCameraOnly == 0 ? TargetDebugConsole : nullptr, "MV Draw Mask", &DrawMask )
	, DB_DrawVectors( FirstCameraOnly == 0 ? TargetDebugConsole : nullptr, "MV Draw Vectors", &DrawVectors )
	, DB_DrawSummaryVectors( FirstCameraOnly == 0 ? TargetDebugConsole : nullptr, "MV Draw Summary Vectors", &DrawSummaryVectors )
	, DB_BucketRefValue( FirstCameraOnly == 0 ? TargetDebugConsole : nullptr, "MV Bucket Ref Value", &BucketRefValue )
	, DB_ClusterBoundaryGrowth( FirstCameraOnly == 0 ? TargetDebugConsole : nullptr, "MV Cluster Boundary Growth", &ClusterBoundaryGrowth )
	, DB_MinSummaryPrintout( FirstCameraOnly == 0 ? TargetDebugConsole : nullptr, "MV Min Summary Printout", &MinSummaryPrintout )
	, DB_MinBlockMoveDistance( FirstCameraOnly == 0 ? TargetDebugConsole : nullptr, "MV Min Block Move Distance", &MinBlockMoveDistance )
	, DB_MaxBlockMoveDistance( FirstCameraOnly == 0 ? TargetDebugConsole : nullptr, "MV Max Block Move Distance", &MaxBlockMoveDistance )
	, DB_MinVectorCount( FirstCameraOnly == 0 ? TargetDebugConsole : nullptr, "MV Min Vector Count", &MinVectorCount )
	, DB_MinRatioOfBounds( FirstCameraOnly == 0 ? TargetDebugConsole : nullptr, "MV Min Ratio Of Bounds", &MinRatioOfBounds )
	, DB_MinClusterPoints( FirstCameraOnly == 0 ? TargetDebugConsole : nullptr, "MV Min Cluster Points", &MinClusterPoints )
	, DB_LostTrackFrames( FirstCameraOnly == 0 ? TargetDebugConsole : nullptr, "MV Lost Track Frames", &LostTrackFrames )
	, DB_MinTrackingFrames( FirstCameraOnly == 0 ? TargetDebugConsole : nullptr, "MV Min Tracking Frames", &MinTrackingFrames )
	{
		FirstCameraOnly++;
	}

	~MotionVectorFilterData()
	{
		FirstCameraOnly--;
	}

	struct Pair
	{
		float Mask;
		int x;
		int y;
		int c;
	};

	struct TrackedObject
	{
		vector<cv::Point2f> PreviousPoints;

		cv::Rect Region;
		int FramesSinceLastSeen;
		int FramesTracked;
	};

	struct LabelGroup
	{
		Point2f TopLeft;
		Point2f BottomRight;
		int Points;
	};

	vector<Pair> Buckets;

	vector<Point2f> Points;
	vector<int> Labels;
	vector<LabelGroup> LabelGroups;

	vector<TrackedObject> Objects;

	Mat blackoutMaskOriginal;
	Mat focusMaskOriginal;
	Mat blackoutMask;
	Mat focusMask;

	bool hasBlackoutMask;
	bool hasFocusMask;

	int MVSinceKF;
	int Frames;
};

PIMPL_CONSTRUCT(MotionVectorFilterData)

struct EquivalentPoint {
	bool operator()(const Point2i& a, const Point2i& b)
	{
		int DiffX = a.x - b.x;
		int DiffY = a.y - b.y;

		int DistanceSquared = (DiffX*DiffX) + (DiffY*DiffY);
		
		return (DistanceSquared <= BucketDistanceSquared);
	}
};

MotionVectorFilter::MotionVectorFilter( const wchar_t* BlackoutMaskPath, const wchar_t* FocusMaskPath )
{
	auto& ID = GetData();

	wstring BlackoutMaskPathANSI(BlackoutMaskPath);
	wstring FocusMaskPathANSI(FocusMaskPath);

	ID.hasBlackoutMask = BlackoutMaskPathANSI.length() > 0;
	ID.hasFocusMask = FocusMaskPathANSI.length() > 0;

	if( ID.hasBlackoutMask )
	{
		ID.blackoutMaskOriginal = cv::imread( string(BlackoutMaskPathANSI.begin(), BlackoutMaskPathANSI.end()), IMREAD_GRAYSCALE );
	}

	if( ID.hasFocusMask )
	{
		ID.focusMaskOriginal = cv::imread( string(FocusMaskPathANSI.begin(), FocusMaskPathANSI.end()), IMREAD_GRAYSCALE );	
	}

	ID.MVSinceKF = 0;
	ID.Frames = 0;
}

MotionVectorFilter::~MotionVectorFilter()
{}

//From https://graphics.stanford.edu/~seander/bithacks.html
unsigned int GetNextPowerOfTwo(unsigned int v)
{
	v--;
	v |= v >> 1;
	v |= v >> 2;
	v |= v >> 4;
	v |= v >> 8;
	v |= v >> 16;
	v++;

	return v;
}

//From https://graphics.stanford.edu/~seander/bithacks.html
unsigned int GetLog2FromPowerOfTwo(unsigned int v)
{
	static const int MultiplyDeBruijnBitPosition2[32] = 
	{
	  0, 1, 28, 2, 29, 14, 24, 3, 30, 22, 20, 15, 25, 17, 4, 8, 
	  31, 27, 13, 23, 21, 19, 16, 7, 26, 12, 18, 6, 11, 5, 10, 9
	};
	return MultiplyDeBruijnBitPosition2[(uint32_t)(v * 0x077CB531U) >> 27];
}

void MotionVectorFilter::UpdateMasks( unsigned int Width, unsigned int Height )
{
	auto& ID = GetData();

	if( ID.hasBlackoutMask )
	{
		cv::resize( ID.blackoutMaskOriginal, ID.blackoutMask, cv::Size(Width, Height), 0.0, 0.0, INTER_AREA );
	}

	if( ID.hasFocusMask )
	{
		cv::resize( ID.focusMaskOriginal, ID.focusMask, cv::Size(Width, Height), 0.0, 0.0, INTER_AREA );
	}

	const float FocusMaskMultiplier = 3.0f - 1.0f;

	for( unsigned int x = 0; x < Width; x++ )
	{
		for( unsigned int y = 0; y < Height; y++ )
		{
			uchar blackoutValue = ID.hasBlackoutMask ? ID.blackoutMask.at<uchar>(Point2i(x,y)) : 255;
			uchar focusValue = ID.hasFocusMask ? ID.focusMask.at<uchar>(Point2i(x,y)) : 0;

			float Mask = (float)focusValue / 255.0f;
			float BlackoutMask = (float)blackoutValue / 255.0f;

			ID.Buckets[(Width * y) + x].Mask = (1.0f + (FocusMaskMultiplier * Mask)) * BlackoutMask;
		}
	}
}

void MotionVectorFilter::ClassifyFrame( FilterFrame& Frame, ClassificationResult& Result )
{
	FilterFrameStatScope Scope( Frame.Stats, FilterStat_MVF_Internal );

	auto& ID = GetData();

	bool WantDebuggingInfo = Frame.WantFullSizeOutput | Frame.WantSmallOutput;

	FilterFrameStatExcludeScope ScaleScope( Frame.Stats, FilterStat_MVF_Internal );
		cv::Mat Dummy;
		cv::Mat& InputFrame = WantDebuggingInfo ? Frame.GetOrDecodeFrame() : Dummy;
	ScaleScope.Stop();

	const unsigned int WidthBucketWidth = Frame.InputFrame->GetWidth() / BUCKET_DIMENSION;
	const unsigned int HeightBucketHeight = Frame.InputFrame->GetHeight() / BUCKET_DIMENSION;

	const unsigned int BucketCount = (WidthBucketWidth) * (HeightBucketHeight);
	
	if (BucketCount != ID.Buckets.size())
	{
		ID.Buckets.resize( BucketCount );
		UpdateMasks( WidthBucketWidth, HeightBucketHeight );
	}

#define BUCKET_INDEX(x,y) ((WidthBucketWidth * y) + x)

	AVFrameSideData* SideData = nullptr;
	
	{
		FilterFrameStatScope Scope( Frame.Stats, FilterStat_MVF_SideData );

		SideData = av_frame_get_side_data( Frame.InputFrame->GetFrame(), AV_FRAME_DATA_MOTION_VECTORS );
	}

	if (!SideData)
	{
		//No data == key frame, no actual motion, but can't make an assessment

		//Result.MotionAmount = 0;
		//Result.ClassificationSuperset |= ClassificationResult::Motion_Motion;

		return;
	}

	const  AVMotionVector* MVData = (const AVMotionVector*)SideData->data;
	const unsigned int MotionVectors = SideData->size / sizeof(*MVData);

	unsigned int UsableMotionVectors = 0;

	const unsigned int MotionVectorSkipFactor = 8;
	{
		FilterFrameStatScope Scope( Frame.Stats, FilterStat_MVF_VectorPass );

		for (unsigned int i = 0; i < MotionVectors; i+=MotionVectorSkipFactor)
		{
			const AVMotionVector& MV = MVData[i];
		
			const int Motion = (MV.motion_x * MV.motion_x) + (MV.motion_y * MV.motion_y);
		
			if( Motion >= (float)MinBlockMoveDistance && Motion <= (float)MaxBlockMoveDistance )
			{
				unsigned int DstBucketX = (unsigned int)MIN( MAX(MV.dst_x,0) / BUCKET_DIMENSION, (int)WidthBucketWidth-1);
				unsigned int DstBucketY = (unsigned int)MIN( MAX(MV.dst_y,0) / BUCKET_DIMENSION, (int)HeightBucketHeight-1);

				auto& Ref = ID.Buckets[BUCKET_INDEX(DstBucketX, DstBucketY)];

				if( Ref.Mask == 0.0f )
				{
					continue;
				}

				Ref.c += 1;
				Ref.x += MV.motion_x;
				Ref.y += MV.motion_y;

				UsableMotionVectors++;

				if( WantDebuggingInfo && DrawVectors )
				{
					FilterFrameStatScope Scope( Frame.Stats, FilterStat_Debug );

					cv::arrowedLine( InputFrame, cv::Point( MV.src_x, MV.src_y ), cv::Point( MV.dst_x, MV.dst_y ), cv::Scalar(255.0,255.0,255.0) );
				}

			
			}
		}
	}

	int RefValue = BucketRefValue * BUCKET_DIMENSION * BUCKET_DIMENSION / MotionVectorSkipFactor;
	//const int RefValue = 50;

	float RescaleX = (float)InputFrame.cols / (float)(WidthBucketWidth);
	float RescaleY = (float)InputFrame.rows / (float)(HeightBucketHeight);

	ID.Points.clear();
	ID.Labels.clear();

	{
		FilterFrameStatScope Scope( Frame.Stats, FilterStat_MVF_ClusterPass );

		for( unsigned int y = 0; y < HeightBucketHeight; y++ )
		{
			const unsigned int BucketBase = WidthBucketWidth * y;
			for( unsigned int x = 0; x < WidthBucketWidth; x++ )
			{
				auto& Ref = ID.Buckets[BucketBase+x];

				float Score = ((Ref.x*Ref.x) + (Ref.y*Ref.y)) * Ref.Mask / (float)(Ref.c+1);
				//float Score = Ref.x * Mask;

				bool thresholdReached = Score >= RefValue && Ref.c >= MinVectorCount;

				if( thresholdReached )
				{
					ID.Points.push_back( Point2f( (float)x, (float)y ) );
				}

				if( WantDebuggingInfo && DrawSummaryVectors && Score > MinSummaryPrintout && Ref.c >= MinVectorCount )
				{
					FilterFrameStatScope Scope( Frame.Stats, FilterStat_Debug );

					float NewX = ((float)x+0.5f) * RescaleX;
					float NewY = ((float)y+0.5f) * RescaleY;

					const float SummaryScale = 8.0f;

					cv::arrowedLine( InputFrame, cv::Point2f( NewX, NewY ), cv::Point2f( NewX + (Ref.x / SummaryScale), NewY + (Ref.y / SummaryScale) ), cv::Scalar(0.0,255.0,255.0), 3 );

					char Buffer[128];
					sprintf_s( Buffer, "%.0f", Score );

					cv::putText( InputFrame, Buffer, Point((int)NewX,(int)NewY), FONT_HERSHEY_PLAIN, 1.0, Scalar(0,255,255), 2 );
				}

				if( WantDebuggingInfo && DrawClusters )
				{
					FilterFrameStatScope Scope( Frame.Stats, FilterStat_Debug );

					cv::Rect Src;
					Src.x = (int)((float)(x * RescaleX));
					Src.y = (int)((float)(y * RescaleY));
					Src.width = (int)(RescaleX);
					Src.height = (int)(RescaleY);

					bool isBlackout = Ref.Mask < 0.5f;
					bool isFocus = Ref.Mask > 1.1f;

					if( thresholdReached )
					{
						cv::rectangle( InputFrame, Src, cvScalar(0.0,0.0,255.0), CV_FILLED );
					}
					else if( isBlackout && DrawMask )
					{
						cv::rectangle( InputFrame, Src, cvScalar(0.0,0.0,60.0 - (60.0 * Ref.Mask)), CV_FILLED );
					}
					else if( isFocus && DrawMask )
					{
						cv::rectangle( InputFrame, Src, cvScalar(0.0,60.0 * Ref.Mask / 5.0,0.0), CV_FILLED );
					}
				}

				Ref.c = 0;
				Ref.x = 0;
				Ref.y = 0;
			}
		}
	}

	ID.LabelGroups.clear();

	ID.Labels.resize(ID.Points.size());

	int MaxLabel = -1;

	for (auto Iter = ID.Objects.begin(); Iter != ID.Objects.end(); ++Iter )
	{
		(*Iter).FramesSinceLastSeen++;
	}

	int TotalArea = 0;
	int ComparisonArea = WidthBucketWidth * HeightBucketHeight;

	const int TargetClusters = 2;
	const int MaxClusters = 5;

	int CurrentClusterIndex = 0;
	int ActualCluters = 0;
	int TrackedClusters = 0;
	
	if( !ID.Points.empty() )
	{
		FilterFrameStatScope Scope( Frame.Stats, FilterStat_MVF_ObjectPass );

		cv::partition<Point2f,EquivalentPoint>( ID.Points, ID.Labels );
		//double Compactness = cv::kmeans( cv::Mat(ID.Points).reshape(1, ID.Points.size()), TargetClusters, ID.Labels, cv::TermCriteria( TermCriteria::EPS+TermCriteria::COUNT, MaxClusters, 1.0 ), 3, KMEANS_PP_CENTERS );

		size_t PointsCount = ID.Points.size();
		for (size_t i = 0; i < PointsCount; i++)
		{
			size_t Label = ID.Labels[i];
			MaxLabel = max<int>( MaxLabel, (int)Label );

			if( Label >= ID.LabelGroups.size() || ID.LabelGroups[Label].Points == 0 )
			{
				size_t NewSize = std::max<size_t>(Label+1, ID.LabelGroups.capacity());
				ID.LabelGroups.resize(NewSize);

				auto& LabelGroup = ID.LabelGroups[Label];

				LabelGroup.Points = 1;
				LabelGroup.TopLeft = ID.Points[i];
				LabelGroup.BottomRight = ID.Points[i];
			}
			else
			{
				auto& LabelGroup = ID.LabelGroups[Label];

				LabelGroup.Points++;

				LabelGroup.TopLeft.x = min( LabelGroup.TopLeft.x, ID.Points[i].x );
				LabelGroup.TopLeft.y = min( LabelGroup.TopLeft.y, ID.Points[i].y );
				LabelGroup.BottomRight.x = max( LabelGroup.BottomRight.x, ID.Points[i].x );
				LabelGroup.BottomRight.y = max( LabelGroup.BottomRight.y, ID.Points[i].y );
			}
		}

		const float HalfClusterBoundaryGrowth = ClusterBoundaryGrowth * 0.5f;

		for (size_t i = 0; i <= MaxLabel; i++)
		{
			const auto& LabelGroup = ID.LabelGroups[i];
			if( LabelGroup.Points > MinClusterPoints )
			{
				cv::Rect Bounds( LabelGroup.TopLeft, LabelGroup.BottomRight );

				int BoundsArea = Bounds.area();
				
				//Expand the cluster to allow overlaps
				float Width = (LabelGroup.BottomRight.x - LabelGroup.TopLeft.x) * ClusterBoundaryGrowth;
				float Height = (LabelGroup.BottomRight.y - LabelGroup.TopLeft.y) * ClusterBoundaryGrowth;
				Bounds.x -= (int)(Width * 0.5f);
				Bounds.y -= (int)(Height * 0.5f);
				Bounds.width += (int)Width;
				Bounds.height +=(int)Height;

				float ClusterArea = (float)(LabelGroup.Points);

				if( ClusterArea > (float)BoundsArea * MinRatioOfBounds )
				{
					bool Found = false;
					for (auto& Object : ID.Objects)
					{
						cv::Rect IntersectedRect = Object.Region & Bounds;
						//if (IntersectedRect.area() > MIN(Bounds.area(), Object.Region.area()) >> 1)
						if (IntersectedRect.area() > 0)
						{
							Object.Region = Bounds;
							Object.FramesSinceLastSeen = 0;
							Object.FramesTracked++;
							Found = true;

							if (Object.FramesTracked > MinTrackingFrames)
							{
								TotalArea += Object.Region.area();

								TrackedClusters++;
							}

							break;
						}
					}

					if (!Found)
					{
						MotionVectorFilterData::TrackedObject Obj;
						Obj.Region = Bounds;
						Obj.FramesSinceLastSeen = 0;
						Obj.FramesTracked = 1;

						ID.Objects.push_back( Obj );
					}

					ActualCluters++;
				}
			}
		}
	}

	//Merge clusters
	for (auto Iter = ID.Objects.begin(); Iter != ID.Objects.end(); Iter++ )
	{
		cv::Rect ExpandedObject1 = (*Iter).Region;
		ExpandedObject1.x -= (int)(ExpandedObject1.width * ClusterBoundaryGrowth * 0.5f);
		ExpandedObject1.y -= (int)(ExpandedObject1.height * ClusterBoundaryGrowth * 0.5f);
		ExpandedObject1.width = (int)(ExpandedObject1.width * (1.0f + ClusterBoundaryGrowth));
		ExpandedObject1.height = (int)(ExpandedObject1.height * (1.0f + ClusterBoundaryGrowth));

		bool Overlapped = false;
		for (auto Iter2 = ID.Objects.begin(); Iter2 != ID.Objects.end(); Iter2++ )
		{
			if( Iter == Iter2 )
			{
				continue;
			}

			cv::Rect ExpandedObject2 = (*Iter2).Region;
			ExpandedObject2.x -= (int)(ExpandedObject2.width * ClusterBoundaryGrowth * 0.5f);
			ExpandedObject2.y -= (int)(ExpandedObject2.height * ClusterBoundaryGrowth * 0.5f);
			ExpandedObject2.width = (int)(ExpandedObject2.width * (1.0f + ClusterBoundaryGrowth));
			ExpandedObject2.height = (int)(ExpandedObject2.height * (1.0f + ClusterBoundaryGrowth));

			if( (ExpandedObject1 & ExpandedObject2).area() > 0 )
			{
				(*Iter).Region = (*Iter).Region | (*Iter2).Region;
				(*Iter).FramesSinceLastSeen = min<int>( (*Iter).FramesSinceLastSeen, (*Iter2).FramesSinceLastSeen);
				(*Iter).FramesTracked = max<int>( (*Iter).FramesTracked, (*Iter2).FramesTracked);
				(*Iter2).FramesSinceLastSeen = INT_MAX;
			}
		}

		float X = ((float)(*Iter).Region.x + ((float)(*Iter).Region.width * 0.5f)) * RescaleX;
		float Y = ((float)(*Iter).Region.y + ((float)(*Iter).Region.height * 0.5f)) * RescaleY;

		(*Iter).PreviousPoints.push_back( Point2f(X,Y) );

		if( WantDebuggingInfo && DrawTrackedObjects )
		{
			FilterFrameStatScope Scope( Frame.Stats, FilterStat_Debug );

			cv::Rect DrawBounds;
			DrawBounds.x = (int)((float)ExpandedObject1.x * RescaleX);
			DrawBounds.y = (int)((float)ExpandedObject1.y * RescaleY);
			DrawBounds.width = (int)((float)ExpandedObject1.width * RescaleX);
			DrawBounds.height = (int)((float)ExpandedObject1.height * RescaleY);

			for( int Point = 0; Point < Iter->PreviousPoints.size(); Point++ )
			{
				if( Point != 0 )
				{
					cv::line( InputFrame, Iter->PreviousPoints[Point], Iter->PreviousPoints[Point-1], cv::Scalar(0,0.0,255.0), 2 );
				}
			}

			if( Iter->FramesTracked > MinTrackingFrames )
			{
				if( Iter->FramesSinceLastSeen < LostTrackFrames / 2 )
				{
					cv::rectangle( InputFrame, DrawBounds, cv::Scalar(0,255.0,0.0), 2 );
				}
				else if( Iter->FramesSinceLastSeen >  LostTrackFrames / 2 )
				{
					cv::rectangle( InputFrame, DrawBounds, cv::Scalar(0,150.0,255.0), 2 );
				}
				else
				{
					cv::rectangle( InputFrame, DrawBounds, cv::Scalar(0,0.0,255.0), 2 );
				}
			}
			else
			{
				if( Iter->FramesSinceLastSeen < LostTrackFrames / 2 && DrawPreTrackedObjects )
				{
					cv::rectangle( InputFrame, DrawBounds, cv::Scalar(255.0,0.0,0.0), 2 );
				}
			}
		}
	}

	if( WantDebuggingInfo && DrawStats )
	{
		FilterFrameStatScope Scope( Frame.Stats, FilterStat_Debug );

		char Buffer[128];
		sprintf_s( Buffer, "MVU=%04d,MVF=%04d,L=%02d,A=%02d,T=%02d", MotionVectors, UsableMotionVectors, MaxLabel, ActualCluters, TrackedClusters );

		cv::putText( InputFrame, Buffer, Point(25,75), FONT_HERSHEY_PLAIN, 2.5, Scalar(255,0,255), 3 );
	}

	for (auto Iter = ID.Objects.begin(); Iter != ID.Objects.end(); )
	{
		bool Delete = (*Iter).FramesSinceLastSeen >= LostTrackFrames;
		if (Delete)
		{
			Iter = ID.Objects.erase(Iter);
		}
		else
		{
			++Iter;
		}
	}

	if (TotalArea > 0)
	{
		Result.ClassificationSuperset |= ClassificationResult::Motion_Motion;
		Result.MotionAmount = (float)TotalArea/(float)ComparisonArea;
	}

	ID.MVSinceKF += UsableMotionVectors;
	ID.Frames++;
}

void MotionVectorFilter::ClearState()
{
	auto& ID = GetData();
	ID.Objects.clear();
}

}}
