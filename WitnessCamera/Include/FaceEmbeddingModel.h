#pragma once

#include "Export.h"

#include <string>
#include <vector>

namespace cv { class Mat; }

namespace Witness{
namespace Camera{

struct FaceEmbeddingModelData;

// Model-agnostic face embedding extractor using ONNX Runtime.
// Takes a 112x112 BGR face crop + optional 5-point landmarks for alignment.
// Returns an L2-normalized embedding vector (dimension determined by model).
class CAMERA_API FaceEmbeddingModel
{
public:

	FaceEmbeddingModel();
	~FaceEmbeddingModel();

	// Load an ONNX embedding model (ArcFace, MobileFaceNet, etc.)
	// Returns true if model loaded successfully.
	bool LoadModel( const char* ModelPath, bool UseGPU = false, const char* CudnnPath = nullptr );

	bool IsModelLoaded() const;

	// Embedding dimension (e.g., 512 for ArcFace, 128 for MobileFaceNet)
	int GetEmbeddingDimension() const;

	// Align a face crop using 5-point landmarks to ArcFace reference coordinates.
	// Input:  BGR face crop (any size), 5 landmark (x,y) pairs in crop-relative pixels
	// Output: 112x112 aligned BGR face
	static cv::Mat AlignFace( const cv::Mat& faceCrop, const float landmarkX[5], const float landmarkY[5] );

	// Extract embedding from a 112x112 BGR face image (aligned or unaligned).
	// Returns L2-normalized embedding vector, or empty vector on failure.
	std::vector<float> GetEmbedding( const cv::Mat& face112 );

	// Cosine similarity between two L2-normalized embeddings.
	static float CosineSimilarity( const std::vector<float>& a, const std::vector<float>& b );

private:

	FaceEmbeddingModelData* m_Data;
	bool m_ModelLoaded;
};

}}
