
// IMPORTANT NOTICE:
//  Various concepts used within this code are being pursued in a patent
//  application and may be patented.
// IMPORTANT NOTICE

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
#include <opencv2/imgproc/imgproc_c.h>

#include <string.h>
#include <atomic>
#include <map>

#include "FilterData.h"
#include <Log.h>

using namespace cv;

#define BUCKET_SHIFT 5
#define BUCKET_DIMENSION (1 << BUCKET_SHIFT)

int FirstCameraOnly = 0;

int BucketDistanceSquared = 3;
float MinSummaryPrintout = 1000.0f;
float MinRatioOfBounds = 0.15f;
float ClusterBoundaryGrowth = 0.33f;
float DBScanClusterProximity = 200.0f;
int DBScanClusterMnpts = 2;

#define DEBUG_MV false

int DrawClusters = false;
int DrawTrackedObjects = DEBUG_MV;
int DrawTrackedObjectLabels = DEBUG_MV;
int DrawPreTrackedObjects = DEBUG_MV;
int DrawTrackedObjectPredictions = DEBUG_MV;
int DrawStats = false;
int DrawMask = false;
int DrawVectors = false;
int DrawSummaryVectors = false;
int BucketRefValue = 12;
int MinBlockMoveDistance = 4;
int MaxBlockMoveDistance = 128000;
int MinVectorCount = 2;
int MinClusterPoints = 2;
int LostTrackFrames = 8;
int LostTrackFramesAfterEngagement = 60;
int MinTrackingFrames = 3;

float KFTranslationScale = 30.0f;
float KFVelocityScale = 200.0f;
float KFBoundsScale = 10.0f;
float KFIdentityScale = 0.1f;
float KFNoiseScale = 0.1f;

#define GROW_RECT(R)															\
		R.x -= (int)(R.width * ClusterBoundaryGrowth * 0.5f);		\
        R.y -= (int)(R.height * ClusterBoundaryGrowth * 0.5f);	\
        R.width += (int)(R.width * ClusterBoundaryGrowth);		\
        R.height += (int)(R.height * ClusterBoundaryGrowth);

namespace Witness{
namespace Camera{


//https://stackoverflow.com/questions/23842940/clustering-image-segments-in-opencv
class DbScan
{
public:
    std::map<int, int> labels;
	std::vector<Rect>& data;
    int C;
    double eps;
    int mnpts;
    double* dp;
    //memoization table in case of complex dist functions
#define DP(i,j) dp[(data.size()*i)+j]
    DbScan(std::vector<Rect>& _data,double _eps,int _mnpts):data(_data)
    {
        C=-1;
        for(int i=0;i<data.size();i++)
        {
            labels[i]=-99;
        }
        eps=_eps;
        mnpts=_mnpts;
    }
    void run()
    {
        dp = new double[data.size()*data.size()];
        for(int i=0;i<data.size();i++)
        {
            for(int j=0;j<data.size();j++)
            {
                if(i==j)
                    DP(i,j)=0;
                else
                    DP(i,j)=-1;
            }
        }
        for(int i=0;i<data.size();i++)
        {
            if(!isVisited(i))
            {
				std::vector<int> neighbours = regionQuery(i);
                if(neighbours.size()<mnpts)
                {
                    labels[i]=-1;//noise
                }else
                {
                    C++;
                    expandCluster(i,neighbours);
                }
            }
        }
        delete [] dp;
    }
    void expandCluster(int p, std::vector<int> neighbours)
    {
        labels[p]=C;
        for(int i=0;i<neighbours.size();i++)
        {
            if(!isVisited(neighbours[i]))
            {
                labels[neighbours[i]]=C;
				std::vector<int> neighbours_p = regionQuery(neighbours[i]);
                if (neighbours_p.size() >= mnpts)
                {
                    expandCluster(neighbours[i],neighbours_p);
                }
            }
        }
    }

    bool isVisited(int i)
    {
        return labels[i]!=-99;
    }

	std::vector<int> regionQuery(int p)
    {
		std::vector<int> res;
        for(int i=0;i<data.size();i++)
        {
            if(distanceFunc(p,i)<=eps)
            {
                res.push_back(i);
            }
        }
        return res;
    }

    double dist2d(Point2d a,Point2d b)
    {
        return sqrt(pow(a.x-b.x,2) + pow(a.y-b.y,2));
    }

