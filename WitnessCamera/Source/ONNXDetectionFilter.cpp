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
#include <cstdio>

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
};

PIMPL_CONSTRUCT(ONNXDetectionFilterData)

ONNXDetectionFilter::ONNXDetectionFilter( const MotionChainNode& Chain, const char* ModelPath, float ConfidenceThreshold, bool UseGPU )
: RecordFilterBase( Chain )
, m_ModelLoaded( false )
{
	auto& ID = GetData();
	ID.ConfidenceThreshold = ConfidenceThreshold;

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
				printf( "ONNX Detection: CUDA GPU acceleration enabled.\n" );
			}
			catch( const Ort::Exception& e )
			{
				printf( "ONNX Detection: CUDA unavailable (%s), using CPU.\n", e.what() );
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
		printf( "ONNX Detection: Model loaded (%s), input %dx%d.\n", ModelPath, ID.InputWidth, ID.InputHeight );
	}
	catch( const Ort::Exception& e )
	{
		printf( "ONNX Detection: Failed to load model '%s': %s\n", ModelPath, e.what() );
	}
	catch( const std::exception& e )
	{
		printf( "ONNX Detection: Failed to load model '%s': %s\n", ModelPath, e.what() );
	}
}

ONNXDetectionFilter::~ONNXDetectionFilter()
{}

bool ONNXDetectionFilter::IsModelLoaded() const
{
	return m_ModelLoaded;
}

bool ONNXDetectionFilter::ProcessFrame( SharedClassificationTask TaskData )
{
	if( !m_ModelLoaded )
		return false;

	auto& ID = GetData();

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
		printf( "ONNX Detection: Inference failed: %s\n", e.what() );
		return false;
	}

	if( outputTensors.empty() )
		return false;

	// Parse output: YOLO26 end-to-end outputs (1, N, 6) = [x1, y1, x2, y2, score, class_id]
	auto& outputTensor = outputTensors[0];
	auto outputInfo = outputTensor.GetTensorTypeAndShapeInfo();
	auto outputShape = outputInfo.GetShape();
	const float* outputData = outputTensor.GetTensorData<float>();

	bool detected = false;

	if( outputShape.size() == 3 && outputShape[2] == 6 )
	{
		// NMS-free end-to-end format: (1, N, 6)
		int numDetections = (int)outputShape[1];

		for( int i = 0; i < numDetections; i++ )
		{
			float x1 = outputData[i * 6 + 0];
			float y1 = outputData[i * 6 + 1];
			float x2 = outputData[i * 6 + 2];
			float y2 = outputData[i * 6 + 3];
			float score = outputData[i * 6 + 4];
			int classId = (int)outputData[i * 6 + 5];

			if( score < ID.ConfidenceThreshold )
				continue;

			if( classId < 0 || classId >= NumCOCOClasses )
				continue;

			// Convert from letterboxed coordinates back to original frame coordinates
			float roiX1 = ( x1 - padX ) / scale;
			float roiY1 = ( y1 - padY ) / scale;
			float roiX2 = ( x2 - padX ) / scale;
			float roiY2 = ( y2 - padY ) / scale;

			// Clamp to frame bounds
			roiX1 = std::max( 0.0f, std::min( roiX1, (float)origWidth ) );
			roiY1 = std::max( 0.0f, std::min( roiY1, (float)origHeight ) );
			roiX2 = std::max( 0.0f, std::min( roiX2, (float)origWidth ) );
			roiY2 = std::max( 0.0f, std::min( roiY2, (float)origHeight ) );

			ClassificationResult::RegionOfInterest ROI;
			ROI.Left = (unsigned int)roiX1;
			ROI.Top = (unsigned int)roiY1;
			ROI.Width = (unsigned int)( roiX2 - roiX1 );
			ROI.Height = (unsigned int)( roiY2 - roiY1 );
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
	else if( outputShape.size() == 3 && outputShape[2] > 6 )
	{
		// Classic YOLO format: (1, N, 4+C) where C = number of classes
		// Rows are [x_center, y_center, width, height, class0_score, class1_score, ...]
		int numDetections = (int)outputShape[1];
		int numClasses = (int)outputShape[2] - 4;

		for( int i = 0; i < numDetections; i++ )
		{
			const float* row = outputData + i * outputShape[2];
			float cx = row[0];
			float cy = row[1];
			float w = row[2];
			float h = row[3];

			// Find best class
			int bestClass = 0;
			float bestScore = row[4];
			for( int c = 1; c < numClasses && c < NumCOCOClasses; c++ )
			{
				if( row[4 + c] > bestScore )
				{
					bestScore = row[4 + c];
					bestClass = c;
				}
			}

			if( bestScore < ID.ConfidenceThreshold )
				continue;

			// Convert from center format to corner format, then to original coordinates
			float x1 = ( cx - w / 2.0f - padX ) / scale;
			float y1 = ( cy - h / 2.0f - padY ) / scale;
			float x2 = ( cx + w / 2.0f - padX ) / scale;
			float y2 = ( cy + h / 2.0f - padY ) / scale;

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
	else
	{
		printf( "ONNX Detection: Unexpected output shape [" );
		for( size_t i = 0; i < outputShape.size(); i++ )
			printf( "%s%lld", i > 0 ? ", " : "", (long long)outputShape[i] );
		printf( "]\n" );
	}

	return detected;
}

}}
