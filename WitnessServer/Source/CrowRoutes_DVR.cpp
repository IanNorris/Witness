#include "CrowListener.h"
#include "CrowAuth.h"

#include <Log.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <charconv>
#include <limits>

namespace fs = std::filesystem;

namespace
{
	bool ParseDvrTime( const std::string& value, int64_t& result )
	{
		const auto parsed = std::from_chars( value.data(), value.data() + value.size(), result );
		return !value.empty() && parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size();
	}

	bool ParseDvrWindow( const std::string& fromStr, const std::string& toStr, int64_t& from, int64_t& to, int64_t maxSeconds = 7 * 24 * 60 * 60 )
	{
		return ParseDvrTime( fromStr, from ) && ParseDvrTime( toStr, to ) &&
			from >= 0 && to > from && to - from <= maxSeconds;
	}
}

void CrowListener::HandleDvrCoverage( const crow::request& req, crow::response& res, int cameraId, const std::string& fromStr, const std::string& toStr )
{
	if( CrowAuth::IsCameraAuthenticated( *m_GlobalContext, req, nullptr,
		CrowAuth::Action::Read, CrowAuth::Privilege::Normal, cameraId ) <= 0 )
	{
		res.code = 403;
		res.end();
		return;
	}

	int64_t from = 0, to = 0;
	if( !ParseDvrWindow( fromStr, toStr, from, to, 366LL * 24 * 60 * 60 ) ) { res.code = 400; res.end(); return; }

	SQLiteDatabaseQueryInstance query( m_GlobalContext->Database, "SelectContinuousCoverage" );
	query->Bind( "@CameraUID", cameraId );
	query->Bind( "@TimestampFrom", from );
	query->Bind( "@TimestampTo", to );

	// Collect raw segment ranges
	struct Range { int64_t from; int64_t to; };
	std::vector<Range> raw;

	query->Execute(
		[&]( const SQLiteDatabaseQuery& q )
		{
			Range r;
			r.from = q.GetColumnValueInt64( 0 );
			r.to = q.GetColumnValueInt64( 1 );
			raw.push_back( r );
			return true;
		}
	);

	// Merge contiguous/overlapping ranges
	std::vector<Range> merged;
	for( auto& r : raw )
	{
		if( !merged.empty() && r.from <= merged.back().to + 2 ) // 2s tolerance for gaps
		{
			merged.back().to = std::max( merged.back().to, r.to );
		}
		else
		{
			merged.push_back( r );
		}
	}

	crow::json::wvalue result;
	std::vector<crow::json::wvalue> ranges;
	for( auto& r : merged )
	{
		crow::json::wvalue range;
		range["from"] = r.from;
		range["to"] = r.to;
		ranges.push_back( std::move( range ) );
	}
	result["ranges"] = std::move( ranges );

	res.set_header( "Content-Type", "application/json" );
	res.body = result.dump();
	res.code = 200;
	res.end();
}

