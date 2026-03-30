#include "CrowListener.h"
#include "CrowAuth.h"
#include "GlobalContext.h"
#include "SoundManager.h"
#include "PlatformHelpers.h"

#include <Log.h>
#include <filesystem>

#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
#pragma comment(lib, "Winmm.lib")
#endif

namespace fs = std::filesystem;

// ===== Action Handlers =====

void CrowListener::HandleActionEnum( const crow::request& req, crow::response& res )
{
	int UserUID = CrowAuth::IsAuthenticated( *m_GlobalContext, req, nullptr,
		CrowAuth::Action::Read, CrowAuth::Privilege::Administrator );
	if( UserUID < 0 )
	{
		res.code = 400;
		res.end();
		return;
	}

	// Get all actions
	std::vector<crow::json::wvalue> actions;
	SQLiteDatabaseQueryInstance q( m_GlobalContext->Database, "SelectAllActions" );
	q->Execute( [&]( const SQLiteDatabaseQuery& query )
	{
		crow::json::wvalue a;
		a["id"] = query.GetColumnValueInt64( 0 );
		a["name"] = std::string( query.GetColumnValueText( 1 ) );
		a["command"] = std::string( query.GetColumnValueText( 2 ) );
		a["param1"] = std::string( query.GetColumnValueText( 3 ) );
		a["param2"] = std::string( query.GetColumnValueText( 4 ) );
		a["param3"] = std::string( query.GetColumnValueText( 5 ) );
		a["priority"] = query.GetColumnValueInt( 6 );
		a["cooldown"] = query.GetColumnValueInt( 7 );
		actions.push_back( std::move( a ) );
		return true;
	});

	// Get all camera-action assignments
	std::vector<crow::json::wvalue> assignments;
	SQLiteDatabaseQueryInstance q2( m_GlobalContext->Database, "SelectAllCameraActions" );
	q2->Execute( [&]( const SQLiteDatabaseQuery& query )
	{
		crow::json::wvalue ca;
		ca["id"] = query.GetColumnValueInt64( 0 );
		ca["actionId"] = query.GetColumnValueInt64( 1 );
		ca["cameraId"] = query.GetColumnValueInt( 2 );
		ca["mdThreshold"] = query.GetColumnValueDouble( 3 );
		const char* dc = query.GetColumnValueText( 4 );
		ca["detectionClass"] = std::string( dc ? dc : "" );
		assignments.push_back( std::move( ca ) );
		return true;
	});

	crow::json::wvalue data;
	data["actions"] = std::move( actions );
	data["assignments"] = std::move( assignments );

	res.set_header( "Content-Type", "application/json" );
	res.body = data.dump();
	res.code = 200;
	res.end();
}

void CrowListener::HandleActionCreate( const crow::request& req, crow::response& res )
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
		res.code = 400;
		res.end();
		return;
	}

	std::string name = body.has( "name" ) ? std::string( body["name"].s() ) : "";
	std::string command = body.has( "command" ) ? std::string( body["command"].s() ) : "PlaySound";
	std::string param1 = body.has( "param1" ) ? std::string( body["param1"].s() ) : "";
	std::string param2 = body.has( "param2" ) ? std::string( body["param2"].s() ) : "";
	std::string param3 = body.has( "param3" ) ? std::string( body["param3"].s() ) : "";
	int priority = body.has( "priority" ) ? (int)body["priority"].i() : 50;
	int cooldown = body.has( "cooldown" ) ? (int)body["cooldown"].i() : 30;

	SQLiteDatabaseQueryInstance q( m_GlobalContext->Database, "CreateAction" );
	q->Bind( "@Name", name.c_str() );
	q->Bind( "@Command", command.c_str() );
	q->Bind( "@Param1", param1.c_str() );
	q->Bind( "@Param2", param2.c_str() );
	q->Bind( "@Param3", param3.c_str() );
	q->Bind( "@Priority", priority );
	q->Bind( "@Cooldown", cooldown );
	q->Execute( nullptr );

	crow::json::wvalue data;
	data["id"] = q->GetLastInsertionId();

	res.set_header( "Content-Type", "application/json" );
	res.body = data.dump();
	res.code = 200;
	res.end();
}

void CrowListener::HandleActionUpdate( const crow::request& req, crow::response& res )
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
		res.code = 400;
		res.end();
		return;
	}

	int actionUID = body.has( "id" ) ? (int)body["id"].i() : 0;
	std::string name = body.has( "name" ) ? std::string( body["name"].s() ) : "";
	std::string command = body.has( "command" ) ? std::string( body["command"].s() ) : "PlaySound";
	std::string param1 = body.has( "param1" ) ? std::string( body["param1"].s() ) : "";
	std::string param2 = body.has( "param2" ) ? std::string( body["param2"].s() ) : "";
	std::string param3 = body.has( "param3" ) ? std::string( body["param3"].s() ) : "";
	int priority = body.has( "priority" ) ? (int)body["priority"].i() : 50;
	int cooldown = body.has( "cooldown" ) ? (int)body["cooldown"].i() : 30;

	SQLiteDatabaseQueryInstance q( m_GlobalContext->Database, "UpdateAction" );
	q->Bind( "@ActionUID", actionUID );
	q->Bind( "@Name", name.c_str() );
	q->Bind( "@Command", command.c_str() );
	q->Bind( "@Param1", param1.c_str() );
	q->Bind( "@Param2", param2.c_str() );
	q->Bind( "@Param3", param3.c_str() );
	q->Bind( "@Priority", priority );
	q->Bind( "@Cooldown", cooldown );
	q->Execute( nullptr );

	res.set_header( "Content-Type", "application/json" );
	res.body = "{}";
	res.code = 200;
	res.end();
}