    double distanceFunc(int ai,int bi)
    {
        if(DP(ai,bi)!=-1)
            return DP(ai,bi);
        Rect a = data[ai];
        Rect b = data[bi];
        /*
        Point2d cena= Point2d(a.x+a.width/2,
                              a.y+a.height/2);
        Point2d cenb = Point2d(b.x+b.width/2,
                              b.y+b.height/2);
        double dist = sqrt(pow(cena.x-cenb.x,2) + pow(cena.y-cenb.y,2));
        DP(ai,bi)=dist;
        DP(bi,ai)=dist;*/
        Point2d tla =Point2d(a.x,a.y);
        Point2d tra =Point2d(a.x+a.width,a.y);
        Point2d bla =Point2d(a.x,a.y+a.height);
        Point2d bra =Point2d(a.x+a.width,a.y+a.height);

        Point2d tlb =Point2d(b.x,b.y);
        Point2d trb =Point2d(b.x+b.width,b.y);
        Point2d blb =Point2d(b.x,b.y+b.height);
        Point2d brb =Point2d(b.x+b.width,b.y+b.height);

        double minDist = 9999999;

        minDist = min(minDist,dist2d(tla,tlb));
        minDist = min(minDist,dist2d(tla,trb));
        minDist = min(minDist,dist2d(tla,blb));
        minDist = min(minDist,dist2d(tla,brb));

        minDist = min(minDist,dist2d(tra,tlb));
        minDist = min(minDist,dist2d(tra,trb));
        minDist = min(minDist,dist2d(tra,blb));
        minDist = min(minDist,dist2d(tra,brb));

        minDist = min(minDist,dist2d(bla,tlb));
        minDist = min(minDist,dist2d(bla,trb));
        minDist = min(minDist,dist2d(bla,blb));
        minDist = min(minDist,dist2d(bla,brb));

        minDist = min(minDist,dist2d(bra,tlb));
        minDist = min(minDist,dist2d(bra,trb));
        minDist = min(minDist,dist2d(bra,blb));
        minDist = min(minDist,dist2d(bra,brb));
        DP(ai,bi)=minDist;
        DP(bi,ai)=minDist;
        return DP(ai,bi);
    }

	std::vector<std::vector<Rect> > getGroups()
    {
		std::vector<std::vector<Rect> > ret;
        for(int i=0;i<=C;i++)
        {
            ret.push_back(std::vector<Rect>());
            for(int j=0;j<data.size();j++)
            {
                if(labels[j]==i)
                {
                    ret[ret.size()-1].push_back(data[j]);
                }
            }
        }
        return ret;
    }
};

struct MotionVectorFilterData : public FilterDataBase
{
	DebugBind<int> DB_BucketDistance;
	DebugBind<int> DB_DrawTrackedObjects;
	DebugBind<int> DB_DrawPreTrackedObjects;
	DebugBind<int> DB_DrawTrackedObjectLabels;
	DebugBind<int> DB_DrawTrackedObjectPredictions;
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
	DebugBind<int> DB_LostTrackFramesAfterEngagement;
	DebugBind<int> DB_MinTrackingFrames;

	DebugBind<float> DB_KFTranslationScale;
	DebugBind<float> DB_KFVelocityScale;
	DebugBind<float> DB_KFBoundsScale;
	DebugBind<float> DB_KFIdentityScale;
	DebugBind<float> DB_KFNoiseScale;

	DebugBind<float> DB_DBScanClusterProximity;
	DebugBind<int> DB_DBScanClusterMnpts;

	MotionVectorFilterData()
	: DB_BucketDistance( FirstCameraOnly == 0 ? TargetDebugConsole : nullptr, "MV Bucket Distance Squared", &BucketDistanceSquared )
	, DB_DrawTrackedObjects( FirstCameraOnly == 0 ? TargetDebugConsole : nullptr, "MV Draw Tracked Objects", &DrawTrackedObjects )
	, DB_DrawTrackedObjectLabels( FirstCameraOnly == 0 ? TargetDebugConsole : nullptr, "MV Draw Tracked Object Labels", &DrawTrackedObjectLabels )
	, DB_DrawPreTrackedObjects( FirstCameraOnly == 0 ? TargetDebugConsole : nullptr, "MV Draw Pre-Tracked Objects", &DrawPreTrackedObjects )
	, DB_DrawTrackedObjectPredictions( FirstCameraOnly == 0 ? TargetDebugConsole : nullptr, "MV Draw Tracked Object Predictions", &DrawTrackedObjectPredictions )
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
	, DB_LostTrackFramesAfterEngagement( FirstCameraOnly == 0 ? TargetDebugConsole : nullptr, "MV Lost Track Frames After Engagement", &LostTrackFramesAfterEngagement )
	, DB_MinTrackingFrames( FirstCameraOnly == 0 ? TargetDebugConsole : nullptr, "MV Min Tracking Frames", &MinTrackingFrames )
	, DB_KFTranslationScale( FirstCameraOnly == 0 ? TargetDebugConsole : nullptr, "MV KF Translation Scale", &KFTranslationScale )
	, DB_KFVelocityScale( FirstCameraOnly == 0 ? TargetDebugConsole : nullptr, "MV KF Velocity Scale", &KFVelocityScale )
	, DB_KFBoundsScale( FirstCameraOnly == 0 ? TargetDebugConsole : nullptr, "MV KF Bounds Scale", &KFBoundsScale )
	, DB_KFIdentityScale( FirstCameraOnly == 0 ? TargetDebugConsole : nullptr, "MV KF Identity Scale", &KFIdentityScale )
	, DB_KFNoiseScale( FirstCameraOnly == 0 ? TargetDebugConsole : nullptr, "MV KF Noise Scale", &KFNoiseScale )
	, DB_DBScanClusterProximity( FirstCameraOnly == 0 ? TargetDebugConsole : nullptr, "MV DBScan Cluster Proximity", &DBScanClusterProximity )
	, DB_DBScanClusterMnpts( FirstCameraOnly == 0 ? TargetDebugConsole : nullptr, "MV DBScan Cluster Mnpts", &DBScanClusterMnpts )

