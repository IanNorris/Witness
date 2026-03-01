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
#include <filesystem>
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

// Pre-load cuDNN DLL so ONNX Runtime can find it when initializing CUDA
static void AddCudnnSearchPaths( const char* CudnnPath )
{
#ifdef _WIN32
	static bool s_Loaded = false;
	if( s_Loaded ) return;
	s_Loaded = true;

	const wchar_t* dllName = L"cudnn64_9.dll";

	// Check if already loadable from default search paths
	HMODULE test = LoadLibraryW( dllName );
	if( test )
	{
		LOG_INFO( "ONNX Detection: cuDNN already available in system path." );
		return;
	}

	auto tryLoad = []( const std::filesystem::path& dir, const wchar_t* dll ) -> bool
	{
		auto full = dir / dll;
		if( !std::filesystem::exists( full ) )
			return false;

		HMODULE h = LoadLibraryW( full.c_str() );
		if( h )
		{
			LOG_INFO( "ONNX Detection: Pre-loaded cuDNN from: %ls", full.c_str() );
			return true;
		}
		return false;
	};

	// If user specified a cuDNN root path, search for the DLL
	if( CudnnPath && CudnnPath[0] != '\0' )
	{
		std::filesystem::path root( CudnnPath );
		if( !std::filesystem::exists( root ) )
		{
			LOG_WARNING( "ONNX Detection: Specified cuDNN path does not exist: %s", CudnnPath );
			return;
		}

		// Search recursively for cudnn64_9.dll
		for( auto& entry : std::filesystem::recursive_directory_iterator( root,
			std::filesystem::directory_options::skip_permission_denied ) )
		{
			if( entry.is_regular_file() && entry.path().filename() == dllName )
			{
				if( tryLoad( entry.path().parent_path(), dllName ) )
					return;
			}
		}
		LOG_WARNING( "ONNX Detection: cudnn64_9.dll not found under: %s", CudnnPath );
		return;
	}

	// Auto-scan common NVIDIA cuDNN install locations
	const std::wstring cudnnRoots[] = {
		L"C:\\Program Files\\NVIDIA\\CUDNN",
		L"C:\\Program Files\\NVIDIA GPU Computing Toolkit\\CUDNN",
	};

	for( const auto& root : cudnnRoots )
	{
		if( !std::filesystem::exists( root ) )
			continue;

		for( auto& entry : std::filesystem::recursive_directory_iterator( root,
			std::filesystem::directory_options::skip_permission_denied ) )
		{
			if( entry.is_regular_file() && entry.path().filename() == dllName )
			{
				if( tryLoad( entry.path().parent_path(), dllName ) )
					return;
			}
		}
	}

	LOG_WARNING( "ONNX Detection: cudnn64_9.dll not found. GPU acceleration may fail. "
		"Set cudnn_path in admin settings or install cuDNN 9.x." );
#endif
}

// Test CUDA by actually creating an ONNX session with CUDA provider.
// Called in the probe child process. Will crash (via __fastfail) if cuDNN is broken.
bool TestCudaAvailability( const char* ModelPath, const char* CudnnPath )
{
#ifdef _WIN32
	AddCudnnSearchPaths( CudnnPath );

	try
	{
		Ort::Env env( ORT_LOGGING_LEVEL_WARNING, "cuda_probe" );
		Ort::SessionOptions options;
		options.SetIntraOpNumThreads( 1 );
		options.SetGraphOptimizationLevel( GraphOptimizationLevel::ORT_ENABLE_ALL );

		OrtCUDAProviderOptions cudaOptions{};
		cudaOptions.device_id = 0;
		cudaOptions.cudnn_conv_algo_search = OrtCudnnConvAlgoSearchDefault;
		cudaOptions.arena_extend_strategy = 1;
		options.AppendExecutionProvider_CUDA( cudaOptions );

		if( ModelPath && ModelPath[0] != '\0' )
		{
			int len = MultiByteToWideChar( CP_UTF8, 0, ModelPath, -1, nullptr, 0 );
			std::wstring widePath( len, 0 );
			MultiByteToWideChar( CP_UTF8, 0, ModelPath, -1, &widePath[0], len );
			Ort::Session session( env, widePath.c_str(), options );
		}

		return true;
	}
	catch( const Ort::Exception& )
	{
		return false;
	}
	catch( ... )
	{
		return false;
	}
#else
	return false;
#endif
}

