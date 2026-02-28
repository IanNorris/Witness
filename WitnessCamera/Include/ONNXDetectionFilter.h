#pragma once

#include "RecordFilterBase.h"

#include <string>

namespace Witness{
namespace Camera{

struct ONNXDetectionFilterData;

class CAMERA_API ONNXDetectionFilter : public RecordFilterBase<ONNXDetectionFilterData>
{
public:

	ONNXDetectionFilter( const MotionChainNode& Chain, const char* ModelPath, float ConfidenceThreshold, bool UseGPU, float MaxFPS = 0.0f );
	virtual ~ONNXDetectionFilter();

	virtual bool ProcessFrame( SharedClassificationTask TaskData );

	bool IsModelLoaded() const;

private:

	bool m_ModelLoaded;
};

}}