void CrowListener::HandleDvrSegment( const crow::request& req, crow::response& res, int segmentId )
{
	int cameraId = 0;
	std::string filePath;
	{
		SQLiteDatabaseQueryInstance query( m_GlobalContext->Database, "SelectContinuousSegmentByUID" );
		query->Bind( "@SegmentUID", segmentId );
		query->Execute( [&]( const SQLiteDatabaseQuery& q ) {
			cameraId = q.GetColumnValueInt( 0 );
			const char* path = q.GetColumnValueText( 1 );
			if( path ) filePath = path;
			return false;
		} );
	}
	if( cameraId <= 0 || filePath.empty() ) { res.code = 404; res.end(); return; }
	if( CrowAuth::IsCameraAuthenticated( *m_GlobalContext, req, nullptr,
		CrowAuth::Action::Read, CrowAuth::Privilege::Normal, cameraId ) <= 0 )
	{ res.code = 403; res.end(); return; }

	std::error_code ec;
	const uint64_t fileSize = fs::file_size( filePath, ec );
	if( ec || fileSize == 0 || fileSize > static_cast<uint64_t>( std::numeric_limits<int64_t>::max() ) )
	{ res.code = 404; res.end(); return; }

	const std::string range = req.get_header_value( "Range" );
	if( range.empty() )
	{
		// Crow sends the file in chunks instead of materialising the whole recording.
		res.set_static_file_info_unsafe( filePath, "video/mp4" );
		res.set_header( "Accept-Ranges", "bytes" );
		res.end();
		return;
	}

	const size_t dash = range.find( '-', 6 );
	int64_t start = 0, end = static_cast<int64_t>( fileSize ) - 1;
	bool valid = range.starts_with( "bytes=" ) && dash != std::string::npos &&
		range.find( ',', dash ) == std::string::npos;
	if( valid )
	{
		const std::string first = range.substr( 6, dash - 6 );
		const std::string last = range.substr( dash + 1 );
		if( first.empty() )
		{
			int64_t suffix = 0;
			valid = ParseDvrTime( last, suffix ) && suffix > 0;
			if( valid ) start = std::max<int64_t>( 0, static_cast<int64_t>( fileSize ) - suffix );
		}
		else
		{
			valid = ParseDvrTime( first, start ) && start >= 0;
			if( valid && !last.empty() ) valid = ParseDvrTime( last, end );
		}
	}
	if( !valid || start >= static_cast<int64_t>( fileSize ) || end < start )
	{
		res.code = 416;
		res.set_header( "Content-Range", "bytes */" + std::to_string( fileSize ) );
		res.end();
		return;
	}
	end = std::min<int64_t>( end, static_cast<int64_t>( fileSize ) - 1 );
	end = std::min<int64_t>( end, start + 2 * 1024 * 1024 - 1 );
	const size_t length = static_cast<size_t>( end - start + 1 );
	std::ifstream file( filePath, std::ios::binary );
	if( !file.seekg( start ) ) { res.code = 404; res.end(); return; }
	std::string body( length, '\0' );
	file.read( body.data(), static_cast<std::streamsize>( length ) );
	if( static_cast<size_t>( file.gcount() ) != length ) { res.code = 404; res.end(); return; }
	res.set_header( "Content-Type", "video/mp4" );
	res.set_header( "Accept-Ranges", "bytes" );
	res.set_header( "Content-Range", "bytes " + std::to_string( start ) + "-" + std::to_string( end ) + "/" + std::to_string( fileSize ) );
	res.body = std::move( body );
	res.code = 206;
	res.end();
}

void CrowListener::HandleDvrEvents( const crow::request& req, crow::response& res, int cameraId, const std::string& fromStr, const std::string& toStr )
{
	if( CrowAuth::IsCameraAuthenticated( *m_GlobalContext, req, nullptr,
		CrowAuth::Action::Read, CrowAuth::Privilege::Normal, cameraId ) <= 0 )
	{ res.code = 403; res.end(); return; }
	int64_t from = 0, to = 0;
	if( !ParseDvrWindow( fromStr, toStr, from, to ) ) { res.code = 400; res.end(); return; }

	std::vector<crow::json::wvalue> clips;
	{
		SQLiteDatabaseQueryInstance clipQuery( m_GlobalContext->Database, "SelectDvrActivity" );
		clipQuery->Bind( "@CameraUID", cameraId );
		clipQuery->Bind( "@TimestampFrom", from );
		clipQuery->Bind( "@TimestampTo", to );
		clipQuery->Execute( [&]( const SQLiteDatabaseQuery& q ) {
			crow::json::wvalue item;
			item["id"] = q.GetColumnValueInt64( 0 );
			item["from"] = q.GetColumnValueInt64( 1 );
			item["to"] = q.GetColumnValueInt64( 1 ) + std::max( q.GetColumnValueInt( 2 ), 1 );
			clips.push_back( std::move( item ) );
			return true;
		} );
	}
	std::vector<crow::json::wvalue> audio;
	SQLiteDatabaseQueryInstance audioQuery( m_GlobalContext->Database, "SelectDvrAudio" );
	audioQuery->Bind( "@CameraUID", cameraId );
	audioQuery->Bind( "@TimestampFrom", from );
	audioQuery->Bind( "@TimestampTo", to );
	audioQuery->Execute( [&]( const SQLiteDatabaseQuery& q ) {
		crow::json::wvalue item;
		const char* group = q.GetColumnValueText( 0 );
		item["group"] = group ? group : "";
		item["from"] = q.GetColumnValueDouble( 1 );
		item["to"] = q.GetColumnValueDouble( 2 );
		item["score"] = q.GetColumnValueDouble( 3 );
		audio.push_back( std::move( item ) );
		return true;
	} );
	crow::json::wvalue result;
	result["clipsTruncated"] = clips.size() > 500;
	result["audioTruncated"] = audio.size() > 500;
	if( clips.size() > 500 ) clips.pop_back();
	if( audio.size() > 500 ) audio.pop_back();
	result["clips"] = std::move( clips );
	result["audio"] = std::move( audio );
	res.set_header( "Content-Type", "application/json" );
	res.body = result.dump();
	res.end();
}

