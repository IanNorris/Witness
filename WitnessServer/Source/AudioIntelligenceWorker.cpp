#include "AudioIntelligenceWorker.h"
#include "SQLite.h"
#include "GlobalContext.h"

#include <Log.h>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <sstream>
#include <thread>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

namespace fs = std::filesystem;
namespace {
constexpr int AudioDetectionVersion = 1;
constexpr int OutputSampleRate = 16000;
}

bool DecodeAudioForIntelligence( const std::string& Path, std::vector<float>& Samples )
{
	AVFormatContext* Format = nullptr;
	AVCodecContext* Codec = nullptr;
	SwrContext* Resampler = nullptr;
	AVPacket* Packet = nullptr;
	AVFrame* Frame = nullptr;
	AVChannelLayout Mono = AV_CHANNEL_LAYOUT_MONO;
	auto Cleanup = [&]()
	{
		av_frame_free( &Frame );
		av_packet_free( &Packet );
		swr_free( &Resampler );
		avcodec_free_context( &Codec );
		avformat_close_input( &Format );
	};

	if( avformat_open_input( &Format, Path.c_str(), nullptr, nullptr ) < 0 ||
		avformat_find_stream_info( Format, nullptr ) < 0 )
	{
		Cleanup();
		return false;
	}
	const int StreamIndex = av_find_best_stream( Format, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0 );
	if( StreamIndex < 0 )
	{
		Cleanup();
		return false;
	}
	const AVCodec* Decoder = avcodec_find_decoder( Format->streams[StreamIndex]->codecpar->codec_id );
	if( !Decoder )
	{
		Cleanup();
		return false;
	}
	Codec = avcodec_alloc_context3( Decoder );
	if( !Codec || avcodec_parameters_to_context( Codec, Format->streams[StreamIndex]->codecpar ) < 0 ||
		avcodec_open2( Codec, Decoder, nullptr ) < 0 )
	{
		Cleanup();
		return false;
	}
	if( swr_alloc_set_opts2( &Resampler, &Mono, AV_SAMPLE_FMT_FLT, OutputSampleRate,
		&Codec->ch_layout, Codec->sample_fmt, Codec->sample_rate, 0, nullptr ) < 0 ||
		swr_init( Resampler ) < 0 )
	{
		Cleanup();
		return false;
	}

	Packet = av_packet_alloc();
	Frame = av_frame_alloc();
	if( !Packet || !Frame )
	{
		Cleanup();
		return false;
	}

	auto ReceiveFrames = [&]()
	{
		while( avcodec_receive_frame( Codec, Frame ) == 0 )
		{
			const int Capacity = static_cast<int>( av_rescale_rnd(
				swr_get_delay( Resampler, Codec->sample_rate ) + Frame->nb_samples,
				OutputSampleRate, Codec->sample_rate, AV_ROUND_UP ) );
			const size_t Offset = Samples.size();
			Samples.resize( Offset + std::max( Capacity, 0 ) );
			uint8_t* Output[] = { reinterpret_cast<uint8_t*>( Samples.data() + Offset ) };
			const int Converted = swr_convert( Resampler, Output, Capacity,
				const_cast<const uint8_t**>( Frame->extended_data ), Frame->nb_samples );
			Samples.resize( Offset + std::max( Converted, 0 ) );
			av_frame_unref( Frame );
		}
	};

	while( av_read_frame( Format, Packet ) >= 0 )
	{
		if( Packet->stream_index == StreamIndex && avcodec_send_packet( Codec, Packet ) >= 0 )
			ReceiveFrames();
		av_packet_unref( Packet );
	}
	avcodec_send_packet( Codec, nullptr );
	ReceiveFrames();
	for( ;; )
	{
		const int Capacity = static_cast<int>( av_rescale_rnd(
			swr_get_delay( Resampler, Codec->sample_rate ), OutputSampleRate,
			Codec->sample_rate, AV_ROUND_UP ) );
		if( Capacity <= 0 ) break;
		const size_t Offset = Samples.size();
		Samples.resize( Offset + Capacity );
		uint8_t* Output[] = { reinterpret_cast<uint8_t*>( Samples.data() + Offset ) };
		const int Converted = swr_convert( Resampler, Output, Capacity, nullptr, 0 );
		Samples.resize( Offset + std::max( Converted, 0 ) );
		if( Converted <= 0 ) break;
	}
	const bool Success = !Samples.empty();
	Cleanup();
	return Success;
}
AudioIntelligenceWorker::AudioIntelligenceWorker( const std::shared_ptr<MessageBus>& MessageBus,
	std::shared_ptr<SQLiteDatabase> DatabaseIn,
	std::shared_ptr<Witness::Camera::AudioClassifier> ClassifierIn,
	std::string CachePathIn,
	std::shared_ptr<GlobalContext> ContextIn,
	std::function<bool()> IsIdleIn )
	: WorkerBase( MessageBus ), Database( std::move( DatabaseIn ) ),
	  Classifier( std::move( ClassifierIn ) ), CachePath( std::move( CachePathIn ) ), Context( std::move( ContextIn ) ),
	  IsIdle( std::move( IsIdleIn ) )
{
}

