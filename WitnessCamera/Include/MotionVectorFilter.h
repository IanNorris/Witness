#pragma once

#include "RecordFilterBase.h"

namespace Witness{
namespace Camera{

struct MotionVectorFilterData;

class CAMERA_API MotionVectorFilter : public RecordFilterBase<MotionVectorFilterData>
{
public:

	MotionVectorFilter( const MotionChainNode& Chain, const char* BlackoutMaskPath, const char* FocusMaskPath );
	virtual ~MotionVectorFilter();

	void UpdateMasks( unsigned int Width, unsigned int Height );

	virtual bool ProcessFrame( SharedClassificationTask TaskData ) override;
	virtual void ClearStateThis() override;
	virtual void UpdateROI( SharedClassificationTask TaskData ) override;
};

}}
