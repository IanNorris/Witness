#pragma once

#include "RecordFilterBase.h"

namespace Witness{
namespace Camera{

struct MotionFilterData;

class CAMERA_API MotionFilter : public RecordFilterBase<MotionFilterData>
{
public:

	MotionFilter( const MotionChainNode& Chain, const char* FilterName );
	virtual ~MotionFilter();

	virtual bool ProcessFrame( SharedClassificationTask TaskData );
};

}}
