#include "Stream.h"
#include "Authenticate.h"

#include "cpprest/json.h"
#include "cpprest/http_client.h"
#include "cpprest/uri.h"
#include "cpprest/asyncrt_utils.h"
#include "cpprest/filestream.h"
#include "sodium.h"

#include <iostream>
#include <iomanip>
#include <chrono>
#include <ctime>
#include <iostream>
#include <experimental/filesystem>

#ifdef _WIN32
#include <winerror.h>
#endif

using namespace web::json;
using namespace web::http::client;

namespace fs = std::experimental::filesystem;


void Command_Stream::OnSegmentMessage(const GlobalContext& Context, http_request& Message, const string_t& TargetCamera, const string_t& TargetSegment, const json::value& Packet)
{
	int TargetCameraInt = _wtoi(TargetCamera.c_str());
	int TargetSegmentInt = _wtoi(TargetSegment.c_str());

	if (!Command_Authenticate::IsCameraAuthenticated(Context, Message, Packet, Command_Authenticate::Action::Read, Command_Authenticate::Privilege::Normal, TargetCameraInt))
	{
		return;
	}

	stringstream_t StreamPathBuf;
	StreamPathBuf << Context.CachePath << "\\Live_" << TargetCameraInt << "_" << TargetSegmentInt << ".mp4";

	auto StreamPath = StreamPathBuf.str();

	if( fs::exists(StreamPath) )
	{
		size64_t FileSize = fs::file_size(StreamPath);

		auto FileHandle = concurrency::streams::file_stream<uint8_t>::open_istream(StreamPath.c_str());

		Concurrency::streams::istream& FileHandleStream = FileHandle.get(); 
		Message.reply( status_codes::OK, FileHandleStream, FileSize, _T("video/mp4") );
		return;
	}

	Message.reply( status_codes::NotFound );
}

void Command_Stream::OnPlaylistMessage(const GlobalContext& Context, http_request& Message, const string_t& TargetCamera, const json::value& Packet)
{
	int TargetCameraInt = _wtoi(TargetCamera.c_str());
	
	if (!Command_Authenticate::IsCameraAuthenticated(Context, Message, Packet, Command_Authenticate::Action::Read, Command_Authenticate::Privilege::Normal, TargetCameraInt))
	{
		return;
	}

	//TODO: Use a SRW here. Write only when updating the array.

	auto CameraState = Context.FindCameraById(TargetCameraInt);

	if (!CameraState)
	{
		Message.reply(status_codes::NotFound);
		return;
	}

	shared_ptr<LiveOutputStream>& LiveStream = CameraState->Worker->GetLiveStream();

	std::vector<LiveStreamSegment> Segments;
	LiveStream->GetSegments(Segments);

	int CurrentSegment = LiveStream->GetCurrentSegment();

	size_t bufferSegments = Segments.size();
	
	stringstream_t Playlist;
	Playlist << "#EXTM3U" << endl;
	Playlist << "#EXT-X-VERSION:3" << endl;
	//Playlist << "#EXT-X-I-FRAMES-ONLY" << endl;
	//Playlist << "#EXT-X-ALLOW-CACHE:YES" << endl;

	if (bufferSegments > 0)
	{
		bufferSegments--;
		
		const size_t extraBufferSegments = 0;

		double MaxLength = 0.0;
		for (int segment = 0; segment < bufferSegments; segment++)
		{
			LiveStreamSegment& Segment = Segments[segment];

			double NewDuration = Segment.Stream->GetClipLength();
			if (NewDuration > MaxLength)
			{
				MaxLength = NewDuration;
			}
		}

		double HoldbackLength = 1.0 * MaxLength;
		//Playlist << "#EXT-X-SERVER-CONTROL:PART-HOLD-BACK=" << HoldbackLength << endl;

		

		Playlist << "#EXT-X-MEDIA-SEQUENCE:" << Segments[0].Stream->GetSegmentIndex() << endl;
		Playlist << "#EXT-X-TARGETDURATION:" << MaxLength + 1 << endl;
		Playlist << "#EXT-X-INDEPENDENT-SEGMENTS" << endl;
		Playlist << "" << endl;

		Playlist.precision(4);

		for (int segment = 0; segment < bufferSegments; segment++)
		{
			LiveStreamSegment& Segment = Segments[segment];

			auto Length = Segment.Stream->GetClipLength();
			Playlist << "#EXINF:" << Length << "," << endl;
			Playlist << "#EXT-X-PROGRAM-DATE-TIME:" << std::put_time(&Segment.StreamStartTime, L"%FT%T") << endl;

			Playlist << "/stream/segment/" << TargetCameraInt << "/" << Segment.Stream->GetSegmentIndex() << ".mp4" << endl;
		}
	}

	http_response Response;
	Response.set_status_code(status_codes::OK);
	Response.set_body(Playlist.str());
	Response.headers().set_content_type(_T("application/x-mpegURL"));
	Response.headers().set_cache_control(_T("no-cache, no-store, must-revalidate"));

	Message.reply(Response);
}

void Command_Stream::OnMessage( GlobalContext& Context, http_request& Message, const string_t& CurrentCommand, vector<string_t>& ChildPath, bool IsPost )
{
	auto Packet = Message.extract_json().get();

	if (ChildPath.size() == 3 && !IsPost)
	{
		auto Command = ChildPath.front();
		if( Command.compare( _T("pl") ) == 0 )
		{
			OnPlaylistMessage( Context, Message, ChildPath[1], Packet );
		}
		else if (Command.compare(_T("segment")) == 0)
		{
			OnSegmentMessage(Context, Message, ChildPath[1], ChildPath[2], Packet);
		}
		else
		{
			Message.reply( status_codes::NotFound );
		}

		return;
	}

	Message.reply( status_codes::NotFound );
}