	, ClearObjectData( false )
	, ObjectIDCounter( 0 )
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
		const static int KalmanStateSize = 6;		//X, Y, VX, VY, W, H
		const static int KalmanMeasureParams = 4;	// X, Y, W, H
		const static int KalmanControlParams = 0;

		//Reference: https://github.com/Myzhar/simple-opencv-kalman-tracker/blob/master/source/opencv-kalman.cpp

		TrackedObject()
			: TrackedPositionFilter( KalmanStateSize, KalmanMeasureParams, KalmanControlParams, CV_32F )
			, KFState( KalmanStateSize, 1, CV_32F )
			, KFMeasure( KalmanMeasureParams, 1, CV_32F )
			, ObjectID( 0 )
			, Classification( 0 )
			, ClassificationGroup( 0 )
			, ClassificationConfidence( 0.0f )
			, FramesSinceLastSeen( INT_MAX )
			, FramesTracked( 0 )
			, WasEngaged( false )
		{
			/*
			float KFTranslationScale = 10.0f;
float KFVelocityScale = 10.0f;
float KFBoundsScale = 10.0f;
float KFIdentityScale = 0.1f;
float KFNoiseScale = 0.1f;*/

			cv::setIdentity( TrackedPositionFilter.transitionMatrix );
			TrackedPositionFilter.measurementMatrix = cv::Mat::zeros( KalmanMeasureParams, KalmanStateSize, CV_32F );
			TrackedPositionFilter.measurementMatrix.at<float>(0) = 1.0f;
			TrackedPositionFilter.measurementMatrix.at<float>(7) = 1.0f;
			TrackedPositionFilter.measurementMatrix.at<float>(16) = 1.0f;
			TrackedPositionFilter.measurementMatrix.at<float>(23) = 1.0f;

			TrackedPositionFilter.processNoiseCov.at<float>(0) = KFTranslationScale;
			TrackedPositionFilter.processNoiseCov.at<float>(7) = KFTranslationScale;
			TrackedPositionFilter.processNoiseCov.at<float>(14) = KFVelocityScale;
			TrackedPositionFilter.processNoiseCov.at<float>(21) = KFVelocityScale;
			TrackedPositionFilter.processNoiseCov.at<float>(28) = KFBoundsScale;
			TrackedPositionFilter.processNoiseCov.at<float>(35) = KFBoundsScale;

			cv::setIdentity( TrackedPositionFilter.measurementNoiseCov, KFIdentityScale );
		}

		std::vector<cv::Point2f> PreviousPoints;

		std::string CustomLabel;

		cv::KalmanFilter TrackedPositionFilter;
		cv::Mat KFState;
		cv::Mat KFMeasure;

		cv::Rect Region;
		unsigned int ObjectID;
		unsigned int Classification;
		unsigned int ClassificationGroup;
		float ClassificationConfidence;
		int FramesSinceLastSeen;
		int FramesTracked;
		bool WasEngaged;
	};

	struct LabelGroup
	{
		Point2f TopLeft;
		Point2f BottomRight;
		int Points;
	};

	std::vector<Pair> Buckets;

	std::vector<Point2f> Points;
	std::vector<int> Labels;
	std::vector<LabelGroup> LabelGroups;

	std::vector<TrackedObject> Objects;

	Mat blackoutMaskOriginal;
	Mat focusMaskOriginal;
	Mat blackoutMask;
	Mat focusMask;

	bool hasBlackoutMask;
	bool hasFocusMask;