void CrowListener::HandleDvrSegments( const crow::request& req, crow::response& res, int cameraId, const std::string& fromStr, const std::string& toStr )
{
	if( CrowAuth::IsCameraAuthenticated( *m_GlobalContext, req, nullptr,
		CrowAuth::Action::Read, CrowAuth::Privilege::Normal, cameraId ) <= 0 )
	{
		res.code = 403;
		res.end();
		return;
	}

	int64_t from = 0, to = 0;
	if( !ParseDvrWindow( fromStr, toStr, from, to ) ) { res.code = 400; res.end(); return; }

	SQLiteDatabaseQueryInstance query( m_GlobalContext->Database, "SelectContinuousSegments" );
	query->Bind( "@CameraUID", cameraId );
	query->Bind( "@TimestampFrom", from );
	query->Bind( "@TimestampTo", to );

	crow::json::wvalue result;
	std::vector<crow::json::wvalue> segments;

	query->Execute(
		[&]( const SQLiteDatabaseQuery& q )
		{
			crow::json::wvalue seg;
			seg["id"] = q.GetColumnValueInt64( 0 );
			seg["from"] = q.GetColumnValueInt64( 2 );
			seg["to"] = q.GetColumnValueInt64( 3 );
			seg["duration"] = q.GetColumnValueInt( 4 );
			segments.push_back( std::move( seg ) );
			return true;
		}
	);

	result["segments"] = std::move( segments );

	res.set_header( "Content-Type", "application/json" );
	res.body = result.dump();
	res.code = 200;
	res.end();
}

void CrowListener::HandleDvrPlaylist( const crow::request& req, crow::response& res, int cameraId, const std::string& fromStr, const std::string& toStr )
{
	if( CrowAuth::IsCameraAuthenticated( *m_GlobalContext, req, nullptr,
		CrowAuth::Action::Read, CrowAuth::Privilege::Normal, cameraId ) <= 0 )
	{
		res.code = 403;
		res.end();
		return;
	}

	int64_t from = 0, to = 0;
	if( !ParseDvrWindow( fromStr, toStr, from, to ) ) { res.code = 400; res.end(); return; }

	SQLiteDatabaseQueryInstance query( m_GlobalContext->Database, "SelectContinuousSegments" );
	query->Bind( "@CameraUID", cameraId );
	query->Bind( "@TimestampFrom", from );
	query->Bind( "@TimestampTo", to );

	struct Segment
	{
		int64_t uid;
		int duration;
	};
	std::vector<Segment> segments;

	query->Execute(
		[&]( const SQLiteDatabaseQuery& q )
		{
			Segment s;
			s.uid = q.GetColumnValueInt64( 0 );
			s.duration = q.GetColumnValueInt( 4 );
			if( s.duration <= 0 ) s.duration = 1;
			segments.push_back( s );
			return true;
		}
	);

	if( segments.empty() )
	{
		LOG_WARNING("[DVR] Playlist empty: cam=%d from=%lld to=%lld", cameraId, from, to);
		res.code = 404;
		res.body = "No segments found";
		res.end();
		return;
	}

	// Find max segment duration for EXT-X-TARGETDURATION
	int maxDuration = 0;
	for( auto& s : segments )
	{
		if( s.duration > maxDuration ) maxDuration = s.duration;
	}

	std::ostringstream m3u8;
	m3u8 << "#EXTM3U\n";
	m3u8 << "#EXT-X-VERSION:3\n";
	m3u8 << "#EXT-X-TARGETDURATION:" << maxDuration << "\n";
	m3u8 << "#EXT-X-PLAYLIST-TYPE:VOD\n";
	m3u8 << "#EXT-X-MEDIA-SEQUENCE:0\n";

	for( auto& s : segments )
	{
		m3u8 << "#EXTINF:" << s.duration << ".000,\n";
		m3u8 << "/dvr/segment/" << s.uid << "\n";
	}

	m3u8 << "#EXT-X-ENDLIST\n";

	res.set_header( "Content-Type", "application/vnd.apple.mpegurl" );
	res.body = m3u8.str();
	res.code = 200;
	res.end();

	LOG_DEBUG("[DVR] Playlist cam=%d segs=%d from=%lld to=%lld maxDur=%d", cameraId, (int)segments.size(), from, to, maxDuration);
}

