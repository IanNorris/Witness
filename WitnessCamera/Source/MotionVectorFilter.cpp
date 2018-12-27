#include "MotionVectorFilter.h"
#include "OutputStream.h"
#include "FFMPEG/Frame.h"

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

constexpr int BucketDistanceSquared = 2;

namespace Witness{
namespace Camera{

struct MotionVectorFilterData : public FilterDataBase
{
	struct Pair
	{
		float Mask;
		int x;
		int y;
		int c;
	};

	struct TrackedObject
	{
		cv::Rect Region;
		int FramesSinceLastSeen;
		int FramesTracked;
	};

	vector<Pair> Buckets;

	vector<Point2f> Points;
	vector<int> Labels;
	vector<Point2i> CurrentCluster;

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

void MotionVectorFilter::FilterFrame( const AVFrame* Frame, ClassificationResult& Result, cv::Mat& InputFrame, cv::Mat& GrayscaleInputFrame )
{
	auto& ID = GetData();

	const unsigned int WidthBucketWidth = Frame->width / BUCKET_DIMENSION;
	const unsigned int HeightBucketHeight = Frame->height / BUCKET_DIMENSION;

	const unsigned int BucketCount = (WidthBucketWidth) * (HeightBucketHeight);
	
	if (BucketCount != ID.Buckets.size())
	{
		ID.Buckets.resize( BucketCount );
		UpdateMasks( WidthBucketWidth, HeightBucketHeight );
	}

#define BUCKET_INDEX(x,y) ((WidthBucketWidth * y) + x)

	AVFrameSideData* SideData = av_frame_get_side_data( Frame, AV_FRAME_DATA_MOTION_VECTORS );
	if (!SideData)
	{
		//No data == key frame, no actual motion, but can't make an assessment

		//Result.MotionAmount = 0;
		//Result.ClassificationSuperset |= ClassificationResult::Motion_Motion;

		return;
	}

	const bool DrawClusters = false;
	const bool DrawMask = false;
	const bool DrawVectors = false;
	const bool DrawSummaryVectors = false;

	const  AVMotionVector* MVData = (const AVMotionVector*)SideData->data;
	const unsigned int MotionVectors = SideData->size / sizeof(*MVData);

	unsigned int UsableMotionVectors = 0;

	const unsigned int MotionVectorSkipFactor = 2;
	for (unsigned int i = 0; i < MotionVectors; i+=MotionVectorSkipFactor)
	{
		const AVMotionVector& MV = MVData[i];
		
		const int Motion = (MV.motion_x * MV.motion_x) + (MV.motion_y * MV.motion_y);
		
		if( Motion >= 8 )
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

			if( DrawVectors )
			{
				cv::arrowedLine( InputFrame, cv::Point( MV.src_x, MV.src_y ), cv::Point( MV.dst_x, MV.dst_y ), cv::Scalar(255.0,255.0,255.0) );
			}

			
		}
	}

	const int RefValue = 64 * BUCKET_DIMENSION * BUCKET_DIMENSION / MotionVectorSkipFactor;
	//const int RefValue = 50;

	float ScaleX = (float)InputFrame.cols / (float)Frame->width;
	float ScaleY = (float)InputFrame.rows / (float)Frame->height;
	float RescaleX = (float)Frame->width / (float)(WidthBucketWidth);
	float RescaleY = (float)Frame->height / (float)(HeightBucketHeight);

	ID.Points.clear();
	ID.Labels.clear();