	std::atomic<bool> ClearObjectData;

	unsigned int ObjectIDCounter;

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

MotionVectorFilter::MotionVectorFilter( const MotionChainNode& Chain, const char* BlackoutMaskPath, const char* FocusMaskPath )
	: RecordFilterBase( Chain )
{
	auto& ID = GetData();

	std::string BlackoutMaskPathStr(BlackoutMaskPath ? BlackoutMaskPath : "");
	std::string FocusMaskPathStr(FocusMaskPath ? FocusMaskPath : "");

	ID.hasBlackoutMask = BlackoutMaskPathStr.length() > 0;
	ID.hasFocusMask = FocusMaskPathStr.length() > 0;

	if( ID.hasBlackoutMask )
	{
		ID.blackoutMaskOriginal = cv::imread(BlackoutMaskPathStr, IMREAD_GRAYSCALE );
		if( ID.blackoutMaskOriginal.empty() )
		{
			LOG_WARNING( "Blackout mask not found: %s", BlackoutMaskPathStr.c_str() );
			ID.hasBlackoutMask = false;
		}
	}

	if( ID.hasFocusMask )
	{
		ID.focusMaskOriginal = cv::imread(FocusMaskPathStr, IMREAD_GRAYSCALE );
		if( ID.focusMaskOriginal.empty() )
		{
			LOG_WARNING( "Focus mask not found: %s", FocusMaskPathStr.c_str() );
			ID.hasFocusMask = false;
		}
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

	if( ID.hasBlackoutMask && !ID.blackoutMaskOriginal.empty() )
	{
		cv::resize( ID.blackoutMaskOriginal, ID.blackoutMask, cv::Size(Width, Height), 0.0, 0.0, INTER_AREA );
	}

	if( ID.hasFocusMask && !ID.focusMaskOriginal.empty() )
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

bool MotionVectorFilter::ProcessFrame( SharedClassificationTask TaskData )
{
	FilterFrameStatScope Scope( TaskData->Frame.Stats, FilterStat_MVF_Internal );

	auto& ID = GetData();

	bool WantDebuggingInfo = TaskData->Frame.WantFullSizeOutput | TaskData->Frame.WantSmallOutput;

	FilterFrameStatExcludeScope ScaleScope( TaskData->Frame.Stats, FilterStat_MVF_Internal );
		cv::Mat Dummy;
		cv::Mat& InputFrame = WantDebuggingInfo ? TaskData->Frame.GetOrDecodeFrame() : Dummy;
	ScaleScope.Stop();

	const unsigned int WidthBucketWidth = TaskData->Frame.InputFrame->GetWidth() / BUCKET_DIMENSION;
	const unsigned int HeightBucketHeight = TaskData->Frame.InputFrame->GetHeight() / BUCKET_DIMENSION;

	const unsigned int BucketCount = (WidthBucketWidth) * (HeightBucketHeight);
	
	if (BucketCount != ID.Buckets.size())
	{
		ID.Buckets.resize( BucketCount );
		UpdateMasks( WidthBucketWidth, HeightBucketHeight );
	}

#define BUCKET_INDEX(x,y) ((WidthBucketWidth * y) + x)

	AVFrameSideData* SideData = nullptr;
	
	{
		FilterFrameStatScope Scope( TaskData->Frame.Stats, FilterStat_MVF_SideData );

		SideData = av_frame_get_side_data( TaskData->Frame.InputFrame->GetFrame(), AV_FRAME_DATA_MOTION_VECTORS );
	}

	if (!SideData)
	{
		//No data == key frame, no actual motion, but can't make an assessment

		//Result.MotionAmount = 0;
		//Result.ClassificationSuperset |= ClassificationResult::Motion_Motion;

		return true;
	}

	const  AVMotionVector* MVData = (const AVMotionVector*)SideData->data;
	const unsigned int MotionVectors = SideData->size / sizeof(*MVData);

	unsigned int UsableMotionVectors = 0;

	const unsigned int MotionVectorSkipFactor = 8;

	// For high-resolution streams, reduce skip factor to sample more MVs.
	// HEVC at 4K produces fewer total MVs, so skipping fewer ensures enough per-bucket data.
	unsigned int effectiveSkipFactor = MotionVectorSkipFactor;
	{
		unsigned int frameWidth = TaskData->Frame.InputFrame->GetWidth();
		unsigned int frameHeight = TaskData->Frame.InputFrame->GetHeight();
		if (frameWidth > 1920 || frameHeight > 1080)
		{
			double areaRatio = (double)(1920 * 1080) / (double)(frameWidth * frameHeight);
			effectiveSkipFactor = (unsigned int)(MotionVectorSkipFactor * areaRatio);
			if (effectiveSkipFactor < 1) effectiveSkipFactor = 1;
		}
	}
	{
		FilterFrameStatScope Scope( TaskData->Frame.Stats, FilterStat_MVF_VectorPass );

		for (unsigned int i = 0; i < MotionVectors; i+=effectiveSkipFactor)
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
					FilterFrameStatScope Scope( TaskData->Frame.Stats, FilterStat_Debug );

					cv::arrowedLine( InputFrame, cv::Point( MV.src_x, MV.src_y ), cv::Point( MV.dst_x, MV.dst_y ), cv::Scalar(255.0,255.0,255.0) );
				}

			
			}
		}
	}