void CrowListener::HandleActionDelete( const crow::request& req, crow::response& res )
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
		res.code = 400;
		res.end();
		return;
	}

	int actionUID = body.has( "id" ) ? (int)body["id"].i() : 0;

	// Delete camera-action assignments first
	SQLiteDatabaseQueryInstance q1( m_GlobalContext->Database, "DeleteCameraActionsForAction" );
	q1->Bind( "@ActionUID", actionUID );
	q1->Execute( nullptr );

	// Delete the action
	SQLiteDatabaseQueryInstance q2( m_GlobalContext->Database, "DeleteAction" );
	q2->Bind( "@ActionUID", actionUID );
	q2->Execute( nullptr );

	res.set_header( "Content-Type", "application/json" );
	res.body = "{}";
	res.code = 200;
	res.end();
}

void CrowListener::HandleActionAssign( const crow::request& req, crow::response& res )
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
		res.code = 400;
		res.end();
		return;
	}

	int actionUID = body.has( "actionId" ) ? (int)body["actionId"].i() : 0;
	int cameraUID = body.has( "cameraId" ) ? (int)body["cameraId"].i() : 0;
	double mdThreshold = body.has( "mdThreshold" ) ? body["mdThreshold"].d() : 0.05;
	std::string detectionClass = body.has( "detectionClass" ) ? std::string( body["detectionClass"].s() ) : "";

	SQLiteDatabaseQueryInstance q( m_GlobalContext->Database, "CreateCameraAction" );
	q->Bind( "@ActionUID", actionUID );
	q->Bind( "@CameraUID", cameraUID );
	q->Bind( "@MDThreshold", mdThreshold );
	q->Bind( "@DetectionClass", detectionClass.c_str() );
	q->Execute( nullptr );

	crow::json::wvalue data;
	data["id"] = q->GetLastInsertionId();

	res.set_header( "Content-Type", "application/json" );
	res.body = data.dump();
	res.code = 200;
	res.end();
}

void CrowListener::HandleActionUnassign( const crow::request& req, crow::response& res )
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
		res.code = 400;
		res.end();
		return;
	}

	int caUID = body.has( "id" ) ? (int)body["id"].i() : 0;

	SQLiteDatabaseQueryInstance q( m_GlobalContext->Database, "DeleteCameraAction" );
	q->Bind( "@CameraActionUID", caUID );
	q->Execute( nullptr );

	res.set_header( "Content-Type", "application/json" );
	res.body = "{}";
	res.code = 200;
	res.end();
}

void CrowListener::HandleActionSounds( const crow::request& req, crow::response& res )
{
	int UserUID = CrowAuth::IsAuthenticated( *m_GlobalContext, req, nullptr,
		CrowAuth::Action::Read, CrowAuth::Privilege::Administrator );
	if( UserUID < 0 )
	{
		res.code = 400;
		res.end();
		return;
	}

	// List available notification sounds relative to the exe directory
	std::vector<crow::json::wvalue> sounds;

	auto soundsDir = Witness::GetExeDir() / "NotificationSounds";

	if( fs::exists( soundsDir ) && fs::is_directory( soundsDir ) )
	{
		for( auto& entry : fs::directory_iterator( soundsDir ) )
		{
			if( entry.is_regular_file() && entry.path().extension() == ".wav" )
			{
				crow::json::wvalue s;
				s["file"] = "NotificationSounds/" + entry.path().filename().string();
				s["name"] = entry.path().stem().string();
				sounds.push_back( std::move( s ) );
			}
		}
	}

	crow::json::wvalue data;
	data["sounds"] = std::move( sounds );

	res.set_header( "Content-Type", "application/json" );
	res.body = data.dump();
	res.code = 200;
	res.end();
}

void CrowListener::HandleActionTestSound( const crow::request& req, crow::response& res )
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
		res.code = 400;
		res.end();
		return;
	}

	std::string soundFile = body.has( "file" ) ? std::string( body["file"].s() ) : "";
	if( soundFile.empty() )
	{
		res.code = 400;
		res.body = R"({"error":"No file specified"})";
		res.end();
		return;
	}

	// Resolve relative path
	fs::path soundPath( soundFile );
	if( soundPath.is_relative() )
	{
		soundPath = Witness::GetExeDir() / soundPath;
	}

	// Validate path exists
	if( !fs::exists( soundPath ) )
	{
		res.code = 404;
		res.body = R"({"error":"Sound file not found"})";
		res.set_header( "Content-Type", "application/json" );
		res.end();
		return;
	}

#ifdef _WIN32
	std::string resolved = soundPath.string();
	PlaySoundA( resolved.c_str(), nullptr, SND_FILENAME | SND_ASYNC );
#endif

	res.set_header( "Content-Type", "application/json" );
	res.body = "{}";
	res.code = 200;
	res.end();
}
