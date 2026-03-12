#include "CrowListener.h"
#include "CrowAuth.h"
#include "SetupConfig.h"
#include "GlobalContext.h"

#include <ONNXDetectionFilter.h>
#include <Log.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>

namespace fs = std::filesystem;

void CrowListener::HandleSetupPage( const crow::request& req, crow::response& res )
{
	// Require admin authentication
	int UserUID = CrowAuth::IsAuthenticated( *m_GlobalContext, req, nullptr,
		CrowAuth::Action::Read, CrowAuth::Privilege::Administrator );
	if( UserUID < 0 )
	{
		res.code = 401;
		res.body = "Admin authentication required. <a href=\"/\">Login first</a>";
		res.set_header( "Content-Type", "text/html" );
		res.end();
		return;
	}

	// Serve the setup wizard page from Web/setup/
	fs::path indexPath = fs::path( m_StaticRoot ) / "setup" / "index.html";
	std::ifstream file( indexPath, std::ios::binary );
	if( file )
	{
		std::string body( (std::istreambuf_iterator<char>(file)),
		                  std::istreambuf_iterator<char>() );
		res.set_header( "Content-Type", "text/html" );
		res.body = std::move( body );
		res.code = 200;
	}
	else
	{
		res.code = 404;
		res.body = "Setup page not found.";
	}
	res.end();
}

void CrowListener::HandleSetupSettings( const crow::request& req, crow::response& res )
{
	// Require admin authentication
	int UserUID = CrowAuth::IsAuthenticated( *m_GlobalContext, req, nullptr,
		CrowAuth::Action::Read, CrowAuth::Privilege::Administrator );
	if( UserUID < 0 )
	{
		res.code = 401;
		res.end();
		return;
	}

	// Read all current settings from DB
	crow::json::wvalue data;
	data["mode"] = "reconfigure";

	{
		SQLiteDatabaseQueryInstance query( m_GlobalContext->Database, "GetAllSettings" );
		query->Execute( [&data]( const SQLiteDatabaseQuery& q )
		{
			const char* name = q.GetColumnValueText( 0 );
			const char* value = q.GetColumnValueText( 1 );
			if( name && value )
			{
				data[name] = std::string( value );
			}
			return true;
		});
	}

	res.set_header( "Content-Type", "application/json" );
	res.body = data.dump();
	res.code = 200;
	res.end();
}

void CrowListener::HandleSetupApply( const crow::request& req, crow::response& res )
{
	auto body = crow::json::load( req.body );
	if( !body )
	{
		res.code = 400;
		res.body = R"({"error":"Invalid JSON"})";
		res.set_header( "Content-Type", "application/json" );
		res.end();
		return;
	}

	// Require admin authentication
	int UserUID = CrowAuth::IsAuthenticated( *m_GlobalContext, req, &body,
		CrowAuth::Action::ReadWrite, CrowAuth::Privilege::Administrator );
	if( UserUID < 0 )
	{
		res.code = 401;
		res.end();
		return;
	}

	// Build config (no username/password for reconfiguration)
	SetupConfig config;
	if( body.has( "hostname" ) )    config.Hostname    = body["hostname"].s();
	if( body.has( "cache_path" ) )  config.CachePath   = body["cache_path"].s();
	if( body.has( "tls_mode" ) )    config.TlsMode     = body["tls_mode"].s();
	if( body.has( "tls_cert" ) )    config.TlsCertPath = body["tls_cert"].s();
	if( body.has( "tls_key" ) )     config.TlsKeyPath  = body["tls_key"].s();
	if( body.has( "tls_contact" ) ) config.TlsContact  = body["tls_contact"].s();
	if( body.has( "detection_backend" ) )    config.DetectionBackend    = body["detection_backend"].s();
	if( body.has( "detection_provider" ) )   config.DetectionProvider   = body["detection_provider"].s();
	if( body.has( "detection_confidence" ) ) config.DetectionConfidence = body["detection_confidence"].s();
	if( body.has( "detection_max_fps" ) )    config.DetectionMaxFPS     = body["detection_max_fps"].s();
	if( body.has( "cudnn_path" ) )           config.CudnnPath           = body["cudnn_path"].s();
	if( body.has( "clip_cleanup_enabled" ) ) config.ClipCleanupEnabled  = body["clip_cleanup_enabled"].s();
	if( body.has( "clip_retention_days" ) )  config.ClipRetentionDays   = body["clip_retention_days"].s();

	// Apply settings to database
	if( !config.ApplyToDatabase( m_GlobalContext->Database ) )
	{
		res.code = 500;
		res.body = R"({"error":"Failed to apply settings"})";
		res.set_header( "Content-Type", "application/json" );
		res.end();
		return;
	}

	LOG_INFO( "Settings updated via /setup by admin (UserUID=%d)", UserUID );

	// Reload TLS if cert paths changed
	if( !config.TlsCertPath.empty() || !config.TlsKeyPath.empty() )
	{
		if( !config.TlsCertPath.empty() ) m_CertPath = config.TlsCertPath;
		if( !config.TlsKeyPath.empty() )  m_KeyPath = config.TlsKeyPath;
		ReloadTLS();
	}

	crow::json::wvalue result;
	result["success"] = true;
	result["message"] = "Settings applied. Some changes may require a server restart.";

	res.set_header( "Content-Type", "application/json" );
	res.body = result.dump();
	res.code = 200;
	res.end();
}