	int RefValue = BucketRefValue * BUCKET_DIMENSION * BUCKET_DIMENSION / MotionVectorSkipFactor;

	// Scale RefValue for high-resolution streams. The filter was calibrated for 1080p H.264
	// (60×33 = 1980 buckets, ~100k MVs). At 4K (120×67 = 8040 buckets), motion vectors spread
	// across 4× more buckets, so per-bucket scores are proportionally lower.
	// HEVC also produces fewer total MVs due to larger CTU blocks (up to 64×64 vs H.264 16×16).
	{
		const unsigned int refWidth = 1920;
		const unsigned int refHeight = 1080;
		unsigned int frameWidth = TaskData->Frame.InputFrame->GetWidth();
		unsigned int frameHeight = TaskData->Frame.InputFrame->GetHeight();
		if (frameWidth > refWidth || frameHeight > refHeight)
		{
			double areaRatio = (double)(refWidth * refHeight) / (double)(frameWidth * frameHeight);
			RefValue = (int)(RefValue * areaRatio);
			if (RefValue < 1) RefValue = 1;
		}
	}
	//const int RefValue = 50;

	float RescaleX = (float)TaskData->Frame.InputFrame->GetWidth() / (float)(WidthBucketWidth);
	float RescaleY = (float)TaskData->Frame.InputFrame->GetHeight() / (float)(HeightBucketHeight);

	ID.Points.clear();
	ID.Labels.clear();

	if (ID.ClearObjectData.load())
	{
		ID.Objects.clear();
		ID.ClearObjectData.store(false);
	}

	{
		FilterFrameStatScope Scope( TaskData->Frame.Stats, FilterStat_MVF_ClusterPass );

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
					FilterFrameStatScope Scope( TaskData->Frame.Stats, FilterStat_Debug );

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
					FilterFrameStatScope Scope( TaskData->Frame.Stats, FilterStat_Debug );

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
	int ComparisonArea = (int)((float)WidthBucketWidth * (float)HeightBucketHeight * RescaleX * RescaleY);

	const int TargetClusters = 2;
	const int MaxClusters = 5;

	int CurrentClusterIndex = 0;
	int ActualCluters = 0;
	int TrackedClusters = 0;
	
	if( !ID.Points.empty() )
	{
		FilterFrameStatScope Scope( TaskData->Frame.Stats, FilterStat_MVF_ObjectPass );

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

				LabelGroup.TopLeft.x = min( LabelGroup.TopLeft.x, ID.Points[i].x - 1 );
				LabelGroup.TopLeft.y = min( LabelGroup.TopLeft.y, ID.Points[i].y - 1 );
				LabelGroup.BottomRight.x = max( LabelGroup.BottomRight.x, ID.Points[i].x + 1 );
				LabelGroup.BottomRight.y = max( LabelGroup.BottomRight.y, ID.Points[i].y + 1 );
			}
		}

		const float HalfClusterBoundaryGrowth = ClusterBoundaryGrowth * 0.5f;

