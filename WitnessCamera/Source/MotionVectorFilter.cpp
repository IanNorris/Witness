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

constexpr int BucketDistanceSquared = (2 * BUCKET_DIMENSION) * (2 * BUCKET_DIMENSION);

namespace Witness{
namespace Camera{

struct MotionVectorFilterData : public FilterDataBase
{
	struct Pair
	{
		int x;
		int y;
	};

	struct TrackedObject
	{
		cv::Rect Region;
		int FramesSinceLastSeen;
		int FramesTracked;
	};

	vector<Pair> Buckets;

	vector<Point2i> Points;
	vector<int> Labels;
	vector<Point2i> CurrentCluster;

	vector<TrackedObject> Objects;

	int MVSinceKF;
	int Frames;
};

PIMPL_CONSTRUCT(MotionVectorFilterData)

struct EquivalentPoint {
	bool operator()(const Point2i& a, const Point2i& b)
	{
		int DiffX = a.x - b.x;
		int DiffY = a.y - b.y;

		int DistanceSquared = (DiffX*DiffX) + (DiffY+DiffY);
		
		return (DistanceSquared <= BucketDistanceSquared);
	}
};

MotionVectorFilter::MotionVectorFilter()
{
	auto& ID = GetData();

	ID.Buckets.resize( BUCKET_DIMENSION * BUCKET_DIMENSION );

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

void MotionVectorFilter::FilterFrame( const AVFrame* Frame, ClassificationResult& Result, cv::Mat& InputFrame, cv::Mat& GrayscaleInputFrame )
{
	auto& ID = GetData();

	const unsigned int WidthBucketWidth = GetNextPowerOfTwo( Frame->width / BUCKET_DIMENSION );
	const unsigned int HeightBucketHeight = GetNextPowerOfTwo( Frame->height / BUCKET_DIMENSION );

	const unsigned int HalfBucketWidth = WidthBucketWidth >> 2;
	const unsigned int HalfBucketHeight = HeightBucketHeight >> 2;

	const int BucketCount = WidthBucketWidth * HeightBucketHeight;

	const unsigned int WidthShift = GetLog2FromPowerOfTwo(WidthBucketWidth);
	const unsigned int HeightShift = GetLog2FromPowerOfTwo(HeightBucketHeight);

	if (BucketCount != ID.Buckets.size())
	{
		ID.Buckets.resize( BucketCount );
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

	const  AVMotionVector* MVData = (const AVMotionVector*)SideData->data;
	const unsigned int MotionVectors = SideData->size / sizeof(*MVData);

	unsigned int UsableMotionVectors = 0;

	for (unsigned int i = 0; i < MotionVectors; i+=8)
	{
		const AVMotionVector& MV = MVData[i];
		
		unsigned int DstBucketX = (unsigned int)MIN( MAX((unsigned int)MV.dst_x,0) >> BUCKET_SHIFT, WidthBucketWidth-1);
		unsigned int DstBucketY = (unsigned int)MIN( MAX((unsigned int)MV.dst_y,0) >> BUCKET_SHIFT, HeightBucketHeight-1);

		//int DeltaX = MV.dst_x - MV.src_x;
		//int DeltaY = MV.dst_y - MV.src_y;
		//int Score = DeltaX * DeltaX + DeltaY * DeltaY;
		//const int ScoreAgainst =  9;
		//int Score = DeltaX * DeltaX + DeltaY * DeltaY;
		//if (Score > ScoreAgainst * ScoreAgainst)
		{
			auto& Ref = ID.Buckets[BUCKET_INDEX(DstBucketX, DstBucketY)];
			Ref.x += MV.motion_x;
			Ref.y += MV.motion_y;
		}		
	}

	const int RefValue = 4 * BUCKET_DIMENSION;

	float ScaleX = (float)InputFrame.cols / (float)Frame->width;
	float ScaleY = (float)InputFrame.rows / (float)Frame->height;

	const bool DrawClusters = false;

	ID.Points.clear();
	ID.Labels.clear();

	for( unsigned int y = 0; y < HeightBucketHeight; y++ )
	{
		const int BucketBase = WidthBucketWidth * y;
		for( unsigned int x = 0; x < WidthBucketWidth; x++ )
		{
			auto& Ref = ID.Buckets[BucketBase+x];

			if( abs(Ref.x) + abs(Ref.y) >= RefValue )
			{
				if( DrawClusters )
				{
					cv::Rect Src;
					Src.x = (int)((float)(x << BUCKET_SHIFT) * ScaleX);
					Src.y = (int)((float)(y << BUCKET_SHIFT) * ScaleY);
					Src.width = (int)(BUCKET_DIMENSION * ScaleX);
					Src.height = (int)(BUCKET_DIMENSION * ScaleY);

					cv::rectangle( InputFrame, Src, cv::Scalar(255.0,255.0,0), CV_FILLED );
				}

				ID.Points.push_back( Point2i( x << BUCKET_SHIFT, y << BUCKET_SHIFT ) );
			}

			Ref.x = 0;
			Ref.y = 0;
		}
	}

	int MaxLabel = -1;

	const int MinTrackingFrames = 3;
	const int MaxCompactness = 3;

	for (auto Iter = ID.Objects.begin(); Iter != ID.Objects.end(); ++Iter )
	{
		(*Iter).FramesSinceLastSeen++;
	}

	int TotalArea = 0;
	int ComparisonArea = WidthBucketWidth * BUCKET_DIMENSION * HeightBucketHeight * BUCKET_DIMENSION;

	if( !ID.Points.empty() )
	{
		int Compactness = cv::partition<Point2i,EquivalentPoint>( ID.Points, ID.Labels );
				
		int CurrentClusterIndex = 0;
		int ActualCluters = 0;
		int TrackedClusters = 0;

		const int MinPoints = 3;
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

			if( Points >= MinPoints && Compactness < MaxCompactness)
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
					DrawBounds.x = (int)((float)Bounds.x * ScaleX);
					DrawBounds.y = (int)((float)Bounds.y * ScaleY);
					DrawBounds.width = (int)((float)Bounds.width * ScaleX);
					DrawBounds.height = (int)((float)Bounds.height * ScaleY);
					cv::rectangle( InputFrame, DrawBounds, cv::Scalar(0,0,255.0), 2 );
				}

				ActualCluters++;
			}
		}
		while( MaxLabel > CurrentClusterIndex++ );


		if( DrawClusters )
		{
			char Buffer[128];
			sprintf_s( Buffer, "C=%02d,L=%02d,A=%02d,T=%02d", Compactness, MaxLabel, ActualCluters, TrackedClusters );

			cv::putText( InputFrame, Buffer, Point(25,75), FONT_HERSHEY_PLAIN, 2.5, Scalar(255,0,255), 3 );
		}
	}

	const int LostTrackFrames = 10;

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

}}
