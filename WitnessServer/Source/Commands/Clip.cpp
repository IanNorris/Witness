#include "Clip.h"
#include "Authenticate.h"

#include "cpprest/json.h"
#include "cpprest/http_client.h"
#include "cpprest/uri.h"
#include "cpprest/asyncrt_utils.h"
#include "cpprest/filestream.h"
#include "sodium.h"

#include <iostream>
#include <chrono>
#include <iostream>
#include <experimental/filesystem>

using namespace web::json;
using namespace web::http::client;

namespace fs = std::experimental::filesystem;

const static int MaxClipsPerQuery = 100;

string_t GetClipName( const GlobalContext& Context, int CameraID, int64_t Timestamp, bool Manual, bool Video)
{
	stringstream_t Stream;
	Stream << Context.CachePath << _T("\\") << CameraID;

	if( Video )
	{
		if (Manual)
		{
			Stream << _T("_Manual");
		}
		else
		{
			Stream << _T("_Auto");
		}
	}

	Stream << _T("_") << Timestamp;

	if (Video)
	{
		Stream << _T(".mp4");
	}
	else
	{
		Stream << _T(".jpg");
	}

	return Stream.str();
}

void Command_Clip::OnMessage( const GlobalContext& Context, http_request& Message, const string_t& CurrentCommand, vector<string_t>& ChildPath, bool IsPost )
{
	auto Packet = Message.extract_json().get();

	if( ChildPath.size() == 3 && !IsPost )
	{
		auto Command = ChildPath.front();
		if( Command.compare( _T("thumb") ) == 0 )
		{
			OnThumbnailMessage( Context, Message, false, ChildPath[1], ChildPath[2], Packet );
		}
		else if( Command.compare( _T("video") ) == 0 )
		{
			OnThumbnailMessage( Context, Message, true, ChildPath[1], ChildPath[2], Packet );
		}
		else
		{
			Message.reply( status_codes::NotFound );
		}

		return;
	}
	if( ChildPath.size() == 6 && !IsPost )
	{
		auto Command = ChildPath.front();
		if( Command.compare( _T("enum") ) == 0 )
		{
			OnEnumClipsMessage( Context, Message, /*TargetCamera*/ ChildPath[1], /*MaxCount*/ ChildPath[2], /*StartDate*/ ChildPath[3], /*RangePeriod*/ ChildPath[4], /*Page*/ ChildPath[5], Packet );
		}
		else
		{
			Message.reply( status_codes::NotFound );
		}

		return;
	}
	else
	{
		Message.reply( status_codes::NotFound );
	}
}

void Command_Clip::OnThumbnailMessage( const GlobalContext& Context, http_request& Message, bool Video, const string_t& TargetCamera, const string_t& TargetClip, const json::value& Packet )
{
	//NO CSRF!

	//TODO: Check user has access to camera

	int TargetCameraInt = _wtoi( TargetCamera.c_str() );
	uint64_t TargetCameraTimestamp = _wtoll( TargetClip.c_str() );

	/*if( !Command_Authenticate::IsAuthenticated( Context, Message, Packet, false ) )
	{
		return;
	}*/

	if( !Video )
	{
		lock_guard<mutex> Lock( Context.Mutex );

		auto IterCamera = Context.Cameras.find( TargetCameraInt );
		if( IterCamera != Context.Cameras.end() )
		{
			const auto& Camera = (*IterCamera).second.ClipThumbnails;
			auto IterClip = Camera.find( TargetCameraTimestamp );
			if( IterClip != Camera.end() && (*IterClip).second.size() != 0 )
			{
				http_response Response;
				Response.set_status_code( status_codes::OK );
				Response.set_body( (*IterClip).second );
				Response.headers().set_content_type( _T("image/jpeg") );
				Response.headers().set_cache_control( _T("no-cache, no-store, must-revalidate") );

				Message.reply( Response );
				return;
			}
		}
	}

	SQLiteDatabaseQueryInstance SelectClip( Context.Database, _T("SelectClip") );
	SelectClip->Bind( "@CameraID", TargetCameraInt );
	SelectClip->Bind( "@Timestamp", (int64_t)TargetCameraTimestamp );

	string_t ClipFilename;

	bool Success = false;

	SelectClip->Execute( 
		[&]( const SQLiteDatabaseQuery& query )
		{
			uint64_t Timestamp = query.GetColumnValueInt64(0);
			int CameraID = query.GetColumnValueInt(1);
			int RecordMode = query.GetColumnValueInt(5);

			ClipFilename = GetClipName( Context, CameraID, Timestamp, RecordMode == 0, Video );
			
			Success = true;

			return true;
		} 
	);

	if( Success && fs::exists( ClipFilename ) )
	{


		size64_t FileSize = fs::file_size( ClipFilename );

		auto FileHandle = concurrency::streams::file_stream<uint8_t>::open_istream(ClipFilename.c_str());

		Concurrency::streams::istream& FileHandleStream = FileHandle.get(); 

		//Matching file
		Message.reply( status_codes::OK, FileHandleStream, FileSize, Video ? _T("video/mp4") : _T("image/jpeg") );
		return;
	}

	Message.reply( status_codes::NotFound );
}

