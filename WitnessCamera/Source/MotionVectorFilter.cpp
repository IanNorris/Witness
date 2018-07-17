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

namespace Witness{
namespace Camera{

struct MotionVectorFilterData : public FilterDataBase
{
	struct Pair
	{
		int x;
		int y;
	};

	vector<Pair> Buckets;

	int MVSinceKF;
	int Frames;
};

PIMPL_CONSTRUCT(MotionVectorFilterData)

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

	const int BucketCount = WidthBucketWidth * HeightBucketHeight;

	const unsigned int WidthShift = GetLog2FromPowerOfTwo(WidthBucketWidth);
	const unsigned int HeightShift = GetLog2FromPowerOfTwo(HeightBucketHeight);

	if (BucketCount != ID.Buckets.size())
	{
		ID.Buckets.resize( BucketCount );
	}

#define BUCKET_INDEX(x,y) ((HeightBucketHeight * y) + x)

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

	for (unsigned int i = 0; i < MotionVectors; i++)
	{
		const AVMotionVector& MV = MVData[i];
		
		unsigned int DstBucketX = MIN( MAX(MV.dst_x,0) >> BUCKET_SHIFT, WidthBucketWidth-1);
		unsigned int DstBucketY = MIN( MAX(MV.dst_y,0) >> BUCKET_SHIFT, HeightBucketHeight-1);

		int DeltaX = MV.dst_x - MV.src_x;
		int DeltaY = MV.dst_y - MV.src_y;

		int Score = DeltaX * DeltaX + DeltaY * DeltaY;

		//const int ScoreAgainst =  15;
		//int Score = DeltaX * DeltaX + DeltaY * DeltaY;
		//if (Score > ScoreAgainst * ScoreAgainst)
		{
			auto& Ref = ID.Buckets[BUCKET_INDEX(DstBucketX, DstBucketY)];
			Ref.x += MV.motion_x;
			Ref.y += MV.motion_y;
		}		
	}

	const int RefValue = 8 * BUCKET_DIMENSION;

	float ScaleX = (float)InputFrame.cols / (float)Frame->width;
	float ScaleY = (float)InputFrame.rows / (float)Frame->height;

	for( unsigned int x = 0; x < WidthBucketWidth; x++ )
	{
		for( unsigned int y = 0; y < HeightBucketHeight; y++ )
		{
			auto& Ref = ID.Buckets[BUCKET_INDEX(x, y)];

			if( abs(Ref.x) >= RefValue || abs(Ref.y) >= RefValue )
			{
				cv::Rect Src;
				Src.x = (int)((float)(x << BUCKET_SHIFT) * ScaleX);
				Src.y = (int)((float)(y << BUCKET_SHIFT) * ScaleY);
				Src.width = (int)(BUCKET_DIMENSION * ScaleX);
				Src.height = (int)(BUCKET_DIMENSION * ScaleY);

				cv::rectangle( InputFrame, Src, cv::Scalar(255.0,255.0,0), CV_FILLED );
			}

			Ref.x = 0;
			Ref.y = 0;
		}
	}

	ID.MVSinceKF += UsableMotionVectors;
	ID.Frames++;

	if (ID.Frames == 25)
	{
		//printf( "%d MV per second\n", ID.MVSinceKF );
		ID.Frames = 0;
		ID.MVSinceKF = 0;
	}
}

}}