void CrowListener::HandleSetupTestCuda( const crow::request& req, crow::response& res )
{
	auto body = crow::json::load( req.body );
	if( !body )
	{
		res.code = 400;
		res.body = R"({"error":"Invalid JSON"})";
		res.set_header( "Content-Type", "application/json" );
		res.end();
		return;
	}

	// Require admin authentication
	int UserUID = CrowAuth::IsAuthenticated( *m_GlobalContext, req, &body,
		CrowAuth::Action::ReadWrite, CrowAuth::Privilege::Administrator );
	if( UserUID < 0 )
	{
		res.code = 401;
		res.body = R"({"error":"Admin authentication required"})";
		res.set_header( "Content-Type", "application/json" );
		res.end();
		return;
	}

	// Get optional cudnn_path from request body
	const char* cudnnPath = nullptr;
	std::string cudnnPathStr;
	if( body.has( "cudnn_path" ) )
	{
		cudnnPathStr = body["cudnn_path"].s();
		if( !cudnnPathStr.empty() )
			cudnnPath = cudnnPathStr.c_str();
	}

	LOG_INFO( "CUDA probe requested by admin (UserUID=%d)", UserUID );

	bool success = Witness::Camera::TestCudaViaProbe( cudnnPath );

	crow::json::wvalue result;
	result["success"] = success;
	result["message"] = success
		? "CUDA GPU acceleration is available and working."
		: "CUDA test failed. Check that CUDA Toolkit 12.x and cuDNN 9.x are installed correctly.";

	res.set_header( "Content-Type", "application/json" );
	res.body = result.dump();
	res.code = 200;
	res.end();
}

void CrowListener::HandleSettingsSet( const crow::request& req, crow::response& res )
{
	try
	{
	auto body = crow::json::load( req.body );
	if( !body )
	{
		res.code = 400;
		res.end();
		return;
	}

	int UserUID = CrowAuth::IsAuthenticated( *m_GlobalContext, req, &body,
		CrowAuth::Action::ReadWrite, CrowAuth::Privilege::Administrator );
	if( UserUID < 0 )
	{
		res.code = 401;
		res.end();
		return;
	}

	if( !body.has("name") || !body.has("value") )
	{
		res.code = 400;
		res.body = R"({"error":"Missing name or value"})";
		res.set_header( "Content-Type", "application/json" );
		res.end();
		return;
	}

	std::string name = body["name"].s();
	std::string value;

	// Handle both string and numeric values from JSON
	auto& valNode = body["value"];
	if( valNode.t() == crow::json::type::String )
		value = valNode.s();
	else if( valNode.t() == crow::json::type::Number )
		value = std::to_string( valNode.d() );
	else
		value = "";

	// Whitelist of settings that can be changed at runtime
	static const std::set<std::string> allowedSettings = {
		"continuous_recording_retention_days",
		"continuous_recording_quota_gb",
		"clip_cleanup_enabled",
		"clip_retention_days",
		"detection_backend",
		"detection_provider",
		"detection_model_path",
		"detection_confidence",
		"detection_max_fps",
		"cudnn_path",
		"mse_partial_duration",
		"face_detection_enabled",
		"face_detection_confidence",
		"face_detection_model_path",
		"face_recognition_enabled",
		"face_recognition_model_path",
		"face_recognition_confidence",
		"face_recognition_min_verified",
		"face_recognition_auto_assign",
	};

	if( allowedSettings.find( name ) == allowedSettings.end() )
	{
		res.code = 403;
		res.body = R"({"error":"Setting not modifiable"})";
		res.set_header( "Content-Type", "application/json" );
		res.end();
		return;
	}

	// Enforce minimum confidence thresholds
	if( name == "face_detection_confidence" )
	{
		double v = std::stod( value );
		if( v < 0.5 ) value = "0.5";
	}
	else if( name == "face_recognition_confidence" )
	{
		double v = std::stod( value );
		if( v < 0.4 ) value = "0.4";
	}
	else if( name == "detection_confidence" )
	{
		double v = std::stod( value );
		if( v < 0.1 ) value = "0.1";
	}
	else if( name == "face_recognition_min_verified" )
	{
		int v = std::stoi( value );
		if( v < 1 ) value = "1";
		if( v > 10 ) value = "10";
	}

	SQLiteDatabaseQueryInstance query( m_GlobalContext->Database, "SetSetting" );
	query->Bind( "@Name", name.c_str() );
	query->Bind( "@Value", value.c_str() );
	query->Execute( nullptr );

	LOG_INFO( "Setting updated by admin (UserUID=%d): %s = %s", UserUID, name.c_str(), value.c_str() );

	res.set_header( "Content-Type", "application/json" );
	res.body = R"({"ok":true})";
	res.code = 200;
	res.end();

	}
	catch( const std::exception& e )
	{
		LOG_ERROR( "HandleSettingsSet exception: %s", e.what() );
		res.code = 500;
		res.body = std::string(R"({"error":")") + e.what() + "\"}";
		res.set_header( "Content-Type", "application/json" );
		res.end();
	}
}
