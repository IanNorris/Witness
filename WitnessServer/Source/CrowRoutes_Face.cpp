#include "CrowListener.h"
#include "CrowAuth.h"
#include "GlobalContext.h"
#include "SQLite.h"

#include <filesystem>
#include <fstream>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

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

	bool showOverlay = req.url_params.get( "overlay" ) != nullptr;

	std::string filePath;
	float lmX[5] = {}, lmY[5] = {};

	try
	{
		SQLiteDatabaseQueryInstance query( m_GlobalContext->Database, "SelectFaceCropByUID" );
		query->Bind( "@CropUID", cropUID );

		query->Execute( [&]( const SQLiteDatabaseQuery& q )
		{
			const char* path = q.GetColumnValueText( 5 ); // FilePath
			if( path )
				filePath = path;
			if( showOverlay )
			{
				for( int i = 0; i < 5; i++ )
				{
					lmX[i] = (float)q.GetColumnValueDouble( 7 + i * 2 );
					lmY[i] = (float)q.GetColumnValueDouble( 8 + i * 2 );
				}
			}
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

	if( showOverlay )
	{
		// Read image, draw landmark overlay, and re-encode
		cv::Mat crop = cv::imread( filePath );
		if( !crop.empty() )
		{
			int w = crop.cols, h = crop.rows;

			// Convert normalized landmarks to pixel coordinates
			float px[5], py[5];
			bool hasLm = false;
			for( int i = 0; i < 5; i++ )
			{
				px[i] = lmX[i] * (float)w;
				py[i] = lmY[i] * (float)h;
				if( lmX[i] != 0.0f || lmY[i] != 0.0f ) hasLm = true;
			}

			if( hasLm )
			{
				// Compute orientation metrics
				float eyeMidX = ( px[0] + px[1] ) * 0.5f;
				float eyeAvgY = ( py[0] + py[1] ) * 0.5f;
				float noseOffsetX = std::abs( px[2] - eyeMidX );
				float mouthMidX = ( px[3] + px[4] ) * 0.5f;
				float mouthSymmetry = std::abs( mouthMidX - eyeMidX );
				float interEyeDist = std::abs( px[1] - px[0] );
				float eyeHeightDiff = std::abs( py[0] - py[1] );
				float mouthAvgY = ( py[3] + py[4] ) * 0.5f;

				// Determine quality: green=frontal, yellow=marginal, red=rejected
				bool verticalOk = eyeAvgY < py[2] && py[2] < mouthAvgY;
				bool frontal = verticalOk && interEyeDist > 25.0f && noseOffsetX < 15.0f
					&& mouthSymmetry < 12.0f && eyeHeightDiff < 12.0f;
				bool marginal = verticalOk && interEyeDist > 15.0f && noseOffsetX < 20.0f
					&& mouthSymmetry < 18.0f && eyeHeightDiff < 18.0f;

				cv::Scalar color = frontal ? cv::Scalar( 0, 200, 0 )    // green
					: marginal ? cv::Scalar( 0, 200, 255 )              // yellow
					: cv::Scalar( 0, 0, 255 );                          // red

				// Draw landmark dots
				for( int i = 0; i < 5; i++ )
					cv::circle( crop, cv::Point( (int)px[i], (int)py[i] ), 2, color, -1, cv::LINE_AA );

				// Eye-to-eye line
				cv::line( crop, cv::Point( (int)px[0], (int)py[0] ), cv::Point( (int)px[1], (int)py[1] ),
					color, 1, cv::LINE_AA );

				// Eye midpoint to nose line
				cv::line( crop, cv::Point( (int)eyeMidX, (int)eyeAvgY ), cv::Point( (int)px[2], (int)py[2] ),
					color, 1, cv::LINE_AA );

				// Nose to mouth center
				float mouthMidY = ( py[3] + py[4] ) * 0.5f;
				cv::line( crop, cv::Point( (int)px[2], (int)py[2] ), cv::Point( (int)mouthMidX, (int)mouthMidY ),
					color, 1, cv::LINE_AA );

				// Mouth line
				cv::line( crop, cv::Point( (int)px[3], (int)py[3] ), cv::Point( (int)px[4], (int)py[4] ),
					color, 1, cv::LINE_AA );

				// Status label
				const char* label = frontal ? "OK" : marginal ? "MARGINAL" : "REJECTED";
				cv::putText( crop, label, cv::Point( 2, 10 ), cv::FONT_HERSHEY_SIMPLEX, 0.3, color, 1, cv::LINE_AA );
			}

			// Encode to JPEG
			std::vector<uchar> buf;
			std::vector<int> params = { cv::IMWRITE_JPEG_QUALITY, 90 };
			cv::imencode( ".jpg", crop, buf, params );

			res.set_header( "Content-Type", "image/jpeg" );
			res.body = std::string( buf.begin(), buf.end() );
			res.code = 200;
			res.end();
			return;
		}
	}

	// Default: serve raw file
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
