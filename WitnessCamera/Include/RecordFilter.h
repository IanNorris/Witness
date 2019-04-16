#pragma once

#include "Export.h"
#include "Pimpl.h"
#include "FFMPEG/Frame.h"

#include "SourceStats.h"

#include <opencv2/core/mat.hpp>

#include <memory>
#include <type_traits>
#include <memory>
#include <string>
#include <functional>

struct AVFrame;
struct SwsContext;

namespace Witness{
namespace Camera{

struct FilterData;
class StreamManager;
class IRecordFilter;

class CAMERA_API FilterFrameContext
{
public:

	FilterFrameContext()
	: ConversionContext( nullptr )
	{}


	SwsContext* ConversionContext;
};

class CAMERA_API FilterFrame
{
public:

	FilterFrame( 
		FilterFrameStats& StatsIn, 
		std::shared_ptr<FFMPEG::Frame>& InputFrameIn,
		std::shared_ptr<FFMPEG::Frame>& OutputFrameIn,
		cv::Mat& DecodedFrameIn,
		cv::Mat& GrayscaleDecodedFrameIn,
		std::shared_ptr<FilterFrameContext>& FrameContextIn )
	: Stats( StatsIn )
	, InputFrame( InputFrameIn )
	, WantFullSizeOutput( false )
	, WantSmallOutput( false )
	, OutputFrame( OutputFrameIn )
	, DecodedFrame( DecodedFrameIn )
	, GrayscaleDecodedFrame( GrayscaleDecodedFrameIn )
	, FrameContext( FrameContextIn )
	{}

	cv::Mat& GetOrDecodeFrame();
	cv::Mat& GetOrDecodeGrayscaleInputFrame();

	FilterFrameStats& Stats;

	std::shared_ptr<FFMPEG::Frame>& InputFrame;
	std::shared_ptr<FilterFrameContext>& FrameContext;

	int64_t							Timestamp;
	unsigned int					TargetHeight;
	int								SourceID;

	bool WantFullSizeOutput;
	bool WantSmallOutput;

private:

	std::shared_ptr<FFMPEG::Frame>& OutputFrame;

	cv::Mat& DecodedFrame;
	cv::Mat& GrayscaleDecodedFrame;
};

class FilterFrameOwner
{
public:

	FilterFrameOwner( const std::shared_ptr<FFMPEG::Frame>& InputFrameIn, const std::shared_ptr<FilterFrameContext>& FrameContextIn )
	: InputFrame( InputFrameIn )
	, FrameContext( FrameContextIn )
	, HasLiveViewer( false )
	{}

	FilterFrame GetFilterFrame()
	{
		return FilterFrame( Stats, InputFrame, OutputFrame, DecodedFrame, GrayscaleDecodedFrame, FrameContext );
	}

	std::shared_ptr<FFMPEG::Frame> InputFrame;
	std::shared_ptr<FFMPEG::Frame> OutputFrame;
	std::shared_ptr<FilterFrameContext> FrameContext;

	cv::Mat DecodedFrame;
	cv::Mat GrayscaleDecodedFrame;

	FilterFrameStats Stats;

	bool HasLiveViewer;
};

struct ClassificationResult
{
	enum ClassificationFlag
	{
		Motion_None					= 0,
		Motion_Motion				= 1 << 0,

		Motion_Animal				= 1 << 1,
		Motion_Animal_Cat			= 1 << 2,
		Motion_Animal_Dog			= 1 << 3,

		Motion_Vehicle				= 1 << 8,
		Motion_Vehicle_Recognized	= 1 << 10,
		Motion_Vehicle_HighRisk		= 1 << 11,

		Motion_Person				= 1 << 15,
		Motion_Person_Recognized	= 1 << 17,
		Motion_Person_HighRisk		= 1 << 18,

		Motion_CustomTag			= 1 << 31
	};

	struct RegionOfInterest
	{
		RegionOfInterest()
		: Filter( nullptr )
		, TrackingID( 0 )
		, Left( 0 )
		, Top( 0 )
		, Width( 0 )
		, Height( 0 )
		, Classification( 0 )
		, ClassificationGroup( 0 )
		, ClassificationConfidence( 0.0 )
		{}