// Spawn a child process to test CUDA safely.
bool TestCudaViaProbe( const char* CudnnPath )
{
#ifdef _WIN32
	// Get our own exe path
	wchar_t exePath[MAX_PATH] = {};
	GetModuleFileNameW( nullptr, exePath, MAX_PATH );

	// Build command line: "exePath" /test-cuda ["cudnnPath"]
	std::wstring cmdLine = L"\"";
	cmdLine += exePath;
	cmdLine += L"\" /test-cuda";

	if( CudnnPath && CudnnPath[0] != '\0' )
	{
		int len = MultiByteToWideChar( CP_UTF8, 0, CudnnPath, -1, nullptr, 0 );
		std::wstring wideCudnn( len, 0 );
		MultiByteToWideChar( CP_UTF8, 0, CudnnPath, -1, &wideCudnn[0], len );
		cmdLine += L" \"";
		cmdLine += wideCudnn.c_str();
		cmdLine += L"\"";
	}

	STARTUPINFOW si = {};
	si.cb = sizeof(si);
	si.dwFlags = STARTF_USESHOWWINDOW;
	si.wShowWindow = SW_HIDE;
	PROCESS_INFORMATION pi = {};

	if( !CreateProcessW( nullptr, &cmdLine[0], nullptr, nullptr, FALSE,
		CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi ) )
	{
		LOG_WARNING( "ONNX Detection: Failed to launch CUDA probe process." );
		return false;
	}

	// Wait up to 30 seconds for the probe
	DWORD waitResult = WaitForSingleObject( pi.hProcess, 30000 );
	DWORD exitCode = 1;

	if( waitResult == WAIT_OBJECT_0 )
	{
		GetExitCodeProcess( pi.hProcess, &exitCode );
	}
	else
	{
		LOG_WARNING( "ONNX Detection: CUDA probe timed out." );
		TerminateProcess( pi.hProcess, 1 );
	}

	CloseHandle( pi.hThread );
	CloseHandle( pi.hProcess );

	return exitCode == 0;
}
#endif

