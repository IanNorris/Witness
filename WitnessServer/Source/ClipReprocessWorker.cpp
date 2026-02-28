#include "ClipReprocessWorker.h"

#include <Log.h>
#include <filesystem>
#include <set>
#include <sstream>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
}

#include <opencv2/core/core.hpp>
#include <opencv2/imgproc.hpp>

namespace fs = std::filesystem;

ClipReprocessWorker::ClipReprocessWorker(
	const std::shared_ptr<MessageBus>& MessageBus,
	std::shared_ptr<SQLiteDatabase> Database,
	std::shared_ptr<Witness::Camera::ONNXDetectionFilter> DetectionFilter,
	std::string CachePath,
	std::function<bool()> IsIdle
)
: WorkerBase( MessageBus )
, Database( std::move( Database ) )
, DetectionFilter( std::move( DetectionFilter ) )
, CachePath( std::move( CachePath ) )
, IsIdle( std::move( IsIdle ) )
{
}

void ClipReprocessWorker::WorkerMain()
{
	UpdateLastTimedAction( "Idle" );

	// Yield to live detection when cameras have active motion
	if( !IsIdle() )
	{
		std::this_thread::sleep_for( std::chrono::seconds( 5 ) );
		return;
	}

	// Query for next clip needing reprocessing
	int64_t clipUID = 0;
	int64_t timestamp = 0;
	int camera = 0;
	int recordMode = 0;
	std::string existingTags;

	{
		SQLiteDatabaseQueryInstance SelectClipForReprocess( Database, "SelectClipForReprocess" );
		SelectClipForReprocess->Bind( "@DetectionVersion", CURRENT_DETECTION_VERSION );
		SelectClipForReprocess->Execute( [&]( const SQLiteDatabaseQuery& query ) -> bool
		{
			clipUID = query.GetColumnValueInt64( 0 );
			timestamp = query.GetColumnValueInt64( 1 );
			camera = query.GetColumnValueInt( 2 );
			recordMode = query.GetColumnValueInt( 6 );
			const char* tags = query.GetColumnValueText( 10 );
			if( tags )
				existingTags = tags;
			return false; // only need first row
		});
	}

	if( clipUID == 0 )
	{
		std::this_thread::sleep_for( std::chrono::seconds( 30 ) );
		return;
	}

	UpdateLastTimedAction( "Reprocessing clip" );

	// Build clip filename: {Camera}_{Auto|Manual}_{Timestamp}.mp4
	std::stringstream nameStream;
	nameStream << camera << "_" << ( recordMode == 1 ? "Auto" : "Manual" ) << "_" << timestamp << ".mp4";
	std::string clipPath = ( fs::path( CachePath ) / nameStream.str() ).string();

	if( !fs::exists( clipPath ) )
	{
		// File missing — mark as processed to skip it
		SQLiteDatabaseQueryInstance UpdateClipDetection( Database, "UpdateClipDetection" );
		UpdateClipDetection->Bind( "@ClipUID", clipUID );
		UpdateClipDetection->Bind( "@Tags", existingTags.c_str() );
		UpdateClipDetection->Bind( "@DetectionVersion", CURRENT_DETECTION_VERSION );
		UpdateClipDetection->Execute( nullptr );
		LOG_WARNING( "ClipReprocess: Clip %lld file not found, skipping: %s", (long long)clipUID, clipPath.c_str() );
		return;
	}

	// Open the clip with FFmpeg
	AVFormatContext* fmtCtx = nullptr;
	if( avformat_open_input( &fmtCtx, clipPath.c_str(), nullptr, nullptr ) < 0 )
	{
		LOG_ERROR( "ClipReprocess: Failed to open clip %lld: %s", (long long)clipUID, clipPath.c_str() );
		return;
	}

	if( avformat_find_stream_info( fmtCtx, nullptr ) < 0 )
	{
		LOG_ERROR( "ClipReprocess: Failed to find stream info for clip %lld", (long long)clipUID );
		avformat_close_input( &fmtCtx );
		return;
	}

	// Find video stream
	int videoIdx = -1;
	for( unsigned i = 0; i < fmtCtx->nb_streams; i++ )
	{
		if( fmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO )
		{
			videoIdx = i;
			break;
		}
	}

	if( videoIdx < 0 )
	{
		LOG_ERROR( "ClipReprocess: No video stream in clip %lld", (long long)clipUID );
		avformat_close_input( &fmtCtx );
		return;
	}

	// Open decoder
	auto codecpar = fmtCtx->streams[videoIdx]->codecpar;
	auto codec = avcodec_find_decoder( codecpar->codec_id );
	if( !codec )
	{
		LOG_ERROR( "ClipReprocess: No decoder for clip %lld", (long long)clipUID );
		avformat_close_input( &fmtCtx );
		return;
	}

	auto codecCtx = avcodec_alloc_context3( codec );
	avcodec_parameters_to_context( codecCtx, codecpar );
	if( avcodec_open2( codecCtx, codec, nullptr ) < 0 )
	{
		LOG_ERROR( "ClipReprocess: Failed to open decoder for clip %lld", (long long)clipUID );
		avcodec_free_context( &codecCtx );
		avformat_close_input( &fmtCtx );
		return;
	}

	// Set up SwsContext for pixel format conversion to BGR24
	SwsContext* swsCtx = sws_getContext(
		codecCtx->width, codecCtx->height, codecCtx->pix_fmt,
		codecCtx->width, codecCtx->height, AV_PIX_FMT_BGR24,
		SWS_BILINEAR, nullptr, nullptr, nullptr
	);

	if( !swsCtx )
	{
		LOG_ERROR( "ClipReprocess: Failed to create SwsContext for clip %lld", (long long)clipUID );
		avcodec_free_context( &codecCtx );
		avformat_close_input( &fmtCtx );
		return;
	}

	// Determine clip duration for sampling every 2 seconds
	AVStream* videoStream = fmtCtx->streams[videoIdx];
	double durationSec = 0.0;
	if( fmtCtx->duration > 0 )
		durationSec = (double)fmtCtx->duration / AV_TIME_BASE;
	else if( videoStream->duration > 0 && videoStream->time_base.den > 0 )
		durationSec = (double)videoStream->duration * av_q2d( videoStream->time_base );

	std::set<std::string> detectedTags;
	AVPacket* pkt = av_packet_alloc();
	AVFrame* frame = av_frame_alloc();
	AVFrame* bgrFrame = av_frame_alloc();

	int frameWidth = codecCtx->width;
	int frameHeight = codecCtx->height;

	// Allocate BGR frame buffer
	int bgrBufSize = av_image_get_buffer_size( AV_PIX_FMT_BGR24, frameWidth, frameHeight, 1 );
	std::vector<uint8_t> bgrBuffer( bgrBufSize );
	av_image_fill_arrays( bgrFrame->data, bgrFrame->linesize, bgrBuffer.data(),
		AV_PIX_FMT_BGR24, frameWidth, frameHeight, 1 );

	// Sample frames at 2-second intervals
	double sampleInterval = 2.0;
	double currentTime = 0.0;

	while( currentTime <= durationSec || currentTime == 0.0 )
	{
		// Seek to target time
		int64_t seekTarget = (int64_t)( currentTime * AV_TIME_BASE );
		if( currentTime > 0.0 )
		{
			av_seek_frame( fmtCtx, -1, seekTarget, AVSEEK_FLAG_BACKWARD );
			avcodec_flush_buffers( codecCtx );
		}

		// Decode one frame
		bool gotFrame = false;
		while( av_read_frame( fmtCtx, pkt ) >= 0 )
		{
			if( pkt->stream_index == videoIdx )
			{
				avcodec_send_packet( codecCtx, pkt );
				if( avcodec_receive_frame( codecCtx, frame ) == 0 )
				{
					// Convert to BGR24 cv::Mat
					sws_scale( swsCtx, frame->data, frame->linesize, 0, frameHeight,
						bgrFrame->data, bgrFrame->linesize );

					cv::Mat mat( frameHeight, frameWidth, CV_8UC3, bgrFrame->data[0], bgrFrame->linesize[0] );

					// Run ONNX detection
					auto detections = DetectionFilter->DetectFrame( mat );
					for( auto& det : detections )
						detectedTags.insert( det.ClassName );

					gotFrame = true;
					av_packet_unref( pkt );
					break;
				}
			}
			av_packet_unref( pkt );
		}

		if( !gotFrame )
			break;

		currentTime += sampleInterval;
	}

	// Cleanup FFmpeg resources
	av_frame_free( &bgrFrame );
	av_frame_free( &frame );
	av_packet_free( &pkt );
	sws_freeContext( swsCtx );
	avcodec_free_context( &codecCtx );
	avformat_close_input( &fmtCtx );

	// Build combined tag string (merge with existing tags)
	std::set<std::string> allTags;
	if( !existingTags.empty() )
	{
		std::istringstream iss( existingTags );
		std::string tag;
		while( std::getline( iss, tag, ',' ) )
		{
			// Trim whitespace
			size_t start = tag.find_first_not_of( ' ' );
			size_t end = tag.find_last_not_of( ' ' );
			if( start != std::string::npos )
				allTags.insert( tag.substr( start, end - start + 1 ) );
		}
	}
	allTags.insert( detectedTags.begin(), detectedTags.end() );

	std::string tagString;
	for( auto& t : allTags )
	{
		if( !tagString.empty() )
			tagString += ",";
		tagString += t;
	}

	// Count remaining clips to reprocess
	int remaining = 0;
	{
		SQLiteDatabaseQueryInstance CountClipsToReprocess( Database, "CountClipsToReprocess" );
		CountClipsToReprocess->Bind( "@DetectionVersion", CURRENT_DETECTION_VERSION );
		CountClipsToReprocess->Execute( [&]( const SQLiteDatabaseQuery& query ) -> bool
		{
			remaining = query.GetColumnValueInt( 0 );
			return false;
		});
	}

	// Update clip in database
	{
		SQLiteDatabaseQueryInstance UpdateClipDetection( Database, "UpdateClipDetection" );
		UpdateClipDetection->Bind( "@ClipUID", clipUID );
		UpdateClipDetection->Bind( "@Tags", tagString.c_str() );
		UpdateClipDetection->Bind( "@DetectionVersion", CURRENT_DETECTION_VERSION );
		UpdateClipDetection->Execute( nullptr );
	}

	LOG_INFO( "Reprocessed clip %lld: %s (%d remaining)", (long long)clipUID, tagString.c_str(), remaining );

	// Sleep between clips to stay low-priority
	std::this_thread::sleep_for( std::chrono::seconds( 1 ) );
}
