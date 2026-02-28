#include "ONNXDetectionFilter.h"
#include "FilterData.h"

#include <onnxruntime_cxx_api.h>

#include <opencv2/core/core.hpp>
#include <opencv2/imgproc.hpp>

#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#endif

#include <vector>
#include <string>
#include <array>
#include <algorithm>
#include <Log.h>

#include <chrono>
#include <ctime>

namespace Witness{
namespace Camera{

// COCO class ID to ClassificationFlag mapping
static unsigned int COCOClassToFlag( int ClassID )
{
	switch( ClassID )
	{
		case 0: return ClassificationResult::Motion_Person;
		case 1: // bicycle
		case 2: // car
		case 3: // motorcycle
		case 5: // bus
		case 7: // truck
			return ClassificationResult::Motion_Vehicle;
		case 14: // bird
		case 17: // horse
		case 18: // sheep
		case 19: // cow
		case 20: // elephant
		case 21: // bear
		case 22: // zebra
		case 23: // giraffe
			return ClassificationResult::Motion_Animal;
		case 15: return ClassificationResult::Motion_Animal_Cat;
		case 16: return ClassificationResult::Motion_Animal_Dog;
		default: return ClassificationResult::Motion_Motion;
	}
}

static const char* COCOClassNames[] = {
	"person", "bicycle", "car", "motorcycle", "airplane", "bus", "train", "truck", "boat",
	"traffic light", "fire hydrant", "stop sign", "parking meter", "bench", "bird", "cat",
	"dog", "horse", "sheep", "cow", "elephant", "bear", "zebra", "giraffe", "backpack",
	"umbrella", "handbag", "tie", "suitcase", "frisbee", "skis", "snowboard", "sports ball",
	"kite", "baseball bat", "baseball glove", "skateboard", "surfboard", "tennis racket",
	"bottle", "wine glass", "cup", "fork", "knife", "spoon", "bowl", "banana", "apple",
	"sandwich", "orange", "broccoli", "carrot", "hot dog", "pizza", "donut", "cake", "chair",
	"couch", "potted plant", "bed", "dining table", "toilet", "tv", "laptop", "mouse",
	"remote", "keyboard", "cell phone", "microwave", "oven", "toaster", "sink", "refrigerator",
	"book", "clock", "vase", "scissors", "teddy bear", "hair drier", "toothbrush"
};
static const int NumCOCOClasses = 80;

struct ONNXDetectionFilterData : public FilterDataBase
{
	ONNXDetectionFilterData()
	: Env( nullptr )
	, ConfidenceThreshold( 0.5f )
	, InputWidth( 640 )
	, InputHeight( 640 )
	{}

	std::unique_ptr<Ort::Env> Env;
	std::unique_ptr<Ort::Session> Session;
	Ort::SessionOptions SessionOptions;

	std::vector<std::string> InputNames;
	std::vector<std::string> OutputNames;
	std::vector<const char*> InputNamePtrs;
	std::vector<const char*> OutputNamePtrs;

	float ConfidenceThreshold;
	int InputWidth;
	int InputHeight;

	// Reusable buffers to avoid per-frame allocation
	std::vector<float> InputTensorData;
	cv::Mat ResizedFrame;
	cv::Mat FloatFrame;

	// Throttle: max detections per second (0 = unlimited)
	float MaxFPS;
	std::chrono::steady_clock::time_point LastInferenceTime;

	// Stats (periodic logging)
	uint64_t FramesReceived;
	uint64_t FramesProcessed;
	uint64_t FramesDetected;
	uint64_t FramesSkippedThrottle;
	uint64_t FramesBaselineFiltered;
	double TotalInferenceMs;
	std::chrono::steady_clock::time_point LastStatsTime;

