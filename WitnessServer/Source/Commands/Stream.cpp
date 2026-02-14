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
#include <cmath>
#include <iostream>
#include <filesystem>
#include <format>

#ifdef _WIN32
#include <winerror.h>
#endif

using namespace web::json;
using namespace web::http::client;

namespace fs = std::filesystem;


void Command_Stream::OnSegmentMessage(const GlobalContext& Context, http_request& Message, const string_t& TargetCamera, const string_t& TargetSegment, const string_t& TargetPart, const json::value& Packet)
{
	
	int TargetCameraInt = _wtoi(TargetCamera.c_str());
	int TargetSegmentInt = _wtoi(TargetSegment.c_str());

	bool IsFull = TargetPart.length() != 0 && TargetPart[0] == 'f';
	bool IsInit = TargetPart.length() != 0 && TargetPart[0] == 'i';
	bool IsPartial = TargetPart.length() != 0 && TargetPart[0] >= '0' && TargetPart[0] <= '9';

#if !_DEBUG
	if (!Command_Authenticate::IsCameraAuthenticated(Context, Message, Packet, Command_Authenticate::Action::Read, Command_Authenticate::Privilege::Normal, TargetCameraInt))
	{
		return;
	}
#endif

	auto CameraState = Context.FindCameraById(TargetCameraInt);
	if (!CameraState)
	{
		Message.reply(status_codes::NotFound);
		return;
	}

	std::shared_ptr<LiveOutputStream>& LiveStream = CameraState->Worker->GetLiveStream();
	if (!LiveStream)
	{
		Message.reply(status_codes::ServiceUnavailable);
		return;
	}

	if (IsInit)
	{
		SegmentBuffer InitData = LiveStream->GetInitSegment();
		if (InitData && !InitData->empty())
		{
			http_response response(status_codes::OK);
			response.headers().add(U("Access-Control-Allow-Origin"), U("*"));
			response.headers().set_content_type(_T("video/mp4"));
			response.set_body(*InitData);
			Message.reply(response);
			return;
		}
	}
	else if (IsFull)
	{
		std::vector<LiveStreamSegment> Segments;
		LiveStream->GetSegments(Segments);

		for (auto& Seg : Segments)
		{
			if (Seg.SegmentIndex == TargetSegmentInt && Seg.Ready && Seg.Data && !Seg.Data->empty())
			{
				http_response response(status_codes::OK);
				response.headers().add(U("Access-Control-Allow-Origin"), U("*"));
				response.headers().set_content_type(_T("video/mp4"));
				response.set_body(*Seg.Data);
				Message.reply(response);
				return;
			}
		}
	}
	else if (IsPartial)
	{
		int TargetPartInt = _wtoi(TargetPart.c_str());

		std::vector<LiveStreamSegment> Segments;
		LiveStream->GetSegments(Segments);

		for (auto& Seg : Segments)
		{
			if (Seg.SegmentIndex == TargetSegmentInt)
			{
				if (TargetPartInt >= 0 && TargetPartInt < (int)Seg.Partials.size())
				{
					auto& Partial = Seg.Partials[TargetPartInt];
					if (Partial.Data && !Partial.Data->empty())
					{
						http_response response(status_codes::OK);
						response.headers().add(U("Access-Control-Allow-Origin"), U("*"));
						response.headers().set_content_type(_T("video/mp4"));
						response.set_body(*Partial.Data);
						Message.reply(response);
						return;
					}
				}
				break;
			}
		}
	}

	Message.reply( status_codes::NotFound );
}