		for (size_t i = 0; i <= MaxLabel; i++)
		{
			const auto& LabelGroup = ID.LabelGroups[i];
			if( LabelGroup.Points > MinClusterPoints )
			{
				cv::Rect Bounds( LabelGroup.TopLeft, LabelGroup.BottomRight );
				cv::Rect UnscaledBounds = Bounds;
				Bounds.x = (int)((float)Bounds.x * RescaleX);
				Bounds.y = (int)((float)Bounds.y * RescaleY);
				Bounds.width = (int)((float)Bounds.width * RescaleX);
				Bounds.height = (int)((float)Bounds.height * RescaleY);

				GROW_RECT( Bounds );

				int UnscaledBoundsArea = UnscaledBounds.area();
				
				float ClusterArea = (float)(LabelGroup.Points);

				if( ClusterArea > (float)UnscaledBoundsArea * MinRatioOfBounds )
				{
					bool Found = false;
					for (auto& Object : ID.Objects)
					{
						cv::Rect ExpandedOriginal = Object.Region;
						cv::Rect ExpandedBounds = Bounds;

						GROW_RECT(ExpandedOriginal);
						GROW_RECT(ExpandedBounds);

						cv::Rect IntersectedRect = ExpandedOriginal & ExpandedBounds;
						if (IntersectedRect.area() > 0)
						{
							Object.Region = IntersectedRect;
							Object.FramesSinceLastSeen = 0;
							Object.FramesTracked++;
							Found = true;

							if (Object.FramesTracked > MinTrackingFrames)
							{
								Object.WasEngaged = true;

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
						Obj.ObjectID = ++ID.ObjectIDCounter;
						Obj.Classification = ClassificationResult::Motion_Motion;
						Obj.ClassificationGroup = 0;

						if( Obj.Region.width > 0 && Obj.Region.height > 0 )
						{
							ID.Objects.push_back( Obj );
						}
					}

					ActualCluters++;
				}
			}
		}
	}

	std::vector<Rect> ClusterRects;

	for (auto Iter = ID.Objects.begin(); Iter != ID.Objects.end(); Iter++ )
	{
		ClusterRects.push_back( (*Iter).Region );
	}

	DbScan MergeClusters( ClusterRects, DBScanClusterProximity, DBScanClusterMnpts );
	MergeClusters.run();

	//Merge clusters
	int ClusterIndex = 0;
	for (auto Iter = ID.Objects.begin(); Iter != ID.Objects.end(); Iter++ )
	{
		const int TotalFrames = 30;
		int Frames = 0;
		bool HasPrevious = false;
		cv::Point2f Velocity;
		cv::Point2f PreviousPoint;
		for( auto PointIter = (*Iter).PreviousPoints.crbegin(); PointIter != (*Iter).PreviousPoints.crend(); ++PointIter )
		{
			if( Frames >= TotalFrames )
			{
				break;
			}

			if( HasPrevious )
			{
				Velocity += PreviousPoint - (*PointIter);
				Frames++;
			}
			PreviousPoint = *PointIter;

			HasPrevious = true;
		}

		if( Frames > 5 )
		{
			Velocity.x /= (float)Frames;
			Velocity.y /= (float)Frames;
		}

		auto& KFState = (*Iter).KFState;

		KFState = (*Iter).TrackedPositionFilter.predict();

		if( WantDebuggingInfo && DrawTrackedObjects )
		{
			cv::rectangle( InputFrame, (*Iter).Region, cv::Scalar(100.0,100.0,100.0), 2 );
		}

		auto& KFMeasure = (*Iter).KFMeasure;
		KFMeasure.at<float>(0) = (float)(((*Iter).Region.x + ((*Iter).Region.width * 0.5f)));
		KFMeasure.at<float>(1) = (float)(((*Iter).Region.y + ((*Iter).Region.height * 0.5f)));
		KFMeasure.at<float>(2) = (float)((*Iter).Region.width);
		KFMeasure.at<float>(3) = (float)((*Iter).Region.height);

		if( (*Iter).FramesTracked == 1 )
		{
			(*Iter).TrackedPositionFilter.errorCovPre.at<float>(0) = KFNoiseScale;
			(*Iter).TrackedPositionFilter.errorCovPre.at<float>(7) = KFNoiseScale;
			(*Iter).TrackedPositionFilter.errorCovPre.at<float>(14) = KFNoiseScale;
			(*Iter).TrackedPositionFilter.errorCovPre.at<float>(21) = KFNoiseScale;
			(*Iter).TrackedPositionFilter.errorCovPre.at<float>(28) = KFNoiseScale;
			(*Iter).TrackedPositionFilter.errorCovPre.at<float>(35) = KFNoiseScale;

			KFState.at<float>(0) = KFMeasure.at<float>(0);
			KFState.at<float>(1) = KFMeasure.at<float>(1);
			KFState.at<float>(2) = 0.0f;
			KFState.at<float>(3) = 0.0f;
			KFState.at<float>(4) = KFMeasure.at<float>(2);
			KFState.at<float>(5) = KFMeasure.at<float>(3);

			(*Iter).TrackedPositionFilter.statePost = KFState;
		}
		else
		{
			(*Iter).TrackedPositionFilter.correct(KFMeasure);
		}


		float PredictedWidth = KFState.at<float>(4);
		float PredictedHeight = KFState.at<float>(5);
		float PredictedX = KFState.at<float>(0);
		float PredictedY = KFState.at<float>(1);

		cv::Rect ExpandedObjectPrediction;
		ExpandedObjectPrediction.width = (int)PredictedWidth;
		ExpandedObjectPrediction.height = (int)PredictedHeight;
		ExpandedObjectPrediction.x = (int)(PredictedX - (ExpandedObjectPrediction.width * 0.5f));
		ExpandedObjectPrediction.y = (int)(PredictedY - (ExpandedObjectPrediction.height * 0.5f));

		(*Iter).Region = ExpandedObjectPrediction;

		GROW_RECT(ExpandedObjectPrediction);

		if( WantDebuggingInfo && DrawTrackedObjects )
		{
			cv::rectangle( InputFrame, ExpandedObjectPrediction, cv::Scalar(50.0,50.0,50.0), 2 );
		}

		int OriginClusterLabel = MergeClusters.labels[ClusterIndex];


		int InnerClusterIndex = 0;
		bool Overlapped = false;
		for (auto Iter2 = ID.Objects.begin(); Iter2 != ID.Objects.end(); Iter2++ )
		{
			if( Iter == Iter2 )
			{
				InnerClusterIndex++;
				continue;
			}

			int InnerClusterLabel = MergeClusters.labels[InnerClusterIndex];

			float Obj2PredictedWidth = (*Iter2).KFState.at<float>(4);
			float Obj2PredictedHeight = (*Iter2).KFState.at<float>(5);
			float Obj2PredictedX = (*Iter2).KFState.at<float>(0);
			float Obj2PredictedY = (*Iter2).KFState.at<float>(1);

			cv::Rect Obj2ExpandedObjectPrediction;
			Obj2ExpandedObjectPrediction.width = (int)Obj2PredictedX;
			Obj2ExpandedObjectPrediction.height = (int)Obj2PredictedX;
			Obj2ExpandedObjectPrediction.x = (int)(Obj2PredictedX - (Obj2ExpandedObjectPrediction.width * 0.5f));
			Obj2ExpandedObjectPrediction.y = (int)(Obj2PredictedY - (Obj2ExpandedObjectPrediction.height * 0.5f));

			GROW_RECT(Obj2ExpandedObjectPrediction);

			if( OriginClusterLabel == InnerClusterLabel && OriginClusterLabel >= 0 && InnerClusterIndex >= 0 )
			{
				(*Iter).Region = (*Iter).Region | (*Iter2).Region;
				(*Iter).FramesSinceLastSeen = min<int>( (*Iter).FramesSinceLastSeen, (*Iter2).FramesSinceLastSeen);
				(*Iter).ObjectID = min<int>( (*Iter).ObjectID, (*Iter2).ObjectID);
				(*Iter).FramesTracked = max<int>( (*Iter).FramesTracked, (*Iter2).FramesTracked);
				(*Iter2).FramesSinceLastSeen = INT_MAX;
				Overlapped = true;

				//TODO: Merge classifications
				if( ((*Iter2).Classification & (~ClassificationResult::Motion_Motion)) != 0 )
				{
					(*Iter).Classification |= (*Iter2).Classification;
					(*Iter).ClassificationGroup |= (*Iter2).ClassificationGroup;
					(*Iter).ClassificationConfidence = max<float>( (*Iter).ClassificationConfidence, (*Iter2).ClassificationConfidence ) ;
					(*Iter).CustomLabel = (*Iter2).CustomLabel;
				}
			}

			InnerClusterIndex++;
		}

		if( (*Iter).Region.width > 0 && (*Iter).Region.height > 0 )
		{
			float X = (float)((*Iter).Region.x + ((*Iter).Region.width / 2));
			float Y = (float)((*Iter).Region.y + ((*Iter).Region.height / 2));

			(*Iter).PreviousPoints.push_back( Point2f(X,Y) );
		}

		if( WantDebuggingInfo && DrawTrackedObjects )
		{
			FilterFrameStatScope Scope( TaskData->Frame.Stats, FilterStat_Debug );

			if( DrawTrackedObjectPredictions )
			{
				for( int Point = 0; Point < Iter->PreviousPoints.size(); Point++ )
				{
					if( Point != 0 )
					{
						cv::line( InputFrame, Iter->PreviousPoints[Point], Iter->PreviousPoints[Point-1], cv::Scalar(0,0.0,255.0), 2 );
					}
				}
			}

			if( Iter->FramesTracked > MinTrackingFrames && Iter->FramesSinceLastSeen < LostTrackFrames )
			{
				if( Iter->FramesSinceLastSeen < LostTrackFrames / 2 )
				{
					cv::rectangle( InputFrame, ExpandedObjectPrediction, cv::Scalar(0,255.0,0.0), 2 );
				}
				else if( Iter->FramesSinceLastSeen >  LostTrackFrames / 2 )
				{
					cv::rectangle( InputFrame, ExpandedObjectPrediction, cv::Scalar(0,150.0,255.0), 2 );
				}
				else
				{
					cv::rectangle( InputFrame, ExpandedObjectPrediction, cv::Scalar(0,0.0,255.0), 2 );
				}
			}
			else
			{
				if( Iter->FramesSinceLastSeen == 0 && DrawPreTrackedObjects )
				{
					cv::rectangle( InputFrame, ExpandedObjectPrediction, cv::Scalar(255.0,0.0,0.0), 2 );
				}
			}
		}

		ClusterIndex++;
	}

	if( WantDebuggingInfo && DrawStats )
	{
		FilterFrameStatScope Scope( TaskData->Frame.Stats, FilterStat_Debug );

		char Buffer[128];
		sprintf_s( Buffer, "MVU=%04d,MVF=%04d,L=%02d,A=%02d,T=%02d", MotionVectors, UsableMotionVectors, MaxLabel, ActualCluters, TrackedClusters );

		cv::putText( InputFrame, Buffer, Point(25,75), FONT_HERSHEY_PLAIN, 2.5, Scalar(255,0,255), 3 );
	}

	for (auto Iter = ID.Objects.begin(); Iter != ID.Objects.end(); )
	{
		bool Delete = (*Iter).WasEngaged ? ((*Iter).FramesSinceLastSeen >= LostTrackFramesAfterEngagement) : ((*Iter).FramesSinceLastSeen >= LostTrackFrames);
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
		TaskData->Result.ClassificationSuperset = 0;
		TaskData->Result.MotionAmount = (float)TotalArea/(float)ComparisonArea;

		for (auto Iter = ID.Objects.begin(); Iter != ID.Objects.end(); Iter++ )
		{
			ClassificationResult::RegionOfInterest ROI;
					
			cv::Rect ExpandedObject = (*Iter).Region;

			ROI.Classification |= ClassificationResult::Motion_Motion;
			ROI.Left = max<int>( ExpandedObject.x, 0 );
			ROI.Top = max<int>( ExpandedObject.y, 0 );
			ROI.Width = min<int>( ExpandedObject.width, TaskData->Frame.InputFrame->GetWidth() - ROI.Left );
			ROI.Height = min<int>( ExpandedObject.height, TaskData->Frame.InputFrame->GetHeight() - ROI.Top );

			if( ROI.Width > 0 && ROI.Height > 0 )
			{
				ROI.TrackingID = (*Iter).ObjectID;
				ROI.Filter = this;

				ROI.Classification = (*Iter).Classification;
				ROI.ClassificationGroup = (*Iter).ClassificationGroup;
				ROI.ClassificationConfidence = (*Iter).ClassificationConfidence;

				TaskData->Result.ClassificationSuperset |= ROI.Classification;

				TaskData->Result.ROI.push_back( ROI );
			}
		}
	}

	ID.MVSinceKF += UsableMotionVectors;
	ID.Frames++;

	return true;
}

void MotionVectorFilter::ClearStateThis()
{
	auto& ID = GetData();
	ID.ClearObjectData.store(true);
}

void MotionVectorFilter::UpdateROI( SharedClassificationTask TaskData )
{
	auto& ID = GetData();

	const unsigned int WidthBucketWidth = TaskData->Frame.InputFrame->GetWidth() / BUCKET_DIMENSION;
	const unsigned int HeightBucketHeight = TaskData->Frame.InputFrame->GetHeight() / BUCKET_DIMENSION;

	float RescaleX = (float)TaskData->Frame.InputFrame->GetWidth() / (float)(WidthBucketWidth);
	float RescaleY = (float)TaskData->Frame.InputFrame->GetHeight() / (float)(HeightBucketHeight);

	for (auto Iter = ID.Objects.begin(); Iter != ID.Objects.end(); Iter++ )
	{				
		cv::Rect ExpandedObject = (*Iter).Region;

		for( auto& ROI : TaskData->Result.ROI )
		{
			cv::Rect ROIRect;
			ROIRect.x = ROI.Left;
			ROIRect.y = ROI.Top;
			ROIRect.width = ROI.Width;
			ROIRect.height = ROI.Height;

			cv::Rect IntersectedRect = ROIRect & ExpandedObject;
			if( IntersectedRect.area() > 0 || ROI.TrackingID == (*Iter).ObjectID )
			{
				(*Iter).Classification |= ROI.Classification;
				(*Iter).ClassificationConfidence = ROI.ClassificationConfidence;
				(*Iter).ClassificationGroup |= ROI.ClassificationGroup;
				(*Iter).CustomLabel = ROI.CustomLabel;
			}
		}
	}
}

}}
