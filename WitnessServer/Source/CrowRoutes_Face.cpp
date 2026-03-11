#include "CrowListener.h"
#include "CrowAuth.h"
#include "GlobalContext.h"
#include "SQLite.h"

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

void CrowListener::HandleFaceQuery( const crow::request& req, crow::response& res, int cameraId )
{
	int UserUID = CrowAuth::IsAuthenticated( *m_GlobalContext, req, nullptr,
		CrowAuth::Action::Read, CrowAuth::Privilege::Normal );
	if( UserUID < 0 )
	{
		res.code = 401;
		res.end();
		return;
	}

	auto fromParam = req.url_params.get( "from" );
	auto toParam = req.url_params.get( "to" );

	if( !fromParam || !toParam )
	{
		res.code = 400;
		res.body = R"({"error":"Missing from/to query params"})";
		res.set_header( "Content-Type", "application/json" );
		res.end();
		return;
	}

	double from = std::stod( fromParam );
	double to = std::stod( toParam );

	crow::json::wvalue result;
	std::vector<crow::json::wvalue> faces;

	try
	{
		SQLiteDatabaseQueryInstance query( m_GlobalContext->Database, "SelectFaceCrops" );
		query->Bind( "@CameraID", cameraId );
		query->Bind( "@TimestampFrom", from );
		query->Bind( "@TimestampTo", to );

		query->Execute( [&]( const SQLiteDatabaseQuery& q )
		{
			crow::json::wvalue face;
			face["id"] = q.GetColumnValueInt64( 0 );       // CropUID
			face["cameraId"] = q.GetColumnValueInt( 1 );    // CameraID
			face["t"] = q.GetColumnValueDouble( 2 );        // Timestamp
			face["trackId"] = q.GetColumnValueInt( 3 );     // TrackingID
			face["conf"] = q.GetColumnValueDouble( 6 );     // Confidence

			// 5-point landmarks (normalized 0-1)
			std::vector<crow::json::wvalue> landmarks;
			for( int i = 0; i < 5; i++ )
			{
				crow::json::wvalue lm;
				lm["x"] = q.GetColumnValueDouble( 7 + i * 2 );
				lm["y"] = q.GetColumnValueDouble( 8 + i * 2 );
				landmarks.push_back( std::move( lm ) );
			}
			face["landmarks"] = std::move( landmarks );

			faces.push_back( std::move( face ) );

			if( faces.size() >= 1000 )
				return false;
			return true;
		});
	}
	catch( const std::exception& e )
	{
		res.code = 500;
		crow::json::wvalue err;
		err["error"] = std::string( e.what() );
		res.set_header( "Content-Type", "application/json" );
		res.body = err.dump();
		res.end();
		return;
	}

	result["faces"] = std::move( faces );
	result["cameraId"] = cameraId;

	res.set_header( "Content-Type", "application/json" );
	res.body = result.dump();
	res.code = 200;
	res.end();
}

void CrowListener::HandleFaceRecent( const crow::request& req, crow::response& res )
{
	int UserUID = CrowAuth::IsAuthenticated( *m_GlobalContext, req, nullptr,
		CrowAuth::Action::Read, CrowAuth::Privilege::Normal );
	if( UserUID < 0 )
	{
		res.code = 401;
		res.end();
		return;
	}

	int limit = 50;
	auto limitParam = req.url_params.get( "limit" );
	if( limitParam )
		limit = std::min( std::max( std::stoi( limitParam ), 1 ), 500 );

	crow::json::wvalue result;
	std::vector<crow::json::wvalue> faces;

	try
	{
		SQLiteDatabaseQueryInstance query( m_GlobalContext->Database, "SelectRecentFaceCrops" );
		query->Bind( "@Limit", limit );

		query->Execute( [&]( const SQLiteDatabaseQuery& q )
		{
			crow::json::wvalue face;
			face["id"] = q.GetColumnValueInt64( 0 );       // CropUID
			face["cameraId"] = q.GetColumnValueInt( 1 );    // CameraID
			face["t"] = q.GetColumnValueDouble( 2 );        // Timestamp
			face["trackId"] = q.GetColumnValueInt( 4 );     // TrackingID
			face["conf"] = q.GetColumnValueDouble( 6 );     // Confidence
			faces.push_back( std::move( face ) );
			return true;
		});
	}
	catch( const std::exception& e )
	{
		res.code = 500;
		crow::json::wvalue err;
		err["error"] = std::string( e.what() );
		res.set_header( "Content-Type", "application/json" );
		res.body = err.dump();
		res.end();
		return;
	}

	result["faces"] = std::move( faces );

	res.set_header( "Content-Type", "application/json" );
	res.body = result.dump();
	res.code = 200;
	res.end();
}

void CrowListener::HandleFaceCropImage( const crow::request& req, crow::response& res, int cropUID )
{
	int UserUID = CrowAuth::IsAuthenticated( *m_GlobalContext, req, nullptr,
		CrowAuth::Action::Read, CrowAuth::Privilege::Normal );
	if( UserUID < 0 )
	{
		res.code = 401;
		res.end();
		return;
	}

	std::string filePath;

	try
	{
		SQLiteDatabaseQueryInstance query( m_GlobalContext->Database, "SelectFaceCropByUID" );
		query->Bind( "@CropUID", cropUID );

		query->Execute( [&]( const SQLiteDatabaseQuery& q )
		{
			const char* path = q.GetColumnValueText( 5 ); // FilePath
			if( path )
				filePath = path;
			return false;
		});
	}
	catch( const std::exception& )
	{
		res.code = 500;
		res.end();
		return;
	}

	if( filePath.empty() || !fs::exists( filePath ) )
	{
		res.code = 404;
		res.end();
		return;
	}

	std::ifstream file( filePath, std::ios::binary );
	if( file )
	{
		std::string body( (std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>() );
		res.set_header( "Content-Type", "image/jpeg" );
		res.set_header( "Cache-Control", "public, max-age=86400" );
		res.body = std::move( body );
		res.code = 200;
		res.end();
		return;
	}

	res.code = 500;
	res.end();
}

void CrowListener::HandleDetectionCropImage( const crow::request& req, crow::response& res, const std::string& cropPath )
{
	int UserUID = CrowAuth::IsAuthenticated( *m_GlobalContext, req, nullptr,
		CrowAuth::Action::Read, CrowAuth::Privilege::Normal );
	if( UserUID < 0 )
	{
		res.code = 401;
		res.end();
		return;
	}

	// cropPath is relative to CachePath — validate it doesn't escape
	fs::path fullPath = fs::path( m_GlobalContext->CachePath ) / "crops" / cropPath;
	fs::path canonical = fs::weakly_canonical( fullPath );
	fs::path cacheCanonical = fs::weakly_canonical( fs::path( m_GlobalContext->CachePath ) );

	// Ensure path stays within CachePath
	auto mismatch = std::mismatch( cacheCanonical.begin(), cacheCanonical.end(), canonical.begin() );
	if( mismatch.first != cacheCanonical.end() )
	{
		res.code = 403;
		res.end();
		return;
	}

	if( !fs::exists( fullPath ) )
	{
		res.code = 404;
		res.end();
		return;
	}

	std::ifstream file( fullPath, std::ios::binary );
	if( file )
	{
		std::string body( (std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>() );
		res.set_header( "Content-Type", "image/jpeg" );
		res.set_header( "Cache-Control", "public, max-age=86400" );
		res.body = std::move( body );
		res.code = 200;
		res.end();
		return;
	}

	res.code = 500;
	res.end();
}
