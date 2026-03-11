#pragma once

#include "RecordFilterBase.h"

#include <string>
#include <vector>

namespace cv { class Mat; }

namespace Witness{
namespace Camera{

struct FaceDetectionFilterData;

// Result of a single face detection within a person crop
struct CAMERA_API FaceDetectionResult
{
	// Bounding box in full-frame pixel coordinates
	float X1, Y1, X2, Y2;
	float Confidence;
	// 5-point landmarks in full-frame pixel coordinates
	// [0]=right eye, [1]=left eye, [2]=nose, [3]=right mouth, [4]=left mouth
	float LandmarkX[5];
	float LandmarkY[5];
};

class CAMERA_API FaceDetectionFilter : public RecordFilterBase<FaceDetectionFilterData>
{
public:

	FaceDetectionFilter( const MotionChainNode& Chain, const char* ModelPath, float ConfidenceThreshold );
	virtual ~FaceDetectionFilter();

	virtual bool ProcessFrame( SharedClassificationTask TaskData );

	bool IsModelLoaded() const;

	// Detect faces in a BGR frame (full-frame coordinates returned)
	std::vector<FaceDetectionResult> DetectFaces( const cv::Mat& bgrFrame );

	// Detect faces only within person ROI regions from prior YOLO detection
	std::vector<FaceDetectionResult> DetectFacesInPersonCrops(
		const cv::Mat& bgrFrame,
		const std::vector<ClassificationResult::RegionOfInterest>& ROIs );

private:

	bool m_ModelLoaded;
};

}}
