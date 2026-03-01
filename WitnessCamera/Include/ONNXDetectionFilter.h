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

	ONNXDetectionFilter( const MotionChainNode& Chain, const char* ModelPath, float ConfidenceThreshold, bool UseGPU, float MaxFPS = 0.0f, const char* CudnnPath = nullptr );
	virtual ~ONNXDetectionFilter();

	virtual bool ProcessFrame( SharedClassificationTask TaskData );

	bool IsModelLoaded() const;

	std::vector<DetectionResult> DetectFrame( const cv::Mat& bgrFrame );

private:

	bool m_ModelLoaded;
};

// Test CUDA availability by creating a probe ONNX session.
// CudnnPath: optional cuDNN root directory (nullptr for auto-scan).
// ModelPath: ONNX model to test with (nullptr uses a minimal internal test).
// Returns true if CUDA+cuDNN work correctly.
CAMERA_API bool TestCudaAvailability( const char* ModelPath, const char* CudnnPath = nullptr );

// Spawn a child process to test CUDA (safe against __fastfail crashes).
// Returns true if the probe process exits successfully.
CAMERA_API bool TestCudaViaProbe( const char* CudnnPath = nullptr );

}}