void Command_Stream::OnPlaylistMessage(const GlobalContext& Context, http_request& Message, const string_t& TargetCamera, const json::value& Packet)
{
	auto queryMap = web::uri::split_query(Message.request_uri().query());
	auto msnIter = queryMap.find(_T("_HLS_msn"));

	int TargetCameraInt = _wtoi(TargetCamera.c_str());
	
#if !_DEBUG
	if (!Command_Authenticate::IsCameraAuthenticated(Context, Message, Packet, Command_Authenticate::Action::Read, Command_Authenticate::Privilege::Normal, TargetCameraInt))
	{
		return;
	}
#endif

	auto CameraState = Context.FindCameraById(TargetCameraInt);

	if (!CameraState)
	{
		Message.reply(status_codes::NotFound);
		return;
	}

	std::shared_ptr<LiveOutputStream>& LiveStream = CameraState->Worker->GetLiveStream();
	if (!LiveStream)
	{
		Message.reply(status_codes::ServiceUnavailable);
		return;
	}

	std::vector<LiveStreamSegment> Segments;
	LiveStream->GetSegments(Segments);

	int CurrentSegment = LiveStream->GetCurrentSegment();
	int InitGeneration = LiveStream->GetInitGeneration();

	size_t bufferSegments = Segments.size();
	
	stringstream_t Playlist;
	Playlist << "#EXTM3U" << "\n";
	Playlist << "#EXT-X-VERSION:7" << "\n";

	uint64_t msnStart = msnIter != queryMap.end() ? _wtoi64((*msnIter).second.c_str()) : 0;

	int startAtSegment = 0;
	if (bufferSegments > 0)
	{
		double MaxLength = 0.0;
		for (int segment = 0; segment < bufferSegments; segment++)
		{
			LiveStreamSegment& Segment = Segments[segment];

			if (Segment.Ready)
			{
				double NewDuration = Segment.Duration;
				if (NewDuration > MaxLength)
				{
					MaxLength = NewDuration;
				}

				if (Segment.SegmentIndex == msnStart)
				{
					MaxLength = NewDuration;
					startAtSegment = segment;
				}
			}
		}

		double PartialTarget = LiveStream->GetPartialTargetDuration();
		double HoldbackLength = PartialTarget * 3.0;
		Playlist << "#EXT-X-SERVER-CONTROL:CAN-BLOCK-RELOAD=NO,PART-HOLD-BACK=" << HoldbackLength << "\n";
		Playlist << "#EXT-X-PART-INF:PART-TARGET=" << PartialTarget << "\n";

		double TargetDuration = MaxLength * 1.18;
		if (TargetDuration < 1.0) TargetDuration = 1.0;

		Playlist << "#EXT-X-MEDIA-SEQUENCE:" << Segments[startAtSegment].SegmentIndex << "\n";
		Playlist << "#EXT-X-INDEPENDENT-SEGMENTS\n";
		Playlist << "#EXT-X-TARGETDURATION:" << (int)std::ceil(TargetDuration) << "\n";
		Playlist << "#EXT-X-MAP:URI=\"" << TargetCameraInt << "/0/i?g=" << InitGeneration << "\"" << "\n";
		Playlist << "" << "\n";

		Playlist.precision(4);

		for (int segment = startAtSegment; segment < bufferSegments; segment++)
		{
			LiveStreamSegment& Segment = Segments[segment];

			if (Segment.Ready)
			{
				// Signal discontinuity after camera reconnect — timestamps
				// and codec parameters may have changed
				if (Segment.Discontinuity)
				{
					Playlist << "#EXT-X-DISCONTINUITY\n";
					Playlist << "#EXT-X-MAP:URI=\"" << TargetCameraInt << "/0/i?g=" << InitGeneration << "\"" << "\n";
				}

				std::string dateTimeFormat = std::format("{:%Y-%m-%dT%H:%M:%S}", Segment.SegmentTime);

				Playlist << "#EXT-X-PROGRAM-DATE-TIME:" << string_t(dateTimeFormat.begin(), dateTimeFormat.end()) << "\n";

				for (auto& Partial : Segment.Partials)
				{
					Playlist << "#EXT-X-PART:DURATION=" << Partial.Duration
						<< ",URI=\"" << TargetCameraInt << "/" << Segment.SegmentIndex << "/" << Partial.PartIndex << "\"";
					if (Partial.Independent)
						Playlist << ",INDEPENDENT=YES";
					Playlist << "\n";
				}

				Playlist << "#EXTINF:" << Segment.Duration << "," << "\n";
				Playlist << TargetCameraInt << "/" << Segment.SegmentIndex << "/f\n";
			}
			else
			{
				if (Segment.Discontinuity)
				{
					Playlist << "#EXT-X-DISCONTINUITY\n";
					Playlist << "#EXT-X-MAP:URI=\"" << TargetCameraInt << "/0/i?g=" << InitGeneration << "\"" << "\n";
				}

				// In-progress segment: emit partials available so far
				for (auto& Partial : Segment.Partials)
				{
					Playlist << "#EXT-X-PART:DURATION=" << Partial.Duration
						<< ",URI=\"" << TargetCameraInt << "/" << Segment.SegmentIndex << "/" << Partial.PartIndex << "\"";
					if (Partial.Independent)
						Playlist << ",INDEPENDENT=YES";
					Playlist << "\n";
				}
			}
		}
	}

	http_response Response;
	Response.set_status_code(status_codes::OK);
	Response.set_body(Playlist.str());
	Response.headers().set_content_type(_T("application/vnd.apple.mpegurl"));
	Response.headers().set_cache_control(_T("no-cache, no-store, must-revalidate"));

	//For testing with hls-demo
#if _DEBUG
	Response.headers().add(U("Access-Control-Allow-Origin"), U("*"));
#endif

	Message.reply(Response);
}

void Command_Stream::OnMessage( GlobalContext& Context, http_request& Message, const string_t& CurrentCommand, std::vector<string_t>& ChildPath, bool IsPost )
{
	auto Packet = Message.extract_json().get();

	// /stream/1 (ChildPath = ["1"])
	if (ChildPath.size() == 1 && !IsPost)
	{
		OnPlaylistMessage(Context, Message, ChildPath[0], Packet);
	}
	// /stream/1/45/14 (ChildPath = ["1", "45", "14"]) or /stream/1/45/f (ChildPath = ["1", "45", "f"]) for full segments
	else if (ChildPath.size() == 3)
	{
		OnSegmentMessage(Context, Message, ChildPath[0], ChildPath[1], ChildPath[2], Packet);
	}
	else
	{
		Message.reply(status_codes::NotFound);
	}
}
