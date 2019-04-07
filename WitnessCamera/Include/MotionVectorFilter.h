#pragma once

#include "RecordFilterBase.h"

namespace Witness{
namespace Camera{

struct MotionVectorFilterData;

class CAMERA_API MotionVectorFilter : public RecordFilterBase<MotionVectorFilterData>
{
public:

	MotionVectorFilter( const MotionChainNode& Chain, const wchar_t* BlackoutMaskPath, const wchar_t* FocusMaskPath );
	virtual ~MotionVectorFilter();

	void UpdateMasks( unsigned int Width, unsigned int Height );

	virtual bool ProcessFrame( SharedClassificationTask TaskData ) override;
	virtual void ClearStateThis() override;
	virtual void UpdateROI( SharedClassificationTask TaskData ) override;
};

}}
