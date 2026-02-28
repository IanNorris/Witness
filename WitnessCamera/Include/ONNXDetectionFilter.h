#pragma once

#include "RecordFilterBase.h"

#include <string>
#include <vector>

namespace cv { class Mat; }

namespace Witness{
namespace Camera{

struct ONNXDetectionFilterData;

struct CAMERA_API DetectionResult
{
	int ClassId;
	float Confidence;
	std::string ClassName;
};

class CAMERA_API ONNXDetectionFilter : public RecordFilterBase<ONNXDetectionFilterData>
{
public:

	ONNXDetectionFilter( const MotionChainNode& Chain, const char* ModelPath, float ConfidenceThreshold, bool UseGPU, float MaxFPS = 0.0f );
	virtual ~ONNXDetectionFilter();

	virtual bool ProcessFrame( SharedClassificationTask TaskData );

	bool IsModelLoaded() const;

	std::vector<DetectionResult> DetectFrame( const cv::Mat& bgrFrame );

private:

	bool m_ModelLoaded;
};

}}