// --- DVR Thumbnail ---

#include <mutex>
#include <list>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
}

#include <opencv2/core/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>

namespace
{
	struct ThumbnailCacheEntry
	{
		std::string Key;
		std::vector<uint8_t> JpegData;
	};

	std::mutex g_ThumbnailCacheMutex;
	std::list<ThumbnailCacheEntry> g_ThumbnailCache;
	const size_t THUMBNAIL_CACHE_MAX = 50;

	std::vector<uint8_t>* FindCachedThumbnail( const std::string& key )
	{
		for( auto it = g_ThumbnailCache.begin(); it != g_ThumbnailCache.end(); ++it )
		{
			if( it->Key == key )
			{
				// Move to front (MRU)
				if( it != g_ThumbnailCache.begin() )
					g_ThumbnailCache.splice( g_ThumbnailCache.begin(), g_ThumbnailCache, it );
				return &g_ThumbnailCache.front().JpegData;
			}
		}
		return nullptr;
	}

	void InsertCachedThumbnail( const std::string& key, std::vector<uint8_t> jpeg )
	{
		if( g_ThumbnailCache.size() >= THUMBNAIL_CACHE_MAX )
			g_ThumbnailCache.pop_back();

		g_ThumbnailCache.push_front( { key, std::move( jpeg ) } );
	}
}

