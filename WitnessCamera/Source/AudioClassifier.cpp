#include "AudioClassifier.h"

#include <onnxruntime_cxx_api.h>
#include <Log.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#endif

namespace Witness {
namespace Camera {

namespace {
constexpr int SampleRate = 16000;
constexpr int WindowSamples = 15360;
constexpr double WindowSeconds = 0.96;
constexpr double HopSeconds = 0.48;

std::string Lower( std::string Value )
{
	std::transform( Value.begin(), Value.end(), Value.begin(),
		[]( unsigned char Character ) { return static_cast<char>( std::tolower( Character ) ); } );
	return Value;
}

std::string CsvLastField( const std::string& Line )
{
	bool Quoted = false;
	size_t FieldStart = 0;
	for( size_t Index = 0; Index < Line.size(); ++Index )
	{
		if( Line[Index] == '"' ) Quoted = !Quoted;
		else if( Line[Index] == ',' && !Quoted ) FieldStart = Index + 1;
	}
	std::string Result = Line.substr( FieldStart );
	if( Result.size() >= 2 && Result.front() == '"' && Result.back() == '"' )
		Result = Result.substr( 1, Result.size() - 2 );
	return Result;
}

bool ContainsTerm( const std::string& Text, const std::string& Term )
{
	size_t Position = Text.find( Term );
	while( Position != std::string::npos )
	{
		const size_t End = Position + Term.size();
		const bool LeftBoundary = Position == 0 || !std::isalnum( static_cast<unsigned char>( Text[Position - 1] ) );
		const bool RightBoundary = End == Text.size() || !std::isalnum( static_cast<unsigned char>( Text[End] ) );
		if( LeftBoundary && RightBoundary ) return true;
		Position = Text.find( Term, Position + 1 );
	}
	return false;
}
}

struct AudioClassifierData
{
	std::unique_ptr<Ort::Env> Env;
	std::unique_ptr<Ort::Session> Session;
	Ort::SessionOptions Options;
	std::string InputName;
	std::string OutputName;
	std::unordered_map<std::string, std::vector<size_t>> GroupIndices;
	bool Loaded = false;
};

AudioClassifier::AudioClassifier() : m_Data( new AudioClassifierData() ) {}
AudioClassifier::~AudioClassifier() { delete m_Data; }

bool AudioClassifier::LoadModel( const char* ModelPath, const char* ClassMapPath )
{
	try
	{
		std::ifstream ClassFile( ClassMapPath );
		if( !ClassFile )
			throw std::runtime_error( "class map could not be opened" );

		const std::unordered_map<std::string, std::vector<std::string>> Patterns = {
			{ "speech", { "speech", "conversation", "narration", "whisper", "human voice" } },
			{ "dog", { "dog", "bark", "howl", "growling" } },
			{ "footsteps", { "footstep", "walk", "walking", "shuffle" } },
			{ "vehicle", { "vehicle", "car", "engine", "truck", "motorcycle" } },
			{ "alarm", { "alarm", "siren", "smoke detector" } },
			{ "glass", { "glass", "shatter" } },
			{ "door", { "door", "knock" } },
			{ "wind", { "wind noise (microphone)" } },
			{ "animal", { "cat", "meow", "bird", "fowl", "turkey", "gobble" } },
		};

		std::string Line;
		std::getline( ClassFile, Line );
		size_t ClassIndex = 0;
		while( std::getline( ClassFile, Line ) )
		{
			const std::string Name = Lower( CsvLastField( Line ) );
			for( const auto& [Group, Terms] : Patterns )
			{
				for( const auto& Term : Terms )
				{
					if( ContainsTerm( Name, Term ) )
					{
						m_Data->GroupIndices[Group].push_back( ClassIndex );
						break;
					}
				}
				if( Group == "wind" && Name == "wind" )
					m_Data->GroupIndices[Group].push_back( ClassIndex );
			}
			++ClassIndex;
		}
		if( ClassIndex != 521 )
			throw std::runtime_error( "class map does not contain 521 classes" );

		m_Data->Env = std::make_unique<Ort::Env>( ORT_LOGGING_LEVEL_WARNING, "witness_audio" );
		m_Data->Options.SetIntraOpNumThreads( 1 );
		m_Data->Options.SetInterOpNumThreads( 1 );
		m_Data->Options.SetGraphOptimizationLevel( GraphOptimizationLevel::ORT_ENABLE_ALL );
#ifdef _WIN32
		int Length = MultiByteToWideChar( CP_UTF8, 0, ModelPath, -1, nullptr, 0 );
		std::wstring WidePath( Length, 0 );
		MultiByteToWideChar( CP_UTF8, 0, ModelPath, -1, WidePath.data(), Length );
		m_Data->Session = std::make_unique<Ort::Session>( *m_Data->Env, WidePath.c_str(), m_Data->Options );
#else
		m_Data->Session = std::make_unique<Ort::Session>( *m_Data->Env, ModelPath, m_Data->Options );
#endif
		Ort::AllocatorWithDefaultOptions Allocator;
		m_Data->InputName = m_Data->Session->GetInputNameAllocated( 0, Allocator ).get();
		m_Data->OutputName = m_Data->Session->GetOutputNameAllocated( 0, Allocator ).get();
		m_Data->Loaded = true;
		LOG_INFO( "Audio intelligence: YAMNet loaded with %zu curated trigger groups", m_Data->GroupIndices.size() );
	}
	catch( const std::exception& Error )
	{
		m_Data->Loaded = false;
		LOG_ERROR( "Audio intelligence: failed to load model: %s", Error.what() );
	}
	return m_Data->Loaded;
}

bool AudioClassifier::IsModelLoaded() const { return m_Data && m_Data->Loaded; }

std::vector<AudioEventResult> AudioClassifier::Classify( const std::vector<float>& Waveform,
	float Threshold ) const
{
	std::vector<AudioEventResult> Results;
	if( !IsModelLoaded() || Waveform.empty() ) return Results;

	std::vector<float> Input = Waveform;
	if( Input.size() < WindowSamples ) Input.resize( WindowSamples, 0.0f );
	const int64_t Shape[] = { static_cast<int64_t>( Input.size() ) };
	auto Memory = Ort::MemoryInfo::CreateCpu( OrtArenaAllocator, OrtMemTypeDefault );
	auto Tensor = Ort::Value::CreateTensor<float>( Memory, Input.data(), Input.size(), Shape, 1 );
	const char* InputNames[] = { m_Data->InputName.c_str() };
	const char* OutputNames[] = { m_Data->OutputName.c_str() };
	auto Outputs = m_Data->Session->Run( Ort::RunOptions{ nullptr }, InputNames, &Tensor, 1, OutputNames, 1 );
	if( Outputs.empty() || !Outputs[0].IsTensor() ) return Results;
	const auto OutputShape = Outputs[0].GetTensorTypeAndShapeInfo().GetShape();
	if( OutputShape.size() != 2 || OutputShape[1] != 521 ) return Results;
	const size_t Windows = static_cast<size_t>( OutputShape[0] );
	const float* Scores = Outputs[0].GetTensorData<float>();
	const double AudioDuration = static_cast<double>( Waveform.size() ) / SampleRate;

	for( const auto& [Group, Indices] : m_Data->GroupIndices )
	{
		AudioEventResult Current;
		bool Active = false;
		for( size_t Window = 0; Window < Windows; ++Window )
		{
			float Score = 0.0f;
			for( size_t Index : Indices ) Score = std::max( Score, Scores[Window * 521 + Index] );
			const double Start = Window * HopSeconds;
			// A lower release threshold prevents a single borderline window from
			// fragmenting one continuous sound into several activity markers.
			if( Score >= ( Active ? Threshold * 0.75f : Threshold ) )
			{
				if( !Active )
				{
					Current = { Group, Start, std::min( AudioDuration, Start + WindowSeconds ), Score };
					Active = true;
				}
				else
				{
					Current.EndSeconds = std::min( AudioDuration, Start + WindowSeconds );
					Current.PeakScore = std::max( Current.PeakScore, Score );
				}
			}
			else if( Active )
			{
				Results.push_back( Current );
				Active = false;
			}
		}
		if( Active ) Results.push_back( Current );
	}
	std::sort( Results.begin(), Results.end(), []( const auto& Left, const auto& Right )
	{
		return Left.StartSeconds < Right.StartSeconds;
	} );
	return Results;
}

}}
