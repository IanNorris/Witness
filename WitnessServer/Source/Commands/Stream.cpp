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
#include <filesystem>
#include <format>

#ifdef _WIN32
#include <winerror.h>
#endif

using namespace web::json;
using namespace web::http::client;

namespace fs = std::filesystem;


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

		Concurrency::streams::istream FileHandleStream = FileHandle.get(); 

		http_response response(status_codes::OK);

		//For testing with hls-demo
//#if _DEBUG
		response.headers().add(U("Access-Control-Allow-Origin"), U("*"));
//#endif

		response.set_body(FileHandleStream, FileSize, _T("video/mp4"));
		Message.reply(response);

		return;
	}

	Message.reply( status_codes::NotFound );
}

void Command_Stream::OnPlaylistMessage(const GlobalContext& Context, http_request& Message, const string_t& TargetCamera, const json::value& Packet)
{
	auto queryMap = web::uri::split_query(Message.request_uri().query());
	auto msnIter = queryMap.find(_T("_HLS_msn"));

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

	std::shared_ptr<LiveOutputStream>& LiveStream = CameraState->Worker->GetLiveStream();

	std::vector<LiveStreamSegment> Segments;
	LiveStream->GetSegments(Segments);

	int CurrentSegment = LiveStream->GetCurrentSegment();

	size_t bufferSegments = Segments.size();
	
	stringstream_t Playlist;
	Playlist << "#EXTM3U" << "\n";
	Playlist << "#EXT-X-VERSION:6" << "\n";
	//Playlist << "#EXT-X-I-FRAMES-ONLY" << endl;
	//Playlist << "#EXT-X-ALLOW-CACHE:YES" << endl;

	uint64_t msnStart = msnIter != queryMap.end() ? _wtoi64((*msnIter).second.c_str()) : 0;

	int startAtSegment = 0;
	if (bufferSegments > 0)
	{
		bufferSegments--;
		
		const size_t extraBufferSegments = 5;

		double MaxLength = 0.0;
		for (int segment = 0; segment < bufferSegments; segment++)
		{
			LiveStreamSegment& Segment = Segments[segment];

			if (Segment.Stream)
			{
				double NewDuration = Segment.Stream->GetClipLength();
				if (NewDuration > MaxLength)
				{
					MaxLength = NewDuration;
				}

				if (Segment.Stream->GetSegmentIndex() == msnStart)
				{
					MaxLength = NewDuration;
					startAtSegment = segment;
				}
			}
		}

		double HoldbackLength = 0.125;
		Playlist << "#EXT-X-SERVER-CONTROL:CAN-BLOCK-RELOAD=NO,PART-HOLD-BACK=" << HoldbackLength << "\n";
		Playlist << "#EXT-X-PART-INF:PART-TARGET=" << HoldbackLength << "\n";

		//
		

		Playlist << "#EXT-X-MEDIA-SEQUENCE:" << Segments[startAtSegment].Stream->GetSegmentIndex() << std::endl;
		Playlist << "#EXT-X-TARGETDURATION:" << MaxLength << std::endl;
		//Playlist << "#EXT-X-MAP:URI=\"/stream/segment/" << TargetCameraInt << "/" << Segments[0].Stream->GetSegmentIndex() << ".mp4\"" << "\n";
		//Playlist << "#EXT-X-MAP:URI=\"" << "/stream/segment/" << TargetCameraInt << "/Init.mp4\"\n";
		Playlist << "" << "\n";

		Playlist.precision(4);

		double LastTime = 0;

		int lastIndex = 0;

		for (int segment = startAtSegment; segment < bufferSegments; segment++)
		{
			LiveStreamSegment& Segment = Segments[segment];

			if (Segment.Stream)
			{
				std::string dateTimeFormat = std::format("{:%Y-%m-%dT%H:%M:%S}", Segment.SegmentTime);

				Playlist << "#EXT-X-PROGRAM-DATE-TIME:" << string_t(dateTimeFormat.begin(), dateTimeFormat.end()) << "\n";
			}
			
			for (auto Part : Segment.PartialStreams)
			{
				if (Part->GetClipLength() > 0)
				{
					Playlist << "#EXT-X-PART:DURATION=" << Part->GetClipLength() << ",URI=\"/stream/segment/" << TargetCameraInt << "/" << Part->GetSegmentIndex() << "." << Part->GetPartIndex() << ".mp4\"";
					if (Part->IsIsolated())
					{
						Playlist << ",INDEPENDENT=YES" << "\n";
					}
					else
					{
						Playlist << "\n";
					}
				}
			}

			if (Segment.Stream)
			{
				lastIndex = Segment.Stream->GetSegmentIndex();

				auto Length = Segment.Stream->GetClipLength();

				Playlist << "#EXTINF:" << Length << "," << "\n";
				Playlist << "/stream/segment/" << TargetCameraInt << "/" << Segment.Stream->GetSegmentIndex() << ".mp4" << "\n";
			}
		}

		//Playlist << "#EXT-X-PRELOAD-HINT:TYPE=PART,URI=\"/stream/segment/" << TargetCameraInt << "/" << (lastIndex+1) << ".mp4\"" << "\n";
		
		/*auto NextTime = Segments[0].StreamStartTime + Segments[0].Stream->GetClipLength();

		for (int segment = 0; segment < extraBufferSegments; segment++)
		{
			LiveStreamSegment& Segment = Segments[segment];

			auto Length = Segments[0].Stream->GetClipLength();
			Playlist << "#EXTINF:" << Length << "," << "\n";
			Playlist << "#EXT-X-PROGRAM-DATE-TIME:" << std::put_time(&Segment.StreamStartTime, L"%FT%T") << "\n";

			Playlist << "/stream/segment/" << TargetCameraInt << "/" << Segment.Stream->GetSegmentIndex() << ".mp4" << "\n";
		}*/
	}

	http_response Response;
	Response.set_status_code(status_codes::OK);
	Response.set_body(Playlist.str());
	Response.headers().set_content_type(_T("application/vnd.apple.mpegurl"));
	Response.headers().set_cache_control(_T("no-cache, no-store, must-revalidate"));

	//For testing with hls-demo
//#if _DEBUG
	Response.headers().add(U("Access-Control-Allow-Origin"), U("*"));
//#endif

	Message.reply(Response);
}

void Command_Stream::OnMessage( GlobalContext& Context, http_request& Message, const string_t& CurrentCommand, std::vector<string_t>& ChildPath, bool IsPost )
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