void CrowListener::HandleDvrThumbnail( const crow::request& req, crow::response& res, int cameraId, const std::string& timestampStr )
{
	if( CrowAuth::IsCameraAuthenticated( *m_GlobalContext, req, nullptr,
		CrowAuth::Action::Read, CrowAuth::Privilege::Normal, cameraId ) <= 0 )
	{
		res.code = 403;
		res.end();
		return;
	}

	int64_t timestamp = 0;
	if( !ParseDvrTime( timestampStr, timestamp ) || timestamp < 0 )
	{
		res.code = 400;
		res.end();
		return;
	}

	// Find the segment containing this timestamp
	std::string filePath;
	int64_t segStart = 0;

	SQLiteDatabaseQueryInstance query( m_GlobalContext->Database, "SelectContinuousSegmentAtTimestamp" );
	query->Bind( "@CameraUID", cameraId );
	query->Bind( "@Timestamp", timestamp );
	query->Execute(
		[&]( const SQLiteDatabaseQuery& q )
		{
			const char* path = q.GetColumnValueText( 1 );
			if( path ) filePath = path;
			segStart = q.GetColumnValueInt64( 2 );
			return false; // only need first result
		}
	);

	if( filePath.empty() || !fs::exists( filePath ) )
	{
		res.code = 404;
		res.end();
		return;
	}

	// Check LRU cache
	std::string cacheKey = std::to_string( cameraId ) + ":" + std::to_string( timestamp );
	{
		const std::lock_guard<std::mutex> lock( g_ThumbnailCacheMutex );
		auto* cached = FindCachedThumbnail( cacheKey );
		if( cached )
		{
			res.set_header( "Content-Type", "image/jpeg" );
			res.set_header( "Cache-Control", "public, max-age=3600" );
			res.body.assign( (const char*)cached->data(), cached->size() );
			res.code = 200;
			res.end();
			return;
		}
	}

	// Open the MP4 with FFmpeg and extract a frame
	AVFormatContext* fmtCtx = nullptr;
	if( avformat_open_input( &fmtCtx, filePath.c_str(), nullptr, nullptr ) < 0 )
	{
		LOG_ERROR( "[DVR] Thumbnail: Failed to open %s", filePath.c_str() );
		res.code = 500;
		res.end();
		return;
	}

	if( avformat_find_stream_info( fmtCtx, nullptr ) < 0 )
	{
		avformat_close_input( &fmtCtx );
		res.code = 500;
		res.end();
		return;
	}

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
		avformat_close_input( &fmtCtx );
		res.code = 500;
		res.end();
		return;
	}

	auto codecpar = fmtCtx->streams[videoIdx]->codecpar;
	auto codec = avcodec_find_decoder( codecpar->codec_id );
	if( !codec )
	{
		avformat_close_input( &fmtCtx );
		res.code = 500;
		res.end();
		return;
	}

	auto codecCtx = avcodec_alloc_context3( codec );
	avcodec_parameters_to_context( codecCtx, codecpar );
	if( avcodec_open2( codecCtx, codec, nullptr ) < 0 )
	{
		avcodec_free_context( &codecCtx );
		avformat_close_input( &fmtCtx );
		res.code = 500;
		res.end();
		return;
	}

	// Seek to the requested position within the segment
	double seekSec = (double)( timestamp - segStart );
	if( seekSec < 0 ) seekSec = 0;
	int64_t seekTarget = (int64_t)( seekSec * AV_TIME_BASE );
	av_seek_frame( fmtCtx, -1, seekTarget, AVSEEK_FLAG_BACKWARD );
	avcodec_flush_buffers( codecCtx );

	// Decode one frame
	AVPacket* pkt = av_packet_alloc();
	AVFrame* frame = av_frame_alloc();
	bool gotFrame = false;

	while( av_read_frame( fmtCtx, pkt ) >= 0 )
	{
		if( pkt->stream_index == videoIdx )
		{
			avcodec_send_packet( codecCtx, pkt );
			if( avcodec_receive_frame( codecCtx, frame ) == 0 )
			{
				gotFrame = true;
				av_packet_unref( pkt );
				break;
			}
		}
		av_packet_unref( pkt );
	}

	if( !gotFrame )
	{
		av_frame_free( &frame );
		av_packet_free( &pkt );
		avcodec_free_context( &codecCtx );
		avformat_close_input( &fmtCtx );
		res.code = 404;
		res.end();
		return;
	}

	// Convert to BGR24
	int w = codecCtx->width;
	int h = codecCtx->height;

	SwsContext* swsCtx = sws_getContext(
		w, h, codecCtx->pix_fmt,
		w, h, AV_PIX_FMT_BGR24,
		SWS_BILINEAR, nullptr, nullptr, nullptr );

	if( !swsCtx )
	{
		av_frame_free( &frame );
		av_packet_free( &pkt );
		avcodec_free_context( &codecCtx );
		avformat_close_input( &fmtCtx );
		res.code = 500;
		res.end();
		return;
	}

	AVFrame* bgrFrame = av_frame_alloc();
	int bgrBufSize = av_image_get_buffer_size( AV_PIX_FMT_BGR24, w, h, 1 );
	std::vector<uint8_t> bgrBuffer( bgrBufSize );
	av_image_fill_arrays( bgrFrame->data, bgrFrame->linesize, bgrBuffer.data(),
		AV_PIX_FMT_BGR24, w, h, 1 );

	sws_scale( swsCtx, frame->data, frame->linesize, 0, h,
		bgrFrame->data, bgrFrame->linesize );

	cv::Mat mat( h, w, CV_8UC3, bgrFrame->data[0], bgrFrame->linesize[0] );

	// Resize to 300px wide
	int thumbW = 300;
	int thumbH = (int)( (double)h / w * thumbW );
	cv::Mat thumb;
	cv::resize( mat, thumb, cv::Size( thumbW, thumbH ), 0, 0, cv::INTER_AREA );

	// Encode to JPEG
	std::vector<uint8_t> jpegBuf;
	std::vector<int> params = { cv::IMWRITE_JPEG_QUALITY, 70 };
	cv::imencode( ".jpg", thumb, jpegBuf, params );

	// Cleanup FFmpeg
	av_frame_free( &bgrFrame );
	av_frame_free( &frame );
	av_packet_free( &pkt );
	sws_freeContext( swsCtx );
	avcodec_free_context( &codecCtx );
	avformat_close_input( &fmtCtx );

	// Cache it
	{
		const std::lock_guard<std::mutex> lock( g_ThumbnailCacheMutex );
		InsertCachedThumbnail( cacheKey, jpegBuf );
	}

	// Return JPEG
	res.set_header( "Content-Type", "image/jpeg" );
	res.set_header( "Cache-Control", "public, max-age=3600" );
	res.body.assign( (const char*)jpegBuf.data(), jpegBuf.size() );
	res.code = 200;
	res.end();
}
