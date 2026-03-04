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
	// We need a query by SegmentUID — reuse SelectContinuousSegments isn't ideal,
	// so let's look up by UID directly. We'll query with a broad range and filter.
	// Actually, let's just find the file path for this segment UID.
	// For now, do a simple lookup.

	// We need a dedicated query — let's use a direct approach
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
}