		std::string	CustomLabel;

		IRecordFilter* Filter;
		unsigned int TrackingID;

		unsigned int Left;
		unsigned int Top;
		unsigned int Width;
		unsigned int Height;

		unsigned int Classification;
		unsigned int ClassificationGroup;

		float ClassificationConfidence;
	};
	
	ClassificationResult()
	: ClassificationSuperset( Motion_None )
	, MotionAmount( 0.0f )
	{}

	std::vector<RegionOfInterest> ROI;

	unsigned int ClassificationSuperset;
	float MotionAmount;
};

class ClassificationTask;
class CAMERA_API IRecordFilter;

typedef std::shared_ptr<ClassificationTask> SharedClassificationTask;

class ClassificationTask
{
public:

	ClassificationTask( const std::shared_ptr<FilterFrameOwner>& FrameOwnerIn )
	: FrameOwner( FrameOwnerIn )
	, Frame( FrameOwnerIn->GetFilterFrame() )
	, Result()
	{}
	
	std::shared_ptr<FilterFrameOwner> FrameOwner;

	std::shared_ptr<IRecordFilter> Origin;
	std::shared_ptr<IRecordFilter> Next;

	std::function<void(SharedClassificationTask,bool)> InsertToQueue;

	FilterFrame Frame;
	ClassificationResult Result;
};

enum class ETaskType
{
	AutoContinuation,
	ManualContinuation,
};

struct MotionChainNode;

struct MotionChainNode
{
	MotionChainNode()
	{
		InclusiveFilter = ~0U;
		ExclusiveFilter = 0;
		MinimumThreshold = 0.0f;
	}

	std::shared_ptr<IRecordFilter> OnSuccess;
	std::shared_ptr<IRecordFilter> OnFailure;

	unsigned int InclusiveFilter; //Mask that must be matched for success
	unsigned int ExclusiveFilter; //Mask that must not be matched for success

	float MinimumThreshold;
};

class CAMERA_API IRecordFilter
{
public:
	
	IRecordFilter( const MotionChainNode& NextChain )
	{
		Chain = new MotionChainNode();
		*Chain = NextChain;
	}

	virtual ~IRecordFilter()
	{
		delete Chain;
		Chain = nullptr;
	}

	virtual ETaskType GetTaskType() { return ETaskType::AutoContinuation; }

	void Continue( SharedClassificationTask TaskData, bool Success )
	{
		bool MotionSuccess = (TaskData->Result.ClassificationSuperset & Chain->InclusiveFilter) != 0
					&&	(TaskData->Result.ClassificationSuperset & Chain->ExclusiveFilter) == 0
					&&	TaskData->Result.MotionAmount >= Chain->MinimumThreshold;
		
		TaskData->Next = MotionSuccess ? Chain->OnSuccess : Chain->OnFailure;

		if( TaskData->Result.ClassificationSuperset > ClassificationResult::Motion_Motion )
		{
			TaskData->Origin->UpdateROITree( TaskData );
		}

		TaskData->InsertToQueue( TaskData, true );
	}

	void DoWork( SharedClassificationTask TaskData )
	{
		bool Result = ProcessFrame( TaskData );

		if( GetTaskType() == ETaskType::AutoContinuation )
		{
			Continue( TaskData, Result );
		}
	}
	
	virtual bool ProcessFrame( SharedClassificationTask TaskData ) = 0;

	virtual void ClearStateThis() {};

	virtual void UpdateROI( SharedClassificationTask TaskData ) {}

	void UpdateROITree( SharedClassificationTask TaskData )
	{
		UpdateROI( TaskData );

		if( Chain->OnSuccess )
		{
			Chain->OnSuccess->UpdateROITree( TaskData );
		}

		if( Chain->OnFailure )
		{
			Chain->OnFailure->UpdateROITree( TaskData );
		}
	}
	
	void ClearState()
	{
		ClearStateThis();

		if( Chain->OnSuccess )
		{
			Chain->OnSuccess->ClearState();
		}

		if( Chain->OnFailure )
		{
			Chain->OnFailure->ClearState();
		}
	}

private:

	MotionChainNode* Chain;
};

}}