void AudioIntelligenceWorker::WorkerMain()
{
	UpdateLastTimedAction( "Waiting for idle audio work" );
	if( IsIdle && !IsIdle() )
	{
		std::this_thread::sleep_for( std::chrono::seconds( 2 ) );
		return;
	}

	int64_t ClipUID = 0;
	int64_t Timestamp = 0;
	int Camera = 0;
	int RecordMode = 0;
	float Threshold = 0.5f;
	{
		SQLiteDatabaseQueryInstance Query( Database, "SelectClipForAudioIntelligence" );
		Query->Bind( "@AudioDetectionVersion", AudioDetectionVersion );
		Query->Execute( [&]( const SQLiteDatabaseQuery& Row )
		{
			ClipUID = Row.GetColumnValueInt64( 0 );
			Timestamp = Row.GetColumnValueInt64( 1 );
			Camera = Row.GetColumnValueInt( 2 );
			RecordMode = Row.GetColumnValueInt( 3 );
			Threshold = static_cast<float>( Row.GetColumnValueDouble( 4 ) );
			return false;
		} );
	}
	if( ClipUID == 0 )
	{
		UpdateLastTimedAction( "Audio queue idle" );
		// Keep shutdown latency bounded; the query is indexed and intentionally
		// cheap when the queue is empty.
		std::this_thread::sleep_for( std::chrono::seconds( 2 ) );
		return;
	}

	UpdateLastTimedAction( "Classifying clip audio" );
	ProcessClip( ClipUID, Timestamp, Camera, RecordMode, Threshold );
	std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );
}

void AudioIntelligenceWorker::MarkProcessed( int64_t ClipUID )
{
	SQLiteDatabaseQueryInstance Query( Database, "MarkClipAudioProcessed" );
	Query->Bind( "@ClipUID", ClipUID );
	Query->Bind( "@AudioDetectionVersion", AudioDetectionVersion );
	Query->Execute( nullptr );
}

void AudioIntelligenceWorker::ProcessClip( int64_t ClipUID, int64_t Timestamp, int Camera,
	int RecordMode, float Threshold )
{
	std::stringstream Name;
	Name << Camera << "_" << ( RecordMode == 1 ? "Auto" : "Manual" ) << "_" << Timestamp << ".mp4";
	const std::string Path = ( fs::path( CachePath ) / Name.str() ).string();
	if( !fs::exists( Path ) )
	{
		MarkProcessed( ClipUID );
		return;
	}

	std::vector<float> Samples;
	if( !DecodeAudioForIntelligence( Path, Samples ) )
	{
		Context->AudioIntelligence.DecodeFailures.fetch_add( 1 );
		LOG_DEBUG( "Audio intelligence: clip %lld has no decodable audio", static_cast<long long>( ClipUID ) );
		MarkProcessed( ClipUID );
		return;
	}

	try
	{
		const auto InferenceStart = std::chrono::steady_clock::now();
		const auto Events = Classifier->Classify( Samples, std::clamp( Threshold, 0.05f, 0.99f ) );
		const uint64_t InferenceUS = static_cast<uint64_t>( std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now() - InferenceStart ).count() );
		{
			SQLiteDatabaseQueryInstance Delete( Database, "DeleteAudioEventsForClip" );
			Delete->Bind( "@ClipUID", ClipUID );
			Delete->Execute( nullptr );
		}
		for( const auto& Event : Events )
		{
			SQLiteDatabaseQueryInstance Insert( Database, "InsertAudioEvent" );
			Insert->Bind( "@ClipUID", ClipUID );
			Insert->Bind( "@CameraID", Camera );
			Insert->Bind( "@GroupName", Event.Group.c_str() );
			Insert->Bind( "@StartTime", static_cast<double>( Timestamp ) + Event.StartSeconds );
			Insert->Bind( "@EndTime", static_cast<double>( Timestamp ) + Event.EndSeconds );
			Insert->Bind( "@PeakScore", static_cast<double>( Event.PeakScore ) );
			Insert->Bind( "@ModelVersion", "yamnet-v1" );
			Insert->Execute( nullptr );
		}
		MarkProcessed( ClipUID );
		Context->AudioIntelligence.ClipsProcessed.fetch_add( 1 );
		Context->AudioIntelligence.EventsProduced.fetch_add( Events.size() );
		Context->AudioIntelligence.LastClipUID.store( static_cast<uint64_t>( ClipUID ) );
		Context->AudioIntelligence.LastInferenceUS.store( InferenceUS );
		Context->AudioIntelligence.TotalInferenceUS.fetch_add( InferenceUS );
		crow::json::wvalue EventData;
		EventData["clipUID"] = ClipUID;
		EventData["cameraID"] = Camera;
		EventData["eventCount"] = Events.size();
		Context->Events->Broadcast( "audio:classified", std::move( EventData ) );
		LOG_DEBUG( "Audio intelligence: clip %lld produced %zu grouped events", static_cast<long long>( ClipUID ), Events.size() );
	}
	catch( const std::exception& Error )
	{
		Context->AudioIntelligence.InferenceFailures.fetch_add( 1 );
		LOG_ERROR( "Audio intelligence: inference failed for clip %lld: %s",
			static_cast<long long>( ClipUID ), Error.what() );
		MarkProcessed( ClipUID );
	}
}
