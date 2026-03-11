#include "FaceDetectionFilter.h"
#include "FilterData.h"

#include <opencv2/core/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/objdetect/face.hpp>

#include <vector>
#include <string>
#include <algorithm>
#include <filesystem>
#include <Log.h>

#include <chrono>

namespace Witness{
namespace Camera{

// Face class uses ClassID 100, outside COCO range (0-79)
static constexpr int FACE_CLASS_ID = 100;
static constexpr unsigned int FACE_CLASS_FLAG = 1 << 19;  // Motion_Face — new flag

struct FaceDetectionFilterData : public FilterDataBase
{
	FaceDetectionFilterData()
	: ConfidenceThreshold( 0.7f )
	{}

	cv::Ptr<cv::FaceDetectorYN> Detector;
	float ConfidenceThreshold;

	// Stats
	uint64_t FramesReceived = 0;
	uint64_t FramesProcessed = 0;
	uint64_t FacesDetected = 0;
	double TotalInferenceMs = 0.0;
	std::chrono::steady_clock::time_point LastStatsTime;
};

PIMPL_CONSTRUCT(FaceDetectionFilterData)


FaceDetectionFilter::FaceDetectionFilter( const MotionChainNode& Chain, const char* ModelPath, float ConfidenceThreshold )
: RecordFilterBase( Chain )
, m_ModelLoaded( false )
{
	auto& Data = GetData();
	Data.ConfidenceThreshold = ConfidenceThreshold;
	Data.LastStatsTime = std::chrono::steady_clock::now();

	if( !ModelPath || !std::filesystem::exists( ModelPath ) )
	{
		LOG_WARNING( "FaceDetection: Model file not found: %s", ModelPath ? ModelPath : "(null)" );
		return;
	}

	try
	{
		// Create with a default input size — we resize per-crop before each detection
		Data.Detector = cv::FaceDetectorYN::create(
			ModelPath,
			"",  // no config file for ONNX models
			cv::Size( 320, 320 ),
			ConfidenceThreshold,
			0.3f,  // NMS threshold
			5000   // top K
		);

		m_ModelLoaded = true;
		LOG_INFO( "FaceDetection: Model loaded from %s (confidence >= %.2f)", ModelPath, ConfidenceThreshold );
	}
	catch( const cv::Exception& e )
	{
		LOG_WARNING( "FaceDetection: Failed to load model: %s", e.what() );
	}
}

FaceDetectionFilter::~FaceDetectionFilter()
{
}

bool FaceDetectionFilter::IsModelLoaded() const
{
	return m_ModelLoaded;
}

std::vector<FaceDetectionResult> FaceDetectionFilter::DetectFaces( const cv::Mat& bgrFrame )
{
	std::vector<FaceDetectionResult> results;
	if( !m_ModelLoaded || bgrFrame.empty() )
		return results;

	auto& Data = GetData();

	// Set input size to match the frame
	Data.Detector->setInputSize( cv::Size( bgrFrame.cols, bgrFrame.rows ) );

	cv::Mat faces;
	Data.Detector->detect( bgrFrame, faces );

	if( !faces.empty() )
	{
		LOG_DEBUG( "FaceDetection: YuNet returned %d candidates on %dx%d crop", faces.rows, bgrFrame.cols, bgrFrame.rows );
		for( int i = 0; i < faces.rows; i++ )
		{
			float score = faces.at<float>( i, 14 );
			LOG_DEBUG( "FaceDetection:   candidate %d: score=%.3f (threshold=%.3f)", i, score, Data.ConfidenceThreshold );
		}
	}

	if( faces.empty() )
		return results;

	// Each row in faces is [x, y, w, h, 5 landmark pairs (10 values), score] = 15 columns
	// Score is at column 14 (NOT column 4 — column 4 is the first landmark X)
	for( int i = 0; i < faces.rows; i++ )
	{
		float confidence = faces.at<float>( i, 14 );
		if( confidence < Data.ConfidenceThreshold )
			continue;

		FaceDetectionResult r;
		float x = faces.at<float>( i, 0 );
		float y = faces.at<float>( i, 1 );
		float w = faces.at<float>( i, 2 );
		float h = faces.at<float>( i, 3 );
		r.X1 = x;
		r.Y1 = y;
		r.X2 = x + w;
		r.Y2 = y + h;
		r.Confidence = confidence;

		// 5-point landmarks: right eye, left eye, nose, right mouth, left mouth
		for( int j = 0; j < 5; j++ )
		{
			r.LandmarkX[j] = faces.at<float>( i, 4 + j * 2 );
			r.LandmarkY[j] = faces.at<float>( i, 5 + j * 2 );
		}

		results.push_back( r );
	}

	return results;
}

std::vector<FaceDetectionResult> FaceDetectionFilter::DetectFacesInPersonCrops(
	const cv::Mat& bgrFrame,
	const std::vector<ClassificationResult::RegionOfInterest>& ROIs )
{
	std::vector<FaceDetectionResult> allFaces;
	if( !m_ModelLoaded || bgrFrame.empty() )
		return allFaces;

	for( auto& roi : ROIs )
	{
		// Only process person detections
		if( ( roi.Classification & ClassificationResult::Motion_Person ) == 0 )
			continue;

		// Expand the person crop slightly to capture faces at the edges
		int expandX = static_cast<int>( roi.Width * 0.1f );
		int expandY = static_cast<int>( roi.Height * 0.1f );

		int cropX = std::max( 0, static_cast<int>( roi.Left ) - expandX );
		int cropY = std::max( 0, static_cast<int>( roi.Top ) - expandY );
		int cropW = std::min( bgrFrame.cols - cropX, static_cast<int>( roi.Width ) + expandX * 2 );
		int cropH = std::min( bgrFrame.rows - cropY, static_cast<int>( roi.Height ) + expandY * 2 );

		if( cropW < 20 || cropH < 20 )
			continue;  // Too small for face detection

		cv::Rect cropRect( cropX, cropY, cropW, cropH );
		cv::Mat personCrop = bgrFrame( cropRect );

		auto faces = DetectFaces( personCrop );

		// Convert face coordinates from crop-relative to full-frame
		for( auto& face : faces )
		{
			face.X1 += cropX;
			face.Y1 += cropY;
			face.X2 += cropX;
			face.Y2 += cropY;

			for( int j = 0; j < 5; j++ )
			{
				face.LandmarkX[j] += cropX;
				face.LandmarkY[j] += cropY;
			}

			// Clamp to frame bounds
			face.X1 = std::max( 0.0f, std::min( face.X1, static_cast<float>( bgrFrame.cols ) ) );
			face.Y1 = std::max( 0.0f, std::min( face.Y1, static_cast<float>( bgrFrame.rows ) ) );
			face.X2 = std::max( 0.0f, std::min( face.X2, static_cast<float>( bgrFrame.cols ) ) );
			face.Y2 = std::max( 0.0f, std::min( face.Y2, static_cast<float>( bgrFrame.rows ) ) );

			allFaces.push_back( face );
		}
	}

	return allFaces;
}

bool FaceDetectionFilter::ProcessFrame( SharedClassificationTask TaskData )
{
	auto& Data = GetData();
	Data.FramesReceived++;

	if( !m_ModelLoaded )
		return true;  // Pass through if model not loaded

	// Only run face detection on frames that contain people
	bool hasPerson = false;
	for( auto& roi : TaskData->Result.ROI )
	{
		if( ( roi.Classification & ClassificationResult::Motion_Person ) != 0 )
		{
			hasPerson = true;
			break;
		}
	}

	if( !hasPerson )
		return true;  // No people detected, pass through

	auto& decodedFrame = TaskData->Frame.GetOrDecodeFrame();
	if( decodedFrame.empty() )
	{
		LOG_DEBUG( "FaceDetection: decoded frame is empty, skipping" );
		return true;
	}

	LOG_DEBUG( "FaceDetection: Processing frame with %zu ROIs (%dx%d)",
		TaskData->Result.ROI.size(), decodedFrame.cols, decodedFrame.rows );

	auto startTime = std::chrono::steady_clock::now();

	auto faces = DetectFacesInPersonCrops( decodedFrame, TaskData->Result.ROI );

	auto endTime = std::chrono::steady_clock::now();
	double elapsedMs = std::chrono::duration<double, std::milli>( endTime - startTime ).count();

	Data.FramesProcessed++;
	Data.TotalInferenceMs += elapsedMs;
	Data.FacesDetected += faces.size();

	LOG_DEBUG( "FaceDetection: %zu faces found in %.1fms (total: %llu faces from %llu frames)",
		faces.size(), elapsedMs, Data.FacesDetected, Data.FramesProcessed );

	// Add face ROIs to TaskData
	for( auto& face : faces )
	{
		ClassificationResult::RegionOfInterest ROI;
		ROI.Left = static_cast<unsigned int>( std::max( 0.0f, face.X1 ) );
		ROI.Top = static_cast<unsigned int>( std::max( 0.0f, face.Y1 ) );
		ROI.Width = static_cast<unsigned int>( face.X2 - face.X1 );
		ROI.Height = static_cast<unsigned int>( face.Y2 - face.Y1 );
		ROI.Classification = FACE_CLASS_FLAG;
		ROI.ClassificationConfidence = face.Confidence;
		ROI.Tags.push_back( "face" );
		ROI.Filter = this;

		// Store 5-point landmarks in pixel coordinates
		ROI.HasLandmarks = true;
		for( int j = 0; j < 5; j++ )
		{
			ROI.LandmarkX[j] = face.LandmarkX[j];
			ROI.LandmarkY[j] = face.LandmarkY[j];
		}

		TaskData->Result.ROI.push_back( ROI );
		TaskData->Result.ClassificationSuperset |= FACE_CLASS_FLAG;
	}

	if( !faces.empty() )
	{
		TaskData->Result.Tags.push_back( "face" );
	}

	// Periodic stats logging (every 60 seconds)
	auto now = std::chrono::steady_clock::now();
	double secSinceStats = std::chrono::duration<double>( now - Data.LastStatsTime ).count();
	if( secSinceStats >= 60.0 )
	{
		if( Data.FramesProcessed > 0 )
		{
			LOG_INFO( "FaceDetection: %llu frames, %llu faces, avg %.1fms/frame",
				Data.FramesProcessed, Data.FacesDetected,
				Data.TotalInferenceMs / Data.FramesProcessed );
		}
		Data.LastStatsTime = now;
	}

	return true;
}

}}
