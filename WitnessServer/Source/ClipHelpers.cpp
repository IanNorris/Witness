#include "ClipHelpers.h"
#include "GlobalContext.h"

#include <iostream>
#include <filesystem>
#include <chrono>

#ifdef _WIN32
#include <winerror.h>
#endif

namespace fs = std::filesystem;

StringT GetClipName( const GlobalContext& Context, int CameraID, int64_t Timestamp, bool Manual, bool Video )
{
	StringStreamT Stream;
	Stream << CameraID;

	if( Video )
	{
		if( Manual )
		{
			Stream << _T("_Manual");
		}
		else
		{
			Stream << _T("_Auto");
		}
	}

	Stream << _T("_") << Timestamp;

	if( Video )
	{
		Stream << _T(".mp4");
	}
	else
	{
		Stream << _T(".jpg");
	}

	return (fs::path(Context.CachePath) / Stream.str()).string();
}

static bool DeleteClipFiles( const GlobalContext& Context, int CameraID, int64_t Timestamp, bool Manual )
{
	auto ThumbnailPath = GetClipName( Context, CameraID, Timestamp, Manual, false );
	auto VideoPath = GetClipName( Context, CameraID, Timestamp, Manual, true );

	std::error_code error;
	if( !std::filesystem::remove( ThumbnailPath, error ) )
	{
		if( error.value() == E_ACCESSDENIED )
		{
			return false;
		}
	}

	if( !std::filesystem::remove( VideoPath, error ) )
	{
		if( error.value() == E_ACCESSDENIED )
		{
			return false;
		}
	}

	return true;
}

struct ClipToDelete
{
	int64_t ClipID;
	int CameraID;
	int64_t Timestamp;
	bool Manual;
};

void DeleteOldClips( const GlobalContext& Context, int DaysToDelete )
{
	const static int SecondsInDay = 60 * 60 * 24;
	int64_t Timestamp = static_cast<int64_t>(GetUnixTimestamp()) - (DaysToDelete * SecondsInDay);

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
		std::cout << _T("Deleting ") << ClipsToDelete.size() << _T(" clips as they were more than ") << DaysToDelete << _T(" days old.") << std::endl;
	}

	for( auto& Clip : ClipsToDelete )
	{
		SQLiteDatabaseQueryInstance DeleteClipQuery( Context.Database, _T("DeleteClip") );
		DeleteClipQuery->Bind( "@ClipUID", Clip.ClipID );

		if( DeleteClipFiles( Context, Clip.CameraID, Clip.Timestamp, Clip.Manual ) )
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
