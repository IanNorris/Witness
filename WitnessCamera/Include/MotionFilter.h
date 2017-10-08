#pragma once

#include "RecordFilterBase.h"

namespace Witness{
namespace Camera{

struct MotionFilterData;

class CAMERA_API MotionFilter : public RecordFilterBase<MotionFilterData>
{
public:

	MotionFilter();
	virtual ~MotionFilter();

	virtual const char* FilterFrame( unsigned int Width, unsigned int Height, void* Data, StreamManager* StreamManager );
};

}}
