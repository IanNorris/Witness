#pragma once

#include "Export.h"

#include <string>
#include <vector>

namespace Witness {
namespace Camera {

struct AudioEventResult
{
	std::string Group;
	double StartSeconds = 0.0;
	double EndSeconds = 0.0;
	float PeakScore = 0.0f;
};

struct AudioClassifierData;

// CPU-only YAMNet wrapper. The caller owns media decoding and supplies mono
// 16 kHz float samples; no waveform is retained after Classify returns.
class CAMERA_API AudioClassifier
{
public:
	AudioClassifier();
	~AudioClassifier();

	bool LoadModel( const char* ModelPath, const char* ClassMapPath );
	bool IsModelLoaded() const;
	std::vector<AudioEventResult> Classify( const std::vector<float>& Waveform,
		float Threshold ) const;

private:
	AudioClassifierData* m_Data;
};

}}