void Command_Clip::OnEnumClipsMessage( const GlobalContext& Context, http_request& Message, const string_t& TargetCamera, const string_t& MaxCount, const string_t& StartDate, const string_t& RangePeriod, const string_t& Page, const json::value& Packet )
{
	//NO CSRF!

	//TODO: Check user has access to camera

	int TargetCameraInt = _wtoi( TargetCamera.c_str() );
	int MaxCountInt = _wtoi( MaxCount.c_str() );
	uint64_t StartDateInt = _wtoll( StartDate.c_str() );
	uint64_t RangePeriodInt = _wtoll( RangePeriod.c_str() );
	int PageInt = _wtoi( Page.c_str() );

	MaxCountInt = min( MaxCountInt, MaxClipsPerQuery );

	if( !Command_Authenticate::IsAuthenticated( Context, Message, Packet, false ) )
	{
		return;
	}

	int Count = 0;
	json::value Data;
	vector<json::value> Array;

	{
		SQLiteDatabaseQueryInstance CountClipsWithinRange( Context.Database, _T("CountClipsWithinRange") );
		CountClipsWithinRange->Bind( "@CameraID", TargetCameraInt );
		CountClipsWithinRange->Bind( "@TimestampFrom", (int64_t)(StartDateInt - RangePeriodInt) );
		CountClipsWithinRange->Bind( "@TimestampTo", (int64_t)StartDateInt );
		CountClipsWithinRange->Bind( "@MaxCount", MaxCountInt );
		CountClipsWithinRange->Bind( "@PageOffset", MaxCountInt * PageInt );

		CountClipsWithinRange->Execute( 
			[&Count]( const SQLiteDatabaseQuery& query )
			{
				Count = query.GetColumnValueInt(0);
				return true;
			}
		);
	}
	
	if( Count > 0 )
	{
		SQLiteDatabaseQueryInstance SelectClipsWithinRange( Context.Database, _T("SelectClipsWithinRange") );
		SelectClipsWithinRange->Bind( "@CameraID", TargetCameraInt );
		SelectClipsWithinRange->Bind( "@TimestampFrom", (int64_t)(StartDateInt - RangePeriodInt) );
		SelectClipsWithinRange->Bind( "@TimestampTo", (int64_t)StartDateInt );
		SelectClipsWithinRange->Bind( "@MaxCount", MaxCountInt );
		SelectClipsWithinRange->Bind( "@PageOffset", MaxCountInt * PageInt );

		SelectClipsWithinRange->Execute( 
			[&Array, &Context]( const SQLiteDatabaseQuery& query )
			{
				uint64_t Timestamp = query.GetColumnValueInt64(0);
				int CameraID = query.GetColumnValueInt(1);
				uint64_t MotionTimestamp = query.GetColumnValueInt64(2);
				int ActiveDuration = query.GetColumnValueInt(3);
				int Duration = query.GetColumnValueInt(4);
				int RecordMode = query.GetColumnValueInt(5);
				double MaxMotion = query.GetColumnValueDouble(6);
				string_t Description = query.GetColumnValueText(7);
			
				json::value Camera;
				Camera[ _T("timestamp") ] = json::value(Timestamp);
				Camera[ _T("cameraID") ] = json::value(CameraID);
				Camera[ _T("motionTimestamp") ] = json::value(MotionTimestamp);
				Camera[ _T("activeDuration") ] = json::value(ActiveDuration);
				Camera[ _T("duration") ] = json::value(Duration);
				Camera[ _T("recordMode") ] = json::value(RecordMode);
				Camera[ _T("maxMotion") ] = json::value(MaxMotion);
				Camera[ _T("description") ] = json::value(Description);

				Array.push_back( Camera );
				
				return true;
			} 
		);
	}

	Data[ _T("count") ] = json::value(Count);
	Data[ _T("clips") ] = json::value::array(Array);

	Message.reply( status_codes::OK, Data );
}