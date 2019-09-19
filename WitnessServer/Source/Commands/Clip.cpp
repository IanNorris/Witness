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

#ifdef _WIN32
#include <winerror.h>
#endif

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

bool DeleteClip( const GlobalContext& Context, int CameraID, int64_t Timestamp, bool Manual )
{
	auto ThumbnailPath = GetClipName( Context, CameraID, Timestamp, Manual, false );
	auto VideoPath = GetClipName( Context, CameraID, Timestamp, Manual, true );
	
	std::error_code error;
	if( !std::experimental::filesystem::remove( ThumbnailPath, error) )
	{
		if( error.value() == E_ACCESSDENIED )
		{
			return false;
		}
	}

	if( !std::experimental::filesystem::remove( VideoPath, error ) )
	{
		if( error.value() == E_ACCESSDENIED )
		{
			return false;
		}
	}

	return true;
}

void Command_Clip::OnMessage( GlobalContext& Context, http_request& Message, const string_t& CurrentCommand, vector<string_t>& ChildPath, bool IsPost )
{
	auto Packet = Message.extract_json().get();

	if( ChildPath.size() == 1 && IsPost )
	{
		auto Command = ChildPath.front();
		if( Command.compare( _T("toggleSave") ) == 0 )
		{
			OnToggleSaveMessage( Context, Message, Packet );
		}
		else if( Command.compare( _T("delete") ) == 0 )
		{
			OnDeleteMessage( Context, Message, Packet );
		}
		else
		{
			Message.reply( status_codes::NotFound );
		}
	}

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
			OnEnumClipsMessage( Context, Message, /*TargetCamera*/ ChildPath[1], /*MaxCount*/ ChildPath[2], /*StartDate*/ ChildPath[3], /*RangePeriod*/ ChildPath[4], /*PageOffset*/ ChildPath[5], Packet );
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
	int TargetCameraInt = _wtoi( TargetCamera.c_str() );
	uint64_t TargetCameraTimestamp = _wtoll( TargetClip.c_str() );

	if( !Command_Authenticate::IsCameraAuthenticated( Context, Message, Packet, Command_Authenticate::Action::Read, Command_Authenticate::Privilege::Normal, TargetCameraInt ) )
	{
		return;
	}

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
			uint64_t ClipID = query.GetColumnValueInt64(0);
			uint64_t Timestamp = query.GetColumnValueInt64(1);
			int CameraID = query.GetColumnValueInt(2);
			int RecordMode = query.GetColumnValueInt(6);

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

void Command_Clip::OnEnumClipsMessage( const GlobalContext& Context, http_request& Message, const string_t& TargetCamera, const string_t& MaxCount, const string_t& StartDate, const string_t& RangePeriod, const string_t& PageOffset, const json::value& Packet )
{
	int TargetCameraInt = _wtoi( TargetCamera.c_str() );
	int MaxCountInt = _wtoi( MaxCount.c_str() );
	uint64_t StartDateInt = _wtoll( StartDate.c_str() );
	uint64_t RangePeriodInt = _wtoll( RangePeriod.c_str() );
	int PageOffsetInt = _wtoi( PageOffset.c_str() );

	MaxCountInt = min( MaxCountInt, MaxClipsPerQuery );

	int UserUID = 0;
	if( TargetCameraInt == -1 )
	{
		UserUID = Command_Authenticate::IsAuthenticated( Context, Message, Packet, Command_Authenticate::Action::Read, Command_Authenticate::Privilege::Normal );
		if( UserUID < 0 )
		{
			return;
		}
	}
	else
	{
		UserUID = Command_Authenticate::IsCameraAuthenticated( Context, Message, Packet, Command_Authenticate::Action::Read, Command_Authenticate::Privilege::Normal, TargetCameraInt );
		if( UserUID < 0 )
		{
			return;
		}
	}

	

	int Count = 0;
	json::value Data;
	vector<json::value> Array;

	{
		SQLiteDatabaseQueryInstance CountClipsWithinRange( Context.Database, TargetCameraInt == -1 ? _T("CountClipsWithinRangeAll") : _T("CountClipsWithinRange") );
		if( TargetCameraInt == -1)
		{
			CountClipsWithinRange->Bind( "@UserUID", UserUID );
		}
		else
		{
			CountClipsWithinRange->Bind( "@CameraID", TargetCameraInt );
		}
		CountClipsWithinRange->Bind( "@TimestampFrom", (int64_t)(StartDateInt - RangePeriodInt) );
		CountClipsWithinRange->Bind( "@TimestampTo", (int64_t)StartDateInt );
		CountClipsWithinRange->Bind( "@MaxCount", MaxCountInt );
		CountClipsWithinRange->Bind( "@PageOffset", PageOffsetInt );

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
		SQLiteDatabaseQueryInstance SelectClipsWithinRange( Context.Database, TargetCameraInt == -1 ? _T("SelectClipsWithinRangeAll") : _T("SelectClipsWithinRange") );
		if( TargetCameraInt == -1)
		{
			SelectClipsWithinRange->Bind( "@UserUID", UserUID );
		}
		else
		{
			SelectClipsWithinRange->Bind( "@CameraID", TargetCameraInt );
		}
		SelectClipsWithinRange->Bind( "@TimestampFrom", (int64_t)(StartDateInt - RangePeriodInt) );
		SelectClipsWithinRange->Bind( "@TimestampTo", (int64_t)StartDateInt );
		SelectClipsWithinRange->Bind( "@MaxCount", MaxCountInt );
		SelectClipsWithinRange->Bind( "@PageOffset", PageOffsetInt );

		SelectClipsWithinRange->Execute( 
			[&Array, &Context]( const SQLiteDatabaseQuery& query )
			{
				uint64_t ClipID = query.GetColumnValueInt64(0);
				uint64_t Timestamp = query.GetColumnValueInt64(1);
				int CameraID = query.GetColumnValueInt(2);
				uint64_t MotionTimestamp = query.GetColumnValueInt64(3);
				int ActiveDuration = query.GetColumnValueInt(4);
				int Duration = query.GetColumnValueInt(5);
				int RecordMode = query.GetColumnValueInt(6);
				double MaxMotion = query.GetColumnValueDouble(7);

				const wchar_t* DescriptionStr = query.GetColumnValueText(8);
				string_t Description = DescriptionStr ? DescriptionStr : _T("");

				int Saved = query.GetColumnValueInt(9);

				string_t Tags = query.GetColumnValueText(10) ? query.GetColumnValueText(10) : _T("");
			
				json::value Camera;
				Camera[ _T("clipUID") ] = json::value(ClipID);
				Camera[ _T("timestamp") ] = json::value(Timestamp);
				Camera[ _T("cameraID") ] = json::value(CameraID);
				Camera[ _T("motionTimestamp") ] = json::value(MotionTimestamp);
				Camera[ _T("activeDuration") ] = json::value(ActiveDuration);
				Camera[ _T("duration") ] = json::value(Duration);
				Camera[ _T("recordMode") ] = json::value(RecordMode);
				Camera[ _T("maxMotion") ] = json::value(MaxMotion);
				Camera[ _T("description") ] = json::value(Description);
				Camera[ _T("saved") ] = json::value(Saved);
				Camera[ _T("tags") ] = json::value(Tags);

				Array.push_back( Camera );
				
				return true;
			} 
		);
	}

	Data[ _T("count") ] = json::value(Count);
	Data[ _T("clips") ] = json::value::array(Array);

	Message.reply( status_codes::OK, Data );
}

void Command_Clip::OnToggleSaveMessage( const GlobalContext& Context, http_request& Message, const json::value& Packet )
{
	string_t Errors;
	int ClipUID = 0;
	bool Value = false;

	bool Success = GetJsonField( Packet, _T("id"), ClipUID, Errors );
	Success &= GetJsonField( Packet, _T("value"), Value, Errors );

	if( !Success )
	{
		Message.reply( status_codes::BadRequest, Errors );
		return;
	}

	int TargetCameraInt = 0;
	{
		SQLiteDatabaseQueryInstance SelectClipID( Context.Database, _T("SelectClipID") );
		SelectClipID->Bind( "@ClipUID", ClipUID );

		bool Success = false;
		SelectClipID->Execute( 
			[&]( const SQLiteDatabaseQuery& query )
			{
				TargetCameraInt = query.GetColumnValueInt(2);
				Success = true;
				return true;
			}
		);
	}

	if( !Command_Authenticate::IsCameraAuthenticated( Context, Message, Packet, Command_Authenticate::Action::ReadWrite, Command_Authenticate::Privilege::Normal, TargetCameraInt ) )
	{
		return;
	}

	SQLiteDatabaseQueryInstance SetClipSaveState( Context.Database, _T("SetClipSaveState") );
	SetClipSaveState->Bind( "@ClipUID", ClipUID );
	SetClipSaveState->Bind( "@Save", Value ? 1 : 0 );

	SetClipSaveState->Execute( 
		[&]( const SQLiteDatabaseQuery& query )
		{
			return true;
		}
	);

	json::value Data;
	Message.reply( status_codes::OK, Data );
}

void Command_Clip::OnDeleteMessage( const GlobalContext& Context, http_request& Message, const json::value& Packet )
{
	string_t Errors;
	int ClipUID = 0;

	bool Success = GetJsonField( Packet, _T("id"), ClipUID, Errors );

	if( !Success )
	{
		Message.reply( status_codes::BadRequest, Errors );
		return;
	}

	int TargetCameraInt = 0;
	{
		SQLiteDatabaseQueryInstance SelectClipID( Context.Database, _T("SelectClipID") );
		SelectClipID->Bind( "@ClipUID", ClipUID );

		bool Success = false;
		SelectClipID->Execute( 
			[&]( const SQLiteDatabaseQuery& query )
			{
				TargetCameraInt = query.GetColumnValueInt(2);
				Success = true;
				return true;
			}
		);
	}

	if( !Command_Authenticate::IsCameraAuthenticated( Context, Message, Packet, Command_Authenticate::Action::ReadWrite, Command_Authenticate::Privilege::Normal, TargetCameraInt ) )
	{
		return;
	}

	SQLiteDatabaseQueryInstance FindClipByUID( Context.Database, _T("FindClipByUID") );
	FindClipByUID->Bind( "@ClipUID", ClipUID );

	int CameraID;
	int64_t Timestamp;
	bool Manual;
	
	FindClipByUID->Execute( 
		[&]( const SQLiteDatabaseQuery& query )
		{
			uint64_t ClipID = query.GetColumnValueInt64(0);
			Timestamp = query.GetColumnValueInt64(1);
			CameraID = query.GetColumnValueInt(2);
			Manual = query.GetColumnValueInt(6) == 0;
			
			return true;
		}
	);

	if( !DeleteClip( Context, CameraID, Timestamp, Manual ) )
	{
		Message.reply( status_codes::NotFound );
		return;
	}

	SQLiteDatabaseQueryInstance DeleteClipQuery( Context.Database, _T("DeleteClip") );
	DeleteClipQuery->Bind( "@ClipUID", ClipUID );

	DeleteClipQuery->Execute( 
		[&]( const SQLiteDatabaseQuery& query )
		{
			return true;
		}
	);

	json::value Data;
	Message.reply( status_codes::OK, Data );
}

struct ClipToDelete
{
	int64_t ClipID;
	int CameraID;
	int64_t Timestamp;
	bool Manual;
};

void Command_Clip::DeleteOldClips( const GlobalContext& Context, int DaysToDelete )
{
	const static int SecondsInDay = 60 * 60 * 24;
	int64_t Timestamp = datetime::utc_timestamp() - (DaysToDelete * SecondsInDay);

	SQLiteDatabaseQueryInstance SelectClipsToDelete( Context.Database, _T("SelectClipsToDelete") );
	SelectClipsToDelete->Bind( "@Timestamp", Timestamp );

	std::vector<ClipToDelete> ClipsToDelete;

	SelectClipsToDelete->Execute( 
		[&]( const SQLiteDatabaseQuery& query )
		{
			uint64_t ClipID = query.GetColumnValueInt64(0);
			int64_t Timestamp = query.GetColumnValueInt64(1);
			int CameraID = query.GetColumnValueInt(2);
			bool Manual = query.GetColumnValueInt(6) == 0;

			ClipToDelete Clip;
			Clip.ClipID = ClipID;
			Clip.CameraID = CameraID;
			Clip.Manual = Manual;
			Clip.Timestamp = Timestamp;

			ClipsToDelete.push_back(Clip);
			
			return true;
		}
	);

	if( ClipsToDelete.size() )
	{
		tcout << _T("Deleting ") << ClipsToDelete.size() << _T(" clips as they were more than ") << DaysToDelete << _T(" days old.") << endl;
	}

	for( auto& Clip : ClipsToDelete )
	{
		SQLiteDatabaseQueryInstance DeleteClipQuery( Context.Database, _T("DeleteClip") );
		DeleteClipQuery->Bind( "@ClipUID", Clip.ClipID );

		if( DeleteClip( Context, Clip.CameraID, Clip.Timestamp, Clip.Manual ) )
		{
			DeleteClipQuery->Execute( 
			[&]( const SQLiteDatabaseQuery& query )
			{
				return true;
			}
		);
		}
	}
}