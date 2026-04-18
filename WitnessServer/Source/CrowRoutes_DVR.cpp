#include "CrowListener.h"
#include "CrowAuth.h"

#include <Log.h>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

void CrowListener::HandleDvrCoverage( const crow::request& req, crow::response& res, int cameraId, const std::string& fromStr, const std::string& toStr )
{
	int UserUID = CrowAuth::IsAuthenticated( *m_GlobalContext, req, nullptr,
		CrowAuth::Action::Read, CrowAuth::Privilege::Normal );
	if( UserUID < 0 )
	{
		res.code = 401;
		res.end();
		return;
	}

	int64_t from = std::stoll( fromStr );
	int64_t to = std::stoll( toStr );

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
	int UserUID = CrowAuth::IsAuthenticated( *m_GlobalContext, req, nullptr,
		CrowAuth::Action::Read, CrowAuth::Privilege::Normal );
	if( UserUID < 0 )
	{
		res.code = 401;
		res.end();
		return;
	}

	// Look up segment file path
	std::string filePath;

	SQLiteDatabaseQueryInstance query( m_GlobalContext->Database, "SelectContinuousSegments" );
	// We need a query by SegmentUID -- reuse SelectContinuousSegments isn't ideal,
	// so let's look up by UID directly. We'll query with a broad range and filter.
	// Actually, let's just find the file path for this segment UID.
	// For now, do a simple lookup.

	// We need a dedicated query -- let's use a direct approach
	std::string sql = "SELECT FilePath FROM ContinuousSegment WHERE SegmentUID = " + std::to_string( segmentId );
	sqlite3_stmt* stmt = nullptr;
	int rc = sqlite3_prepare_v2( m_GlobalContext->Database->GetDatabase(), sql.c_str(), -1, &stmt, nullptr );
	if( rc == SQLITE_OK )
	{
		if( sqlite3_step( stmt ) == SQLITE_ROW )
		{
			const char* path = (const char*)sqlite3_column_text( stmt, 0 );
			if( path ) filePath = path;
		}
		sqlite3_finalize( stmt );
	}

	if( filePath.empty() || !fs::exists( filePath ) )
	{
		LOG_WARNING("[DVR] Segment 404: uid=%d path=%s", segmentId, filePath.empty() ? "(not found)" : filePath.c_str());
		res.code = 404;
		res.end();
		return;
	}

	// Get file size
	auto fileSize = fs::file_size( filePath );

	// Check for Range header
	std::string rangeHeader;
	auto it = req.headers.find( "Range" );
	if( it != req.headers.end() )
	{
		rangeHeader = it->second;
	}

	if( !rangeHeader.empty() && rangeHeader.substr( 0, 6 ) == "bytes=" )
	{
		// Parse range: bytes=start-end or bytes=start-
		std::string rangeSpec = rangeHeader.substr( 6 );
		size_t dashPos = rangeSpec.find( '-' );
		int64_t rangeStart = 0;
		int64_t rangeEnd = (int64_t)fileSize - 1;

		if( dashPos != std::string::npos )
		{
			std::string startStr = rangeSpec.substr( 0, dashPos );
			std::string endStr = rangeSpec.substr( dashPos + 1 );

			if( !startStr.empty() ) rangeStart = std::stoll( startStr );
			if( !endStr.empty() ) rangeEnd = std::stoll( endStr );
		}

		// Clamp
		if( rangeStart < 0 ) rangeStart = 0;
		if( rangeEnd >= (int64_t)fileSize ) rangeEnd = (int64_t)fileSize - 1;
		int64_t contentLength = rangeEnd - rangeStart + 1;

		std::ifstream file( filePath, std::ios::binary );
		file.seekg( rangeStart );
		std::string body( contentLength, '\0' );
		file.read( &body[0], contentLength );

		res.set_header( "Content-Type", "video/mp4" );
		res.set_header( "Accept-Ranges", "bytes" );
		res.set_header( "Content-Range", "bytes " + std::to_string( rangeStart ) + "-" + std::to_string( rangeEnd ) + "/" + std::to_string( fileSize ) );
		res.body = std::move( body );
		res.code = 206;
		res.end();
	}
	else
	{
		// Full file
		std::ifstream file( filePath, std::ios::binary );
		std::string body( (std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>() );

		res.set_header( "Content-Type", "video/mp4" );
		res.set_header( "Accept-Ranges", "bytes" );
		res.set_header( "Content-Length", std::to_string( fileSize ) );
		res.body = std::move( body );
		res.code = 200;
		res.end();
	}
}

void CrowListener::HandleDvrSegments( const crow::request& req, crow::response& res, int cameraId, const std::string& fromStr, const std::string& toStr )
{
	int UserUID = CrowAuth::IsAuthenticated( *m_GlobalContext, req, nullptr,
		CrowAuth::Action::Read, CrowAuth::Privilege::Normal );
	if( UserUID < 0 )
	{
		res.code = 401;
		res.end();
		return;
	}

	int64_t from = std::stoll( fromStr );
	int64_t to = std::stoll( toStr );

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
	int UserUID = CrowAuth::IsAuthenticated( *m_GlobalContext, req, nullptr,
		CrowAuth::Action::Read, CrowAuth::Privilege::Normal );
	if( UserUID < 0 )
	{
		res.code = 401;
		res.end();
		return;
	}

	int64_t from = std::stoll( fromStr );
	int64_t to = std::stoll( toStr );

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
	int UserUID = CrowAuth::IsAuthenticated( *m_GlobalContext, req, nullptr,
		CrowAuth::Action::Read, CrowAuth::Privilege::Normal );
	if( UserUID < 0 )
	{
		res.code = 401;
		res.end();
		return;
	}

	int64_t timestamp = 0;
	try { timestamp = std::stoll( timestampStr ); }
	catch( ... )
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