	// Baseline: objects that are always present in the scene
	struct BaselineObject
	{
		int ClassId;
		float X1, Y1, X2, Y2;  // Normalized to [0,1] relative to frame
	};
	std::vector<BaselineObject> Baseline;
	std::chrono::steady_clock::time_point LastBaselineTime;
	float BaselineIntervalSec;
	bool BaselineInitialized;
};

PIMPL_CONSTRUCT(ONNXDetectionFilterData)

ONNXDetectionFilter::ONNXDetectionFilter( const MotionChainNode& Chain, const char* ModelPath, float ConfidenceThreshold, bool UseGPU, float MaxFPS )
: RecordFilterBase( Chain )
, m_ModelLoaded( false )
{
	auto& ID = GetData();
	ID.ConfidenceThreshold = ConfidenceThreshold;
	ID.MaxFPS = MaxFPS;
	ID.LastInferenceTime = std::chrono::steady_clock::time_point{};
	ID.FramesReceived = 0;
	ID.FramesProcessed = 0;
	ID.FramesDetected = 0;
	ID.FramesSkippedThrottle = 0;
	ID.FramesBaselineFiltered = 0;
	ID.TotalInferenceMs = 0.0;
	ID.LastStatsTime = std::chrono::steady_clock::now();
	ID.LastBaselineTime = std::chrono::steady_clock::time_point{};
	ID.BaselineIntervalSec = 30.0f;
	ID.BaselineInitialized = false;

	try
	{
		ID.Env = std::make_unique<Ort::Env>( ORT_LOGGING_LEVEL_WARNING, "witness_detection" );

		ID.SessionOptions.SetIntraOpNumThreads( 1 );
		ID.SessionOptions.SetGraphOptimizationLevel( GraphOptimizationLevel::ORT_ENABLE_ALL );

		if( UseGPU )
		{
			// The onnxruntime-gpu package includes CUDA support
			// CUDA will be used if available, otherwise falls back to CPU
			try
			{
				OrtCUDAProviderOptions cudaOptions;
				cudaOptions.device_id = 0;
				ID.SessionOptions.AppendExecutionProvider_CUDA( cudaOptions );
				LOG_INFO( "ONNX Detection: CUDA GPU acceleration enabled." );
			}
			catch( const Ort::Exception& e )
			{
				LOG_WARNING( "ONNX Detection: CUDA unavailable (%s), using CPU.", e.what() );
			}
		}

		// Convert to wide string on Windows for ONNX Runtime
#ifdef _WIN32
		int len = MultiByteToWideChar( CP_UTF8, 0, ModelPath, -1, nullptr, 0 );
		std::wstring widePath( len, 0 );
		MultiByteToWideChar( CP_UTF8, 0, ModelPath, -1, &widePath[0], len );
		ID.Session = std::make_unique<Ort::Session>( *ID.Env, widePath.c_str(), ID.SessionOptions );
#else
		ID.Session = std::make_unique<Ort::Session>( *ID.Env, ModelPath, ID.SessionOptions );
#endif

		Ort::AllocatorWithDefaultOptions allocator;

		// Get input info
		size_t numInputs = ID.Session->GetInputCount();
		for( size_t i = 0; i < numInputs; i++ )
		{
			auto name = ID.Session->GetInputNameAllocated( i, allocator );
			ID.InputNames.push_back( name.get() );
		}

		// Get output info
		size_t numOutputs = ID.Session->GetOutputCount();
		for( size_t i = 0; i < numOutputs; i++ )
		{
			auto name = ID.Session->GetOutputNameAllocated( i, allocator );
			ID.OutputNames.push_back( name.get() );
		}

		// Cache raw pointers for Run()
		for( auto& name : ID.InputNames )
			ID.InputNamePtrs.push_back( name.c_str() );
		for( auto& name : ID.OutputNames )
			ID.OutputNamePtrs.push_back( name.c_str() );

		// Get input dimensions
		auto inputInfo = ID.Session->GetInputTypeInfo( 0 );
		auto tensorInfo = inputInfo.GetTensorTypeAndShapeInfo();
		auto inputShape = tensorInfo.GetShape();
		if( inputShape.size() == 4 )
		{
			ID.InputHeight = (int)inputShape[2];
			ID.InputWidth = (int)inputShape[3];
		}

		// Pre-allocate input tensor buffer
		ID.InputTensorData.resize( 3 * ID.InputWidth * ID.InputHeight );

		m_ModelLoaded = true;

		// Log output tensor shape for diagnostics
		auto outInfo = ID.Session->GetOutputTypeInfo( 0 );
		auto outTensorInfo = outInfo.GetTensorTypeAndShapeInfo();
		auto outShape = outTensorInfo.GetShape();
		{
			char shapeBuf[256];
			int pos = 0;
			for( size_t i = 0; i < outShape.size(); i++ )
				pos += snprintf( shapeBuf + pos, sizeof(shapeBuf) - pos, "%s%lld", i > 0 ? ", " : "", (long long)outShape[i] );
			LOG_INFO( "ONNX Detection: Model loaded (%s), input %dx%d, output [%s], confidence >= %.0f%%.",
				ModelPath, ID.InputWidth, ID.InputHeight, shapeBuf, ID.ConfidenceThreshold * 100.0f );
		}
	}
	catch( const Ort::Exception& e )
	{
		LOG_ERROR( "ONNX Detection: Failed to load model '%s': %s", ModelPath, e.what() );
	}
	catch( const std::exception& e )
	{
		LOG_ERROR( "ONNX Detection: Failed to load model '%s': %s", ModelPath, e.what() );
	}
}

ONNXDetectionFilter::~ONNXDetectionFilter()
{}

bool ONNXDetectionFilter::IsModelLoaded() const
{
	return m_ModelLoaded;
}

static float ComputeIoU( float ax1, float ay1, float ax2, float ay2,
                          float bx1, float by1, float bx2, float by2 )
{
	float ix1 = std::max( ax1, bx1 );
	float iy1 = std::max( ay1, by1 );
	float ix2 = std::min( ax2, bx2 );
	float iy2 = std::min( ay2, by2 );

	float iw = std::max( 0.0f, ix2 - ix1 );
	float ih = std::max( 0.0f, iy2 - iy1 );
	float intersection = iw * ih;

	float areaA = ( ax2 - ax1 ) * ( ay2 - ay1 );
	float areaB = ( bx2 - bx1 ) * ( by2 - by1 );
	float unionArea = areaA + areaB - intersection;

	return unionArea > 0.0f ? intersection / unionArea : 0.0f;
}

bool ONNXDetectionFilter::ProcessFrame( SharedClassificationTask TaskData )
{
	if( !m_ModelLoaded )
		return false;

	auto& ID = GetData();
	auto now = std::chrono::steady_clock::now();

	bool hasMotion = ( TaskData->Result.ClassificationSuperset & ClassificationResult::Motion_Motion ) != 0;

	if( !hasMotion )
	{
		// No-motion frame: only process if it's time for a baseline update
		double baselineElapsed = std::chrono::duration<double>( now - ID.LastBaselineTime ).count();
		if( ID.BaselineInitialized && baselineElapsed < ID.BaselineIntervalSec )
			return false;
	}
	else
	{
		ID.FramesReceived++;

		// Throttle: skip if we've inferred too recently (motion frames only)
		if( ID.MaxFPS > 0.0f )
		{
			double elapsedMs = std::chrono::duration<double, std::milli>( now - ID.LastInferenceTime ).count();
			double minIntervalMs = 1000.0 / ID.MaxFPS;
			if( elapsedMs < minIntervalMs )
			{
				ID.FramesSkippedThrottle++;
				return false;
			}
		}
	}

	// Periodic stats logging (every 60 seconds)
	{
		double statsSec = std::chrono::duration<double>( now - ID.LastStatsTime ).count();
		if( statsSec >= 60.0 && ID.FramesProcessed > 0 )
		{
			double avgMs = ID.TotalInferenceMs / ID.FramesProcessed;
			LOG_INFO( "ONNX Camera %d stats: %llu/%llu detected (%.0f%%), %llu skipped, %llu baseline-filtered, avg %.0fms",
				TaskData->Frame.SourceID,
				(unsigned long long)ID.FramesDetected,
				(unsigned long long)ID.FramesProcessed,
				100.0 * ID.FramesDetected / ID.FramesProcessed,
				(unsigned long long)ID.FramesSkippedThrottle,
				(unsigned long long)ID.FramesBaselineFiltered,
				avgMs );
			ID.FramesReceived = 0;
			ID.FramesProcessed = 0;
			ID.FramesDetected = 0;
			ID.FramesSkippedThrottle = 0;
			ID.FramesBaselineFiltered = 0;
			ID.TotalInferenceMs = 0.0;
			ID.LastStatsTime = now;
		}
	}

	// Get the decoded BGR frame from the pipeline
	cv::Mat& frame = TaskData->Frame.GetOrDecodeFrame();
	if( frame.empty() )
		return false;

	int origWidth = frame.cols;
	int origHeight = frame.rows;

	// Letterbox resize: maintain aspect ratio with padding
	float scaleX = (float)ID.InputWidth / origWidth;
	float scaleY = (float)ID.InputHeight / origHeight;
	float scale = std::min( scaleX, scaleY );

	int newWidth = (int)( origWidth * scale );
	int newHeight = (int)( origHeight * scale );
	int padX = ( ID.InputWidth - newWidth ) / 2;
	int padY = ( ID.InputHeight - newHeight ) / 2;

	cv::resize( frame, ID.ResizedFrame, cv::Size( newWidth, newHeight ), 0, 0, cv::INTER_LINEAR );

	// Create padded image (gray padding)
	cv::Mat padded( ID.InputHeight, ID.InputWidth, CV_8UC3, cv::Scalar( 114, 114, 114 ) );
	ID.ResizedFrame.copyTo( padded( cv::Rect( padX, padY, newWidth, newHeight ) ) );

	// Convert BGR to RGB and normalize to [0, 1]
	cv::cvtColor( padded, padded, cv::COLOR_BGR2RGB );
	padded.convertTo( ID.FloatFrame, CV_32F, 1.0 / 255.0 );

	// HWC to CHW conversion
	int channelSize = ID.InputWidth * ID.InputHeight;
	float* tensorData = ID.InputTensorData.data();
	const float* frameData = (const float*)ID.FloatFrame.data;

	for( int y = 0; y < ID.InputHeight; y++ )
	{
		for( int x = 0; x < ID.InputWidth; x++ )
		{
			int srcIdx = ( y * ID.InputWidth + x ) * 3;
			tensorData[0 * channelSize + y * ID.InputWidth + x] = frameData[srcIdx + 0]; // R
			tensorData[1 * channelSize + y * ID.InputWidth + x] = frameData[srcIdx + 1]; // G
			tensorData[2 * channelSize + y * ID.InputWidth + x] = frameData[srcIdx + 2]; // B
		}
	}

	// Create input tensor
	std::array<int64_t, 4> inputShape = { 1, 3, ID.InputHeight, ID.InputWidth };
	auto memoryInfo = Ort::MemoryInfo::CreateCpu( OrtArenaAllocator, OrtMemTypeDefault );
	auto inputTensor = Ort::Value::CreateTensor<float>(
		memoryInfo,
		ID.InputTensorData.data(),
		ID.InputTensorData.size(),
		inputShape.data(),
		inputShape.size()
	);

	// Run inference
	auto inferenceStart = std::chrono::steady_clock::now();
	std::vector<Ort::Value> outputTensors;
	try
	{
		outputTensors = ID.Session->Run(
			Ort::RunOptions{ nullptr },
			ID.InputNamePtrs.data(),
			&inputTensor,
			1,
			ID.OutputNamePtrs.data(),
			ID.OutputNamePtrs.size()
		);
	}
	catch( const Ort::Exception& e )
	{
		LOG_ERROR( "ONNX Detection: Inference failed: %s", e.what() );
		return false;
	}

	auto inferenceEnd = std::chrono::steady_clock::now();
	ID.LastInferenceTime = inferenceEnd;
	ID.FramesProcessed++;
	ID.TotalInferenceMs += std::chrono::duration<double, std::milli>( inferenceEnd - inferenceStart ).count();

	if( outputTensors.empty() )
		return false;

	// Parse output tensor
	auto& outputTensor = outputTensors[0];
	auto outputInfo = outputTensor.GetTensorTypeAndShapeInfo();
	auto outputShape = outputInfo.GetShape();
	const float* outputData = outputTensor.GetTensorData<float>();

	bool detected = false;
	size_t roiStartIndex = TaskData->Result.ROI.size();
	size_t tagsStartIndex = TaskData->Result.Tags.size();
	unsigned int supersetBefore = TaskData->Result.ClassificationSuperset;

	if( outputShape.size() != 3 || outputShape[0] != 1 )
	{
		char shapeBuf[256];
		int pos = 0;
		for( size_t i = 0; i < outputShape.size(); i++ )
			pos += snprintf( shapeBuf + pos, sizeof(shapeBuf) - pos, "%s%lld", i > 0 ? ", " : "", (long long)outputShape[i] );
		LOG_WARNING( "ONNX Detection: Unexpected output shape [%s]", shapeBuf );
		return false;
	}

	// Ultralytics YOLO26 ONNX has three possible output formats:
	// 1. End-to-end NMS-free: (1, N, 6) = [x1, y1, x2, y2, score, class_id] — default export
	// 2. Raw transposed: (1, 4+C, N) e.g. (1, 84, 8400) — older/custom export
	// 3. Raw standard: (1, N, 4+C) e.g. (1, 8400, 84)
	int64_t dim1 = outputShape[1];
	int64_t dim2 = outputShape[2];

	if( dim2 == 6 )
	{
		// End-to-end NMS-free format: (1, N, 6) = [x1, y1, x2, y2, score, class_id]
		int numDetections = (int)dim1;

		for( int i = 0; i < numDetections; i++ )
		{
			const float* row = outputData + i * 6;
			float score = row[4];
			if( score < ID.ConfidenceThreshold )
				continue;

			int classId = (int)row[5];
			if( classId < 0 || classId >= NumCOCOClasses )
				continue;

			// Coordinates are already corner format [x1,y1,x2,y2] in letterbox space
			float x1 = ( row[0] - padX ) / scale;
			float y1 = ( row[1] - padY ) / scale;
			float x2 = ( row[2] - padX ) / scale;
			float y2 = ( row[3] - padY ) / scale;

			x1 = std::max( 0.0f, std::min( x1, (float)origWidth ) );
			y1 = std::max( 0.0f, std::min( y1, (float)origHeight ) );
			x2 = std::max( 0.0f, std::min( x2, (float)origWidth ) );
			y2 = std::max( 0.0f, std::min( y2, (float)origHeight ) );

			ClassificationResult::RegionOfInterest ROI;
			ROI.Left = (unsigned int)x1;
			ROI.Top = (unsigned int)y1;
			ROI.Width = (unsigned int)( x2 - x1 );
			ROI.Height = (unsigned int)( y2 - y1 );
			ROI.Classification = COCOClassToFlag( classId );
			ROI.ClassificationConfidence = score;
			ROI.Tags.push_back( COCOClassNames[classId] );
			ROI.Filter = this;

			TaskData->Result.ROI.push_back( ROI );
			TaskData->Result.ClassificationSuperset |= ROI.Classification;
			TaskData->Result.Tags.push_back( COCOClassNames[classId] );

			detected = true;
		}
	}
	else
	{
		// Raw format with per-class scores. Detect transposed vs standard layout.
		int numDetections;
		int numChannels;
		bool transposed;

		if( dim1 < dim2 )
		{
			// Transposed: (1, channels, boxes) e.g. (1, 84, 8400)
			numChannels = (int)dim1;
			numDetections = (int)dim2;
			transposed = true;
		}
		else
		{
			// Standard: (1, boxes, channels) e.g. (1, 8400, 84)
			numDetections = (int)dim1;
			numChannels = (int)dim2;
			transposed = false;
		}

		int numClasses = numChannels - 4;
		if( numClasses <= 0 )
		{
				LOG_WARNING( "ONNX Detection: Invalid channel count %d (need at least 5)", numChannels );
			return false;
		}

		for( int i = 0; i < numDetections; i++ )
		{
			float cx, cy, bw, bh;
			int bestClass = 0;
			float bestScore;

			if( transposed )
			{
				// (1, C, N): channel c, box i = data[c * N + i]
				cx = outputData[0 * numDetections + i];
				cy = outputData[1 * numDetections + i];
				bw = outputData[2 * numDetections + i];
				bh = outputData[3 * numDetections + i];

				bestScore = outputData[4 * numDetections + i];
				for( int c = 1; c < numClasses && c < NumCOCOClasses; c++ )
				{
					float s = outputData[( 4 + c ) * numDetections + i];
					if( s > bestScore )
					{
						bestScore = s;
						bestClass = c;
					}
				}
			}
			else
			{
				// (1, N, C): box i, channel c = data[i * C + c]
				const float* row = outputData + i * numChannels;
				cx = row[0];
				cy = row[1];
				bw = row[2];
				bh = row[3];

				bestScore = row[4];
				for( int c = 1; c < numClasses && c < NumCOCOClasses; c++ )
				{
					if( row[4 + c] > bestScore )
					{
						bestScore = row[4 + c];
						bestClass = c;
					}
				}
			}

			if( bestScore < ID.ConfidenceThreshold )
				continue;

			// Convert from center format to corner format, then to original coordinates
			float x1 = ( cx - bw / 2.0f - padX ) / scale;
			float y1 = ( cy - bh / 2.0f - padY ) / scale;
			float x2 = ( cx + bw / 2.0f - padX ) / scale;
			float y2 = ( cy + bh / 2.0f - padY ) / scale;

			x1 = std::max( 0.0f, std::min( x1, (float)origWidth ) );
			y1 = std::max( 0.0f, std::min( y1, (float)origHeight ) );
			x2 = std::max( 0.0f, std::min( x2, (float)origWidth ) );
			y2 = std::max( 0.0f, std::min( y2, (float)origHeight ) );

			ClassificationResult::RegionOfInterest ROI;
			ROI.Left = (unsigned int)x1;
			ROI.Top = (unsigned int)y1;
			ROI.Width = (unsigned int)( x2 - x1 );
			ROI.Height = (unsigned int)( y2 - y1 );
			ROI.Classification = COCOClassToFlag( bestClass );
			ROI.ClassificationConfidence = bestScore;
			if( bestClass < NumCOCOClasses )
				ROI.Tags.push_back( COCOClassNames[bestClass] );
			ROI.Filter = this;

			TaskData->Result.ROI.push_back( ROI );
			TaskData->Result.ClassificationSuperset |= ROI.Classification;
			if( bestClass < NumCOCOClasses )
				TaskData->Result.Tags.push_back( COCOClassNames[bestClass] );

			detected = true;
		}
	}

	// Baseline mode: if no motion, store detections as baseline and return false
	if( !hasMotion )
	{
		ID.Baseline.clear();
		for( size_t i = roiStartIndex; i < TaskData->Result.ROI.size(); i++ )
		{
			auto& r = TaskData->Result.ROI[i];
			ONNXDetectionFilterData::BaselineObject obj;
			obj.ClassId = -1;
			// Find COCO class ID from tag name
			if( !r.Tags.empty() )
			{
				for( int c = 0; c < NumCOCOClasses; c++ )
				{
					if( r.Tags[0] == COCOClassNames[c] )
					{
						obj.ClassId = c;
						break;
					}
				}
			}
			// Store normalized coordinates
			obj.X1 = (float)r.Left / origWidth;
			obj.Y1 = (float)r.Top / origHeight;
			obj.X2 = (float)( r.Left + r.Width ) / origWidth;
			obj.Y2 = (float)( r.Top + r.Height ) / origHeight;
			ID.Baseline.push_back( obj );
		}

		// Remove all traces from TaskData (ROIs, tags, superset flags)
		TaskData->Result.ROI.resize( roiStartIndex );
		TaskData->Result.Tags.resize( tagsStartIndex );
		TaskData->Result.ClassificationSuperset = supersetBefore;

		ID.LastBaselineTime = now;
		if( !ID.BaselineInitialized )
		{
			ID.BaselineInitialized = true;
			LOG_INFO( "ONNX Camera %d: baseline captured (%zu objects)",
				TaskData->Frame.SourceID, ID.Baseline.size() );
		}

		return false;
	}

	// Motion mode: filter out detections that match baseline objects
	if( ID.BaselineInitialized && !ID.Baseline.empty() )
	{
		// Walk backwards so we can erase without invalidating indices
		for( int i = (int)TaskData->Result.ROI.size() - 1; i >= (int)roiStartIndex; i-- )
		{
			auto& r = TaskData->Result.ROI[i];
			float rx1 = (float)r.Left / origWidth;
			float ry1 = (float)r.Top / origHeight;
			float rx2 = (float)( r.Left + r.Width ) / origWidth;
			float ry2 = (float)( r.Top + r.Height ) / origHeight;

			// Find matching tag name for class comparison
			int detClassId = -1;
			if( !r.Tags.empty() )
			{
				for( int c = 0; c < NumCOCOClasses; c++ )
				{
					if( r.Tags[0] == COCOClassNames[c] )
					{
						detClassId = c;
						break;
					}
				}
			}

			for( auto& b : ID.Baseline )
			{
				if( b.ClassId == detClassId && ComputeIoU( rx1, ry1, rx2, ry2, b.X1, b.Y1, b.X2, b.Y2 ) > 0.5f )
				{
					// Remove corresponding tag
					if( !r.Tags.empty() )
					{
						auto& tags = TaskData->Result.Tags;
						for( auto it = tags.begin(); it != tags.end(); ++it )
						{
							if( *it == r.Tags[0] )
							{
								tags.erase( it );
								break;
							}
						}
					}
					TaskData->Result.ROI.erase( TaskData->Result.ROI.begin() + i );
					ID.FramesBaselineFiltered++;
					detected = false;  // Re-check below
					break;
				}
			}
		}

		// Re-check if any of our detections remain
		detected = ( TaskData->Result.ROI.size() > roiStartIndex );
	}

	if( detected )
	{
		ID.FramesDetected++;

		// Log only the detections added by this filter (skip upstream ROIs)
		char detBuf[512];
		int pos = 0;
		bool first = true;
		for( size_t i = roiStartIndex; i < TaskData->Result.ROI.size(); i++ )
		{
			auto& r = TaskData->Result.ROI[i];
			if( !first ) pos += snprintf( detBuf + pos, sizeof(detBuf) - pos, ", " );
			pos += snprintf( detBuf + pos, sizeof(detBuf) - pos, "%s(%.0f%%)",
				r.Tags.empty() ? "unknown" : r.Tags[0].c_str(),
				r.ClassificationConfidence * 100.0f );
			first = false;
		}
		LOG_DEBUG( "ONNX Camera %d: %s", TaskData->Frame.SourceID, detBuf );
	}

	return detected;
}

}}
