#pragma once

#include "RecordFilterBase.h"

namespace Witness{
namespace Camera{

struct MotionFilterData;

class CAMERA_API MotionFilter : public RecordFilterBase<MotionFilterData>
{
public:

	MotionFilter( double MotionThreshold );
	virtual ~MotionFilter();

	virtual ClassificationResult FilterFrame( unsigned int Width, unsigned int Height, void* Data, StreamManager* StreamManager );

private:

	double MotionThreshold;
};

}}
