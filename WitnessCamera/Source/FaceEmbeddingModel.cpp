#include "FaceEmbeddingModel.h"

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
#include <cmath>
#include <numeric>
#include <Log.h>

namespace Witness{
namespace Camera{

// ArcFace standard 112x112 reference landmarks
// [0]=right eye, [1]=left eye, [2]=nose, [3]=right mouth, [4]=left mouth
static const float ARCFACE_REF_X[5] = { 38.2946f, 73.5318f, 56.0252f, 41.5493f, 70.7299f };
static const float ARCFACE_REF_Y[5] = { 51.6963f, 51.5014f, 71.7366f, 92.3655f, 92.2041f };

struct FaceEmbeddingModelData
{
	FaceEmbeddingModelData()
	: InputWidth( 112 )
	, InputHeight( 112 )
	, EmbeddingDimension( 0 )
	{}

	std::unique_ptr<Ort::Env> Env;
	std::unique_ptr<Ort::Session> Session;
	Ort::SessionOptions SessionOptions;

	std::vector<std::string> InputNames;
	std::vector<std::string> OutputNames;
	std::vector<const char*> InputNamePtrs;
	std::vector<const char*> OutputNamePtrs;

	int InputWidth;
	int InputHeight;
	int EmbeddingDimension;

	// Reusable buffers
	std::vector<float> InputTensorData;
};

FaceEmbeddingModel::FaceEmbeddingModel()
: m_Data( new FaceEmbeddingModelData() )
, m_ModelLoaded( false )
{
}

FaceEmbeddingModel::~FaceEmbeddingModel()
{
	delete m_Data;
}

bool FaceEmbeddingModel::LoadModel( const char* ModelPath, bool UseGPU, const char* CudnnPath )
{
	try
	{
		m_Data->Env = std::make_unique<Ort::Env>( ORT_LOGGING_LEVEL_WARNING, "witness_face_embedding" );

		m_Data->SessionOptions.SetIntraOpNumThreads( 1 );
		m_Data->SessionOptions.SetGraphOptimizationLevel( GraphOptimizationLevel::ORT_ENABLE_ALL );

		if( UseGPU )
		{
			try
			{
				OrtCUDAProviderOptions cudaOptions{};
				cudaOptions.device_id = 0;
				cudaOptions.cudnn_conv_algo_search = OrtCudnnConvAlgoSearchDefault;
				cudaOptions.arena_extend_strategy = 1;
				m_Data->SessionOptions.AppendExecutionProvider_CUDA( cudaOptions );
				LOG_INFO( "FaceEmbedding: CUDA GPU acceleration enabled." );
			}
			catch( const Ort::Exception& e )
			{
				LOG_WARNING( "FaceEmbedding: CUDA init failed, falling back to CPU: %s", e.what() );
			}
		}

		// Create session
#ifdef _WIN32
		int len = MultiByteToWideChar( CP_UTF8, 0, ModelPath, -1, nullptr, 0 );
		std::wstring widePath( len, 0 );
		MultiByteToWideChar( CP_UTF8, 0, ModelPath, -1, &widePath[0], len );
		m_Data->Session = std::make_unique<Ort::Session>( *m_Data->Env, widePath.c_str(), m_Data->SessionOptions );
#else
		m_Data->Session = std::make_unique<Ort::Session>( *m_Data->Env, ModelPath, m_Data->SessionOptions );
#endif

		// Discover input/output names
		Ort::AllocatorWithDefaultOptions allocator;

		size_t numInputs = m_Data->Session->GetInputCount();
		for( size_t i = 0; i < numInputs; i++ )
		{
			auto name = m_Data->Session->GetInputNameAllocated( i, allocator );
			m_Data->InputNames.push_back( name.get() );
		}

		size_t numOutputs = m_Data->Session->GetOutputCount();
		for( size_t i = 0; i < numOutputs; i++ )
		{
			auto name = m_Data->Session->GetOutputNameAllocated( i, allocator );
			m_Data->OutputNames.push_back( name.get() );
		}

		for( auto& name : m_Data->InputNames )
			m_Data->InputNamePtrs.push_back( name.c_str() );
		for( auto& name : m_Data->OutputNames )
			m_Data->OutputNamePtrs.push_back( name.c_str() );

		// Get input dimensions (expect [1, 3, H, W])
		auto inputInfo = m_Data->Session->GetInputTypeInfo( 0 );
		auto tensorInfo = inputInfo.GetTensorTypeAndShapeInfo();
		auto inputShape = tensorInfo.GetShape();
		if( inputShape.size() == 4 )
		{
			m_Data->InputHeight = (int)inputShape[2];
			m_Data->InputWidth = (int)inputShape[3];
		}

		// Get embedding dimension from output shape (expect [1, N])
		auto outInfo = m_Data->Session->GetOutputTypeInfo( 0 );
		auto outTensorInfo = outInfo.GetTensorTypeAndShapeInfo();
		auto outShape = outTensorInfo.GetShape();
		if( outShape.size() >= 2 )
		{
			m_Data->EmbeddingDimension = (int)outShape[1];
		}
		else if( outShape.size() == 1 )
		{
			m_Data->EmbeddingDimension = (int)outShape[0];
		}

		// Pre-allocate input tensor buffer
		m_Data->InputTensorData.resize( 3 * m_Data->InputWidth * m_Data->InputHeight );

		m_ModelLoaded = true;

		LOG_INFO( "FaceEmbedding: Model loaded (%s), input %dx%d, embedding dim %d.",
			ModelPath, m_Data->InputWidth, m_Data->InputHeight, m_Data->EmbeddingDimension );
	}
	catch( const Ort::Exception& e )
	{
		LOG_ERROR( "FaceEmbedding: Failed to load model '%s': %s", ModelPath, e.what() );
		m_ModelLoaded = false;
	}
	catch( const std::exception& e )
	{
		LOG_ERROR( "FaceEmbedding: Failed to load model '%s': %s", ModelPath, e.what() );
		m_ModelLoaded = false;
	}

	return m_ModelLoaded;
}

bool FaceEmbeddingModel::IsModelLoaded() const
{
	return m_ModelLoaded;
}

int FaceEmbeddingModel::GetEmbeddingDimension() const
{
	return m_Data ? m_Data->EmbeddingDimension : 0;
}

cv::Mat FaceEmbeddingModel::AlignFace( const cv::Mat& faceCrop, const float landmarkX[5], const float landmarkY[5] )
{
	// Source points: detected landmarks in the input crop
	cv::Point2f src[5];
	for( int i = 0; i < 5; i++ )
	{
		src[i] = cv::Point2f( landmarkX[i], landmarkY[i] );
	}

	// Destination points: ArcFace reference landmarks for 112x112
	cv::Point2f dst[5];
	for( int i = 0; i < 5; i++ )
	{
		dst[i] = cv::Point2f( ARCFACE_REF_X[i], ARCFACE_REF_Y[i] );
	}

	// Use 3 most stable points for affine transform: both eyes + nose
	cv::Point2f srcPts[3] = { src[0], src[1], src[2] };
	cv::Point2f dstPts[3] = { dst[0], dst[1], dst[2] };

	cv::Mat M = cv::getAffineTransform( srcPts, dstPts );
	if( M.empty() )
	{
		// Fallback: just resize to 112x112
		cv::Mat resized;
		cv::resize( faceCrop, resized, cv::Size( 112, 112 ) );
		return resized;
	}

	cv::Mat aligned;
	cv::warpAffine( faceCrop, aligned, M, cv::Size( 112, 112 ) );
	return aligned;
}

std::vector<float> FaceEmbeddingModel::GetEmbedding( const cv::Mat& face112 )
{
	if( !m_ModelLoaded || !m_Data->Session )
		return {};

	// Resize if needed
	cv::Mat resized;
	if( face112.cols != m_Data->InputWidth || face112.rows != m_Data->InputHeight )
		cv::resize( face112, resized, cv::Size( m_Data->InputWidth, m_Data->InputHeight ) );
	else
		resized = face112;

	// BGR to RGB, normalize to [0, 1]
	cv::Mat rgb;
	cv::cvtColor( resized, rgb, cv::COLOR_BGR2RGB );
	cv::Mat floatFrame;
	rgb.convertTo( floatFrame, CV_32F, 1.0 / 255.0 );

	// HWC to CHW
	int channelSize = m_Data->InputWidth * m_Data->InputHeight;
	float* tensorData = m_Data->InputTensorData.data();
	const float* frameData = (const float*)floatFrame.data;

	for( int y = 0; y < m_Data->InputHeight; y++ )
	{
		for( int x = 0; x < m_Data->InputWidth; x++ )
		{
			int srcIdx = ( y * m_Data->InputWidth + x ) * 3;
			tensorData[0 * channelSize + y * m_Data->InputWidth + x] = frameData[srcIdx + 0];
			tensorData[1 * channelSize + y * m_Data->InputWidth + x] = frameData[srcIdx + 1];
			tensorData[2 * channelSize + y * m_Data->InputWidth + x] = frameData[srcIdx + 2];
		}
	}

	// Create input tensor
	std::array<int64_t, 4> inputShape = { 1, 3, m_Data->InputHeight, m_Data->InputWidth };
	auto memoryInfo = Ort::MemoryInfo::CreateCpu( OrtArenaAllocator, OrtMemTypeDefault );
	auto inputTensor = Ort::Value::CreateTensor<float>(
		memoryInfo,
		m_Data->InputTensorData.data(),
		m_Data->InputTensorData.size(),
		inputShape.data(),
		inputShape.size()
	);

	// Run inference
	std::vector<Ort::Value> outputTensors;
	try
	{
		outputTensors = m_Data->Session->Run(
			Ort::RunOptions{ nullptr },
			m_Data->InputNamePtrs.data(),
			&inputTensor,
			1,
			m_Data->OutputNamePtrs.data(),
			m_Data->OutputNamePtrs.size()
		);
	}
	catch( const Ort::Exception& e )
	{
		LOG_ERROR( "FaceEmbedding: Inference failed: %s", e.what() );
		return {};
	}

	// Extract embedding
	float* outputData = outputTensors[0].GetTensorMutableData<float>();
	auto outputInfo = outputTensors[0].GetTensorTypeAndShapeInfo();
	auto outputShape = outputInfo.GetShape();

	int dim = m_Data->EmbeddingDimension;
	if( dim <= 0 )
	{
		// Try to determine from output
		dim = 1;
		for( auto s : outputShape )
			dim *= (int)s;
	}

	std::vector<float> embedding( outputData, outputData + dim );

	// L2 normalize
	float norm = 0.0f;
	for( float v : embedding )
		norm += v * v;
	norm = std::sqrt( norm );
	if( norm > 1e-6f )
	{
		for( float& v : embedding )
			v /= norm;
	}

	return embedding;
}

float FaceEmbeddingModel::CosineSimilarity( const std::vector<float>& a, const std::vector<float>& b )
{
	if( a.size() != b.size() || a.empty() )
		return 0.0f;

	// For L2-normalized vectors, cosine similarity = dot product
	float dot = 0.0f;
	for( size_t i = 0; i < a.size(); i++ )
		dot += a[i] * b[i];

	return dot;
}

}}
