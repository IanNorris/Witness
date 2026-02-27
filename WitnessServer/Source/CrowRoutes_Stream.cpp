#include "CrowListener.h"
#include "CrowAuth.h"
#include "GlobalContext.h"

#include <format>
#include <cmath>

void CrowListener::HandlePlaylist( const crow::request& req, crow::response& res, int cameraId )
{
	auto CameraState = m_GlobalContext->FindCameraById( cameraId );
	if( !CameraState )
	{
		res.code = 404;
		res.end();
		return;
	}

	std::shared_ptr<LiveOutputStream>& LiveStream = CameraState->Worker->GetLiveStream();
	if( !LiveStream )
	{
		res.code = 503;
		res.end();
		return;
	}

	std::vector<LiveStreamSegment> Segments;
	LiveStream->GetSegments( Segments );

	int CurrentSegment = LiveStream->GetCurrentSegment();
	int InitGeneration = LiveStream->GetInitGeneration();

	size_t bufferSegments = Segments.size();

	std::ostringstream Playlist;
	Playlist << "#EXTM3U\n";
	Playlist << "#EXT-X-VERSION:7\n";

	// Check for _HLS_msn query param
	auto msnParam = req.url_params.get( "_HLS_msn" );
	uint64_t msnStart = msnParam ? std::stoull( msnParam ) : 0;

	int startAtSegment = 0;
	if( bufferSegments > 0 )
	{
		double MaxLength = 0.0;
		for( int segment = 0; segment < (int)bufferSegments; segment++ )
		{
			LiveStreamSegment& Seg = Segments[segment];
			if( Seg.Ready )
			{
				if( Seg.Duration > MaxLength )
					MaxLength = Seg.Duration;
				if( (uint64_t)Seg.SegmentIndex == msnStart )
				{
					MaxLength = Seg.Duration;
					startAtSegment = segment;
				}
			}
		}

		double PartialTarget = LiveStream->GetPartialTargetDuration();
		double HoldbackLength = PartialTarget * 3.0;
		Playlist << "#EXT-X-SERVER-CONTROL:CAN-BLOCK-RELOAD=NO,PART-HOLD-BACK=" << HoldbackLength << "\n";
		Playlist << "#EXT-X-PART-INF:PART-TARGET=" << PartialTarget << "\n";

		double TargetDuration = MaxLength * 1.18;
		if( TargetDuration < 1.0 ) TargetDuration = 1.0;

		Playlist << "#EXT-X-MEDIA-SEQUENCE:" << Segments[startAtSegment].SegmentIndex << "\n";
		Playlist << "#EXT-X-INDEPENDENT-SEGMENTS\n";
		Playlist << "#EXT-X-TARGETDURATION:" << (int)std::ceil( TargetDuration ) << "\n";
		Playlist << "#EXT-X-MAP:URI=\"" << cameraId << "/0/i?g=" << InitGeneration << "\"\n";
		Playlist << "\n";

		Playlist.precision(4);

		for( int segment = startAtSegment; segment < (int)bufferSegments; segment++ )
		{
			LiveStreamSegment& Seg = Segments[segment];
			if( Seg.Ready )
			{
				if( Seg.Discontinuity )
				{
					Playlist << "#EXT-X-DISCONTINUITY\n";
					Playlist << "#EXT-X-MAP:URI=\"" << cameraId << "/0/i?g=" << InitGeneration << "\"\n";
				}

				std::string dateTimeFormat = std::format( "{:%Y-%m-%dT%H:%M:%S}", Seg.SegmentTime );
				Playlist << "#EXT-X-PROGRAM-DATE-TIME:" << dateTimeFormat << "\n";

				for( auto& Partial : Seg.Partials )
				{
					Playlist << "#EXT-X-PART:DURATION=" << Partial.Duration
						<< ",URI=\"" << cameraId << "/" << Seg.SegmentIndex << "/" << Partial.PartIndex << "\"";
					if( Partial.Independent )
						Playlist << ",INDEPENDENT=YES";
					Playlist << "\n";
				}

				Playlist << "#EXTINF:" << Seg.Duration << ",\n";
				Playlist << cameraId << "/" << Seg.SegmentIndex << "/f\n";
			}
			else
			{
				if( Seg.Discontinuity )
				{
					Playlist << "#EXT-X-DISCONTINUITY\n";
					Playlist << "#EXT-X-MAP:URI=\"" << cameraId << "/0/i?g=" << InitGeneration << "\"\n";
				}

				for( auto& Partial : Seg.Partials )
				{
					Playlist << "#EXT-X-PART:DURATION=" << Partial.Duration
						<< ",URI=\"" << cameraId << "/" << Seg.SegmentIndex << "/" << Partial.PartIndex << "\"";
					if( Partial.Independent )
						Playlist << ",INDEPENDENT=YES";
					Playlist << "\n";
				}
			}
		}
	}

	res.set_header( "Content-Type", "application/vnd.apple.mpegurl" );
	res.set_header( "Cache-Control", "no-cache, no-store, must-revalidate" );
	res.body = Playlist.str();
	res.code = 200;
	res.end();
}

void CrowListener::HandleSegment( const crow::request& req, crow::response& res, int cameraId, int segmentId, const std::string& partId )
{
	auto CameraState = m_GlobalContext->FindCameraById( cameraId );
	if( !CameraState )
	{
		res.code = 404;
		res.end();
		return;
	}

	std::shared_ptr<LiveOutputStream>& LiveStream = CameraState->Worker->GetLiveStream();
	if( !LiveStream )
	{
		res.code = 503;
		res.end();
		return;
	}

	bool IsFull = !partId.empty() && partId[0] == 'f';
	bool IsInit = !partId.empty() && partId[0] == 'i';
	bool IsPartial = !partId.empty() && partId[0] >= '0' && partId[0] <= '9';

	if( IsInit )
	{
		SegmentBuffer InitData = LiveStream->GetInitSegment();
		if( InitData && !InitData->empty() )
		{
			res.set_header( "Content-Type", "video/mp4" );
			res.set_header( "Access-Control-Allow-Origin", "*" );
			res.body.assign( (const char*)InitData->data(), InitData->size() );
			res.code = 200;
			res.end();
			return;
		}
	}
	else if( IsFull )
	{
		std::vector<LiveStreamSegment> Segments;
		LiveStream->GetSegments( Segments );

		for( auto& Seg : Segments )
		{
			if( Seg.SegmentIndex == segmentId && Seg.Ready && Seg.Data && !Seg.Data->empty() )
			{
				res.set_header( "Content-Type", "video/mp4" );
				res.set_header( "Access-Control-Allow-Origin", "*" );
				res.body.assign( (const char*)Seg.Data->data(), Seg.Data->size() );
				res.code = 200;
				res.end();
				return;
			}
		}
	}
	else if( IsPartial )
	{
		int targetPart = std::atoi( partId.c_str() );

		std::vector<LiveStreamSegment> Segments;
		LiveStream->GetSegments( Segments );

		for( auto& Seg : Segments )
		{
			if( Seg.SegmentIndex == segmentId )
			{
				if( targetPart >= 0 && targetPart < (int)Seg.Partials.size() )
				{
					auto& Partial = Seg.Partials[targetPart];
					if( Partial.Data && !Partial.Data->empty() )
					{
						res.set_header( "Content-Type", "video/mp4" );
						res.set_header( "Access-Control-Allow-Origin", "*" );
						res.body.assign( (const char*)Partial.Data->data(), Partial.Data->size() );
						res.code = 200;
						res.end();
						return;
					}
				}
				break;
			}
		}
	}

	res.code = 404;
	res.end();
}