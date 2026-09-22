#pragma once

#include "RecordFilterBase.h"

namespace Witness{
namespace Camera{

struct MotionVectorFilterData;

// Legacy defaults, now owned by each filter so calibration can vary by camera.
// Values remain in the legacy raw-vector units until a versioned algorithm
// change explicitly migrates them.
struct MotionVectorTuning
{
	int BucketRefValue = 12;
	int MinBlockMoveDistance = 4;
	int MaxBlockMoveDistance = 128000;
	int MinVectorCount = 2;
	int MinClusterPoints = 2;
	float MinRatioOfBounds = 0.15f;
	int MinTrackingFrames = 3;
};

class CAMERA_API MotionVectorFilter : public RecordFilterBase<MotionVectorFilterData>
{
public:

	MotionVectorFilter( const MotionChainNode& Chain, const char* BlackoutMaskPath, const char* FocusMaskPath,
		const MotionVectorTuning& Tuning = {} );
	virtual ~MotionVectorFilter();

	void UpdateMasks( unsigned int Width, unsigned int Height );

	virtual bool ProcessFrame( SharedClassificationTask TaskData ) override;
	virtual void ClearStateThis() override;
	virtual void UpdateROI( SharedClassificationTask TaskData ) override;
};

}}