ONNXDetectionFilter::ONNXDetectionFilter( const MotionChainNode& Chain, const char* ModelPath, float ConfidenceThreshold, bool UseGPU, float MaxFPS, const char* CudnnPath )
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

		static enum { Untested, Available, Unavailable } s_CudaStatus = Untested;
		static std::string s_CudaError;

		if( UseGPU )
		{
			if( s_CudaStatus == Untested )
			{
				AddCudnnSearchPaths( CudnnPath );

				// Probe CUDA in a child process first — cuDNN calls __fastfail on error
				// which kills the process and cannot be caught by SEH or signal handlers.
				LOG_INFO( "ONNX Detection: Testing CUDA availability (probe process)..." );
				if( !TestCudaViaProbe( CudnnPath ) )
				{
					LOG_WARNING( "ONNX Detection: CUDA probe failed. Falling back to CPU. "
						"Check CUDA Toolkit 12.x, cuDNN 9.x, and GPU driver are installed correctly." );
					s_CudaStatus = Unavailable;
				}
				else
				{
					try
					{
						OrtCUDAProviderOptions cudaOptions{};
						cudaOptions.device_id = 0;
						cudaOptions.cudnn_conv_algo_search = OrtCudnnConvAlgoSearchDefault;
						cudaOptions.arena_extend_strategy = 1;
						ID.SessionOptions.AppendExecutionProvider_CUDA( cudaOptions );
						LOG_INFO( "ONNX Detection: CUDA GPU acceleration enabled." );
						s_CudaStatus = Available;
					}
					catch( const Ort::Exception& e )
					{
						s_CudaError = e.what();
						LOG_WARNING( "ONNX Detection: CUDA initialization failed. Falling back to CPU." );
						LOG_WARNING( "ONNX Detection: CUDA error detail: %s", s_CudaError.c_str() );
						s_CudaStatus = Unavailable;
					}
				}
			}
			else if( s_CudaStatus == Available )
			{
				OrtCUDAProviderOptions cudaOptions{};
				cudaOptions.device_id = 0;
				cudaOptions.cudnn_conv_algo_search = OrtCudnnConvAlgoSearchDefault;
				cudaOptions.arena_extend_strategy = 1;
				ID.SessionOptions.AppendExecutionProvider_CUDA( cudaOptions );
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
	cv::Mat& rawFrame = TaskData->Frame.GetOrDecodeFrame();
	if( rawFrame.empty() )
		return false;

	// Apply CLAHE for night/IR frames to improve detection
	cv::Mat enhancedFrame;
	cv::Mat& frame = rawFrame;
	if( ClassifyLighting( rawFrame ) == LightingCondition::Night )
	{
		enhancedFrame = ApplyCLAHE( rawFrame );
		frame = enhancedFrame;
	}

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

std::vector<DetectionResult> ONNXDetectionFilter::DetectFrame( const cv::Mat& bgrFrame )
{
	std::vector<DetectionResult> results;

	if( !m_ModelLoaded || bgrFrame.empty() )
		return results;

	auto& ID = GetData();

	int origWidth = bgrFrame.cols;
	int origHeight = bgrFrame.rows;

	// Letterbox resize: maintain aspect ratio with padding
	float scaleX = (float)ID.InputWidth / origWidth;
	float scaleY = (float)ID.InputHeight / origHeight;
	float scale = std::min( scaleX, scaleY );

	int newWidth = (int)( origWidth * scale );
	int newHeight = (int)( origHeight * scale );
	int padX = ( ID.InputWidth - newWidth ) / 2;
	int padY = ( ID.InputHeight - newHeight ) / 2;

	cv::Mat resized;
	cv::resize( bgrFrame, resized, cv::Size( newWidth, newHeight ), 0, 0, cv::INTER_LINEAR );

	cv::Mat padded( ID.InputHeight, ID.InputWidth, CV_8UC3, cv::Scalar( 114, 114, 114 ) );
	resized.copyTo( padded( cv::Rect( padX, padY, newWidth, newHeight ) ) );

	cv::cvtColor( padded, padded, cv::COLOR_BGR2RGB );
	cv::Mat floatFrame;
	padded.convertTo( floatFrame, CV_32F, 1.0 / 255.0 );

	// HWC to CHW conversion
	int channelSize = ID.InputWidth * ID.InputHeight;
	std::vector<float> tensorData( 3 * channelSize );
	const float* frameData = (const float*)floatFrame.data;

	for( int y = 0; y < ID.InputHeight; y++ )
	{
		for( int x = 0; x < ID.InputWidth; x++ )
		{
			int srcIdx = ( y * ID.InputWidth + x ) * 3;
			tensorData[0 * channelSize + y * ID.InputWidth + x] = frameData[srcIdx + 0];
			tensorData[1 * channelSize + y * ID.InputWidth + x] = frameData[srcIdx + 1];
			tensorData[2 * channelSize + y * ID.InputWidth + x] = frameData[srcIdx + 2];
		}
	}

	std::array<int64_t, 4> inputShape = { 1, 3, ID.InputHeight, ID.InputWidth };
	auto memoryInfo = Ort::MemoryInfo::CreateCpu( OrtArenaAllocator, OrtMemTypeDefault );
	auto inputTensor = Ort::Value::CreateTensor<float>(
		memoryInfo,
		tensorData.data(),
		tensorData.size(),
		inputShape.data(),
		inputShape.size()
	);

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
		LOG_ERROR( "ONNX DetectFrame: Inference failed: %s", e.what() );
		return results;
	}

	if( outputTensors.empty() )
		return results;

	auto& outputTensor = outputTensors[0];
	auto outputInfo = outputTensor.GetTensorTypeAndShapeInfo();
	auto outputShape = outputInfo.GetShape();
	const float* outputData = outputTensor.GetTensorData<float>();

	if( outputShape.size() != 3 || outputShape[0] != 1 )
		return results;

	int64_t dim1 = outputShape[1];
	int64_t dim2 = outputShape[2];

	if( dim2 == 6 )
	{
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

			DetectionResult r;
			r.ClassId = classId;
			r.Confidence = score;
			r.ClassName = COCOClassNames[classId];
			results.push_back( r );
		}
	}
	else
	{
		int numDetections, numChannels;
		bool transposed;

		if( dim1 < dim2 )
		{
			numChannels = (int)dim1;
			numDetections = (int)dim2;
			transposed = true;
		}
		else
		{
			numDetections = (int)dim1;
			numChannels = (int)dim2;
			transposed = false;
		}

		int numClasses = numChannels - 4;
		if( numClasses <= 0 )
			return results;

		for( int i = 0; i < numDetections; i++ )
		{
			int bestClass = 0;
			float bestScore;

			if( transposed )
			{
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
				const float* row = outputData + i * numChannels;
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

			if( bestClass < NumCOCOClasses )
			{
				DetectionResult r;
				r.ClassId = bestClass;
				r.Confidence = bestScore;
				r.ClassName = COCOClassNames[bestClass];
				results.push_back( r );
			}
		}
	}

	return results;
}

LightingCondition ClassifyLighting( const cv::Mat& bgrFrame )
{
	if( bgrFrame.empty() )
		return LightingCondition::Unknown;

	// Convert to HSV to analyze brightness (V) and color saturation (S)
	cv::Mat hsv;
	cv::cvtColor( bgrFrame, hsv, cv::COLOR_BGR2HSV );

	cv::Scalar meanVal = cv::mean( hsv );
	double meanSaturation = meanVal[1];
	double meanBrightness = meanVal[2];

	// Low brightness = night, regardless of saturation
	// IR cameras produce low-saturation grayscale even at moderate brightness
	if( meanBrightness < 60.0 )
		return LightingCondition::Night;

	// Moderate brightness but very low saturation = IR illuminated (grayscale)
	if( meanSaturation < 25.0 )
		return LightingCondition::Night;

	return LightingCondition::Day;
}

cv::Mat ApplyCLAHE( const cv::Mat& bgrFrame, double clipLimit, int tileSize )
{
	if( bgrFrame.empty() )
		return bgrFrame;

	// Convert to LAB color space, apply CLAHE to L channel
	cv::Mat lab;
	cv::cvtColor( bgrFrame, lab, cv::COLOR_BGR2Lab );

	std::vector<cv::Mat> labChannels;
	cv::split( lab, labChannels );

	auto clahe = cv::createCLAHE( clipLimit, cv::Size( tileSize, tileSize ) );
	clahe->apply( labChannels[0], labChannels[0] );

	cv::merge( labChannels, lab );

	cv::Mat result;
	cv::cvtColor( lab, result, cv::COLOR_Lab2BGR );
	return result;
}

}}