	for( unsigned int y = 0; y < HeightBucketHeight; y++ )
	{
		const unsigned int BucketBase = WidthBucketWidth * y;
		for( unsigned int x = 0; x < WidthBucketWidth; x++ )
		{
			auto& Ref = ID.Buckets[BucketBase+x];

			float Score = ((Ref.x*Ref.x) + (Ref.y*Ref.y)) * Ref.Mask / (float)(Ref.c+1);
			//float Score = Ref.x * Mask;

			bool thresholdReached = Score >= RefValue;

			if( thresholdReached )
			{
				ID.Points.push_back( Point2f( (float)x, (float)y ) );
			}

			if( DrawSummaryVectors && Score > 1.0 )
			{
				float NewX = ((float)x+0.5f) * RescaleX * ScaleX;
				float NewY = ((float)y+0.5f) * RescaleY * ScaleY;

				const float SummaryScale = 8.0f;

				cv::arrowedLine( InputFrame, cv::Point2f( NewX, NewY ), cv::Point2f( NewX + (Ref.x / SummaryScale), NewY + (Ref.y / SummaryScale) ), cv::Scalar(0.0,255.0,255.0), 3 );

				char Buffer[128];
				sprintf_s( Buffer, "%4.0f", Score );

				cv::putText( InputFrame, Buffer, Point((int)NewX,(int)NewY), FONT_HERSHEY_PLAIN, 1.0, Scalar(0,255,255), 2 );
			}

			if( DrawClusters )
			{
				cv::Rect Src;
				Src.x = (int)((float)(x * RescaleX) * ScaleX);
				Src.y = (int)((float)(y * RescaleY) * ScaleY);
				Src.width = (int)(RescaleX * ScaleX);
				Src.height = (int)(RescaleY * ScaleY);

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

	ID.Labels.resize(ID.Points.size());

	int MaxLabel = -1;

	const int MinTrackingFrames = 4;
	const int MaxCompactness = 0;

	for (auto Iter = ID.Objects.begin(); Iter != ID.Objects.end(); ++Iter )
	{
		(*Iter).FramesSinceLastSeen++;
	}

	int TotalArea = 0;
	int ComparisonArea = WidthBucketWidth * HeightBucketHeight;

	const int TargetClusters = 2;
	const int MaxClusters = 5;

	if( !ID.Points.empty() )
	{
		cv::partition<Point2f,EquivalentPoint>( ID.Points, ID.Labels );
		//double Compactness = cv::kmeans( cv::Mat(ID.Points).reshape(1, ID.Points.size()), TargetClusters, ID.Labels, cv::TermCriteria( TermCriteria::EPS+TermCriteria::COUNT, MaxClusters, 1.0 ), 3, KMEANS_PP_CENTERS );
				
		int CurrentClusterIndex = 0;
		int ActualCluters = 0;
		int TrackedClusters = 0;

		const int MinPoints = 2;
		do
		{
			int Points = 0;

			MaxLabel = -1;
			ID.CurrentCluster.clear();

			for (int i = 0; i < ID.Points.size(); i++)
			{
				if (ID.Labels[i] == CurrentClusterIndex)
				{
					ID.CurrentCluster.push_back(ID.Points[i]);
					Points++;
				}

				if (ID.Labels[i] > MaxLabel)
				{
					MaxLabel = ID.Labels[i];
				}

			}

			if( Points >= MinPoints)
			{
				cv::Rect Bounds = cv::boundingRect(ID.CurrentCluster);

				bool Found = false;
				for (auto& Object : ID.Objects)
				{
					cv::Rect IntersectedRect = Object.Region & Bounds;
					if (IntersectedRect.area() > MIN(Bounds.area(), Object.Region.area()) >> 1)
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

				if( DrawClusters )
				{
					cv::Rect DrawBounds = Bounds;
					DrawBounds.x = (int)((float)Bounds.x * RescaleX * ScaleX);
					DrawBounds.y = (int)((float)Bounds.y * RescaleY * ScaleY);
					DrawBounds.width = (int)((float)Bounds.width * RescaleX * ScaleX);
					DrawBounds.height = (int)((float)Bounds.height * RescaleY * ScaleY);
					cv::rectangle( InputFrame, DrawBounds, cv::Scalar(0,0,255.0), 2 );
				}

				ActualCluters++;
			}
		}
		while( MaxLabel > CurrentClusterIndex++ );


		if( DrawClusters )
		{
			char Buffer[128];
			sprintf_s( Buffer, "MVU=%04d,MVF=%04d,L=%02d,A=%02d,T=%02d", MotionVectors, UsableMotionVectors, MaxLabel, ActualCluters, TrackedClusters );

			cv::putText( InputFrame, Buffer, Point(25,75), FONT_HERSHEY_PLAIN, 2.5, Scalar(255,0,255), 3 );
		}
	}

	const int LostTrackFrames = 20;

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
