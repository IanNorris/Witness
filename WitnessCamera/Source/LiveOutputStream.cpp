#include "LiveOutputStream.h"
#include "LiveOutputStreamData.h"
#include "OutputStream.h"
#include "InputStream.h"
#include "StreamData.h"
#include "PacketCapture.h"
#include "TimestampRegressionGuard.h"
#include "AudioSampleClock.h"

#include <Log.h>
#include <algorithm>
#include <sstream>
#include <chrono>
#include <cmath>
#include <climits>
#include <filesystem>
#include <format>
#include <atomic>
#include <cstdlib>
#include <string_view>

namespace Witness{
namespace Camera{

static const int AVIOBufferSize = 64 * 1024;
static const uint64_t Fnv1aOffsetBasis = 14695981039346656037ULL;
static const uint64_t Fnv1aPrime = 1099511628211ULL;

// Fail open to evidence collection for absent/invalid values. This is only an
// optional packet-observer switch, never a general stream-validation bypass.
static bool RetainStreamTraceFromEnvironment(const char* Name)
{
#ifdef _WIN32
	char* Value = nullptr;
	size_t Required = 0;
	const auto Error = _dupenv_s(&Value, &Required, Name);
	const std::unique_ptr<char, decltype(&std::free)> Owned(Value, &std::free);
	return Error != 0 || !Value || std::string_view(Value) != "0";
#else
	const char* Value = std::getenv(Name);
	return !Value || std::string_view(Value) != "0";
#endif
}

struct PacketCaptureState
{
	std::mutex Mutex;
	std::shared_ptr<PacketCapture> Current;
};

static uint64_t HashBytes(const uint8_t* Data, size_t Size, uint64_t Seed = Fnv1aOffsetBasis)
{
	uint64_t Hash = Seed;
	for (size_t Index = 0; Index < Size; ++Index)
	{
		Hash ^= Data[Index];
		Hash *= Fnv1aPrime;
	}
	return Hash;
}

static uint32_t HashBytes32(const uint8_t* Data, size_t Size)
{
	uint32_t Hash = 2166136261U;
	for (size_t Index = 0; Index < Size; ++Index)
	{
		Hash ^= Data[Index];
		Hash *= 16777619U;
	}
	return Hash;
}

namespace
{
std::string RedactUrlCredentials( const char* Message )
{
	std::string Result = Message ? Message : "";
	size_t SearchFrom = 0;
	while( true )
	{
		const size_t SchemeEnd = Result.find( "://", SearchFrom );
		if( SchemeEnd == std::string::npos )
			break;
		const size_t AuthorityStart = SchemeEnd + 3;
		const size_t AuthorityEnd = Result.find_first_of( "/ \\t\\r\\n", AuthorityStart );
		const size_t At = Result.find( '@', AuthorityStart );
		if( At != std::string::npos &&
			(AuthorityEnd == std::string::npos || At < AuthorityEnd) )
		{
			const size_t Colon = Result.find( ':', AuthorityStart );
			if( Colon != std::string::npos && Colon < At )
			{
				Result.replace( Colon + 1, At - Colon - 1, "[redacted]" );
				SearchFrom = Colon + 11;
				continue;
			}
		}
		SearchFrom = AuthorityStart;
	}
	return Result;
}

std::string FFmpegMessageSignature( const std::string& Message )
{
	std::string Result;
	Result.reserve( Message.size() );
	bool InNumber = false;
	bool PreserveNumber = false;
	for( size_t Index = 0; Index < Message.size(); ++Index )
	{
		const char Character = Message[Index];
		const bool Digit = Character >= '0' && Character <= '9';
		if( Digit )
		{
			// FFmpeg's "stream 0"/"stream 1" identifies video versus audio.
			// Preserve that identifier while collapsing changing PTS/DTS values.
			if( !InNumber ) PreserveNumber = Index >= 7 &&
				Message.compare( Index - 7, 7, "stream " ) == 0;
			if( PreserveNumber ) Result += Character;
			else if( !InNumber ) Result += '#';
		}
		else
		{
			Result += Character;
		}
		InNumber = Digit;
	}
	return Result;
}

struct PacketStructure
{
	int Packetization = 0;
	int UnitCount = 0;
	int PrimaryUnitType = -1;
};

int CodecUnitType(AVCodecID Codec, uint8_t Header)
{
	if (Codec == AV_CODEC_ID_H264)
		return Header & 0x1f;
	if (Codec == AV_CODEC_ID_HEVC)
		return (Header >> 1) & 0x3f;
	return -1;
}

bool IsVideoCodingLayerUnit(AVCodecID Codec, int UnitType)
{
	return (Codec == AV_CODEC_ID_H264 && UnitType >= 1 && UnitType <= 5) ||
		(Codec == AV_CODEC_ID_HEVC && UnitType >= 0 && UnitType <= 31);
}

void RecordCodecUnit(PacketStructure& Result, AVCodecID Codec, uint8_t Header)
{
	const int UnitType = CodecUnitType(Codec, Header);
	if (Result.UnitCount == 0)
		Result.PrimaryUnitType = UnitType;
	else if (!IsVideoCodingLayerUnit(Codec, Result.PrimaryUnitType) &&
		IsVideoCodingLayerUnit(Codec, UnitType))
		Result.PrimaryUnitType = UnitType;
	++Result.UnitCount;
}

PacketStructure AnalysePacketStructure(AVCodecID Codec, const uint8_t* Data, size_t Size)
{
	PacketStructure Result;
	if (!Data || Size == 0)
		return Result;

	if (Codec == AV_CODEC_ID_AAC)
	{
		if (Size >= 2 && Data[0] == 0xff && (Data[1] & 0xf6) == 0xf0)
			Result.Packetization = 3;
		return Result;
	}
	if (Codec != AV_CODEC_ID_H264 && Codec != AV_CODEC_ID_HEVC)
		return Result;

	// RTP depacketizers normally produce Annex B access units. Record every
	// start code so parameter-set/IDR composition can be compared across faults.
	for (size_t Index = 0; Index + 3 < Size; )
	{
		size_t HeaderIndex = Size;
		if (Data[Index] == 0 && Data[Index + 1] == 0 && Data[Index + 2] == 1)
			HeaderIndex = Index + 3;
		else if (Index + 4 < Size && Data[Index] == 0 && Data[Index + 1] == 0 &&
			Data[Index + 2] == 0 && Data[Index + 3] == 1)
			HeaderIndex = Index + 4;
		if (HeaderIndex < Size)
		{
			Result.Packetization = 1;
			RecordCodecUnit(Result, Codec, Data[HeaderIndex]);
			Index = HeaderIndex + 1;
		}
		else
			++Index;
	}
	if (Result.UnitCount > 0)
		return Result;

	// MP4-style sources may expose four-byte length-prefixed NAL units.
	PacketStructure LengthPrefixed;
	size_t Offset = 0;
	while (Offset + 4 <= Size)
	{
		const uint32_t UnitSize = (uint32_t(Data[Offset]) << 24) |
			(uint32_t(Data[Offset + 1]) << 16) |
			(uint32_t(Data[Offset + 2]) << 8) | uint32_t(Data[Offset + 3]);
		Offset += 4;
		if (UnitSize == 0 || UnitSize > Size - Offset)
			return Result;
		RecordCodecUnit(LengthPrefixed, Codec, Data[Offset]);
		Offset += UnitSize;
	}
	if (Offset == Size && LengthPrefixed.UnitCount > 0)
	{
		LengthPrefixed.Packetization = 2;
		return LengthPrefixed;
	}
	return Result;
}

struct IsoBmffStructure
{
	bool Valid = false;
	int BoxCount = 0;
	int FtypCount = 0;
	int MoovCount = 0;
	int MoofCount = 0;
	int MdatCount = 0;
	int Error = 0;
	uint64_t ErrorOffset = 0;
};

uint32_t ReadBigEndian32(const uint8_t* Data)
{
	return (uint32_t(Data[0]) << 24) | (uint32_t(Data[1]) << 16) |
		(uint32_t(Data[2]) << 8) | uint32_t(Data[3]);
}

uint64_t ReadBigEndian64(const uint8_t* Data)
{
	return (uint64_t(ReadBigEndian32(Data)) << 32) | ReadBigEndian32(Data + 4);
}

constexpr uint32_t BoxType(char A, char B, char C, char D)
{
	return (uint32_t(uint8_t(A)) << 24) | (uint32_t(uint8_t(B)) << 16) |
		(uint32_t(uint8_t(C)) << 8) | uint32_t(uint8_t(D));
}

IsoBmffStructure AnalyseIsoBmff(const uint8_t* Data, size_t Size, bool InitSegment)
{
	IsoBmffStructure Result;
	size_t Offset = 0;
	while (Offset < Size)
	{
		if (Size - Offset < 8)
		{
			Result.Error = 1; // truncated box header
			Result.ErrorOffset = Offset;
			return Result;
		}
		uint64_t BoxSize = ReadBigEndian32(Data + Offset);
		const uint32_t Type = ReadBigEndian32(Data + Offset + 4);
		size_t HeaderSize = 8;
		if (BoxSize == 1)
		{
			if (Size - Offset < 16)
			{
				Result.Error = 1;
				Result.ErrorOffset = Offset;
				return Result;
			}
			BoxSize = ReadBigEndian64(Data + Offset + 8);
			HeaderSize = 16;
		}
		else if (BoxSize == 0)
		{
			BoxSize = Size - Offset;
		}
		if (BoxSize < HeaderSize)
		{
			Result.Error = 2; // invalid declared box size
			Result.ErrorOffset = Offset;
			return Result;
		}
		if (BoxSize > Size - Offset)
		{
			Result.Error = 3; // box extends beyond fragment
			Result.ErrorOffset = Offset;
			return Result;
		}

		++Result.BoxCount;
		if (Type == BoxType('f', 't', 'y', 'p')) ++Result.FtypCount;
		else if (Type == BoxType('m', 'o', 'o', 'v')) ++Result.MoovCount;
		else if (Type == BoxType('m', 'o', 'o', 'f')) ++Result.MoofCount;
		else if (Type == BoxType('m', 'd', 'a', 't')) ++Result.MdatCount;
		Offset += (size_t)BoxSize;
	}

	if (InitSegment)
	{
		if (Result.FtypCount == 0 || Result.MoovCount == 0)
			Result.Error = 4; // missing required init box
	}
	else if (Result.MoofCount == 0 || Result.MdatCount == 0 ||
		Result.MoofCount != Result.MdatCount)
	{
		Result.Error = 5; // incomplete media fragment pair
	}
	Result.Valid = Result.Error == 0;
	return Result;
}
}

LiveOutputStreamData::LiveOutputStreamData(const std::string& LiveCachePath, InputStream* InputStream)
	: _LiveCachePath( LiveCachePath )
	, _StreamBacklog( new std::vector<LiveStreamSegment>() )
	, _InputStream(InputStream)
	, _FormatContext( nullptr )
	, _AVIOContext( nullptr )
	, _AVIOBuffer( nullptr )
	, _HeaderWritten( false )
	, _InitSegmentCaptured( false )
	, _HasInitialDTS( false )
	, _HasBFrames( false )
	, _HasAudioStream( false )
	, _AllowTimestampNormalization( false )
	, _AllowObservedTimestampRegressionRepair( false )
	, _NormalizeNoBFrameTimestamps( false )
	, _TimestampNormalizationRejected( false )
	, _InitialDTS( 0 )
	, _InitialTimestampUs( AV_NOPTS_VALUE )
	, _LastInputDTS( AV_NOPTS_VALUE )
	, _LastPacketDuration( 0 )
	, _LastOutputPacketDuration( 0 )
	, _LastWrittenDTS( AV_NOPTS_VALUE )
	, _LastWrittenAudioDTS( AV_NOPTS_VALUE )
	, _AudioInputStreamIndex( -1 )
	, _SegmentStartDTS( 0 )
	, _OutputSegmentStartDTS( AV_NOPTS_VALUE )
	, _CurrentSegmentDuration( 0.0 )
	, _CurrentSegmentNominalDuration( 0.0 )
	, _TimestampProbeSamples( 0 )
	, _TimestampProbeOutliers( 0 )
	, _TimestampProbeInputTicks( 0 )
	, _TimestampProbeDurationTicks( 0 )
	, _TimestampStableCadenceSamples( 0 )
	, _TimestampStableCadenceTicks( 0 )
	, _SourceTimestampOffset( 0 )
	, _TimestampCorrectionRemainder( 0 )
	, _LastTimestampPhaseError( 0 )
	, _LastTimestampCorrection( 0 )
	, _LastTimestampCorrectionSaturated( false )
	, _CurrentSegmentIndex(0)
	, _CurrentPartialIndex(0)
	, _PartialStartDTS(AV_NOPTS_VALUE)
	, _CurrentPartialDuration(0.0)
	, _CurrentPartialAudioDuration(0.0)
	, _PartialTargetDuration(0.15)
	, _CurrentPartialIsIndependent(false)
	, _CurrentPartialHasPacket(false)
	, _CurrentPartialKeyframeSeekSafe(false)
	, _SegmentTimestampNormalizationActive(false)
	, _PartialBufferOffset(0)
	, _DiscontinuityPending(false)
	, _InitGeneration(0)
	, _SegmentsMutex( new std::mutex )
{
    _PendingVideoPacket = av_packet_alloc();
    _PacketCaptureState = new PacketCaptureState();
    _RetainPacketTrace = RetainStreamTraceFromEnvironment("WITNESS_PACKET_TRACE");
    _RetainMuxTrace = RetainStreamTraceFromEnvironment("WITNESS_MUX_TRACE");
}

LiveOutputStreamData::~LiveOutputStreamData()
{
    delete _PacketCaptureState;
    delete _StreamBacklog;
    delete _SegmentsMutex;
}

LiveOutputStream::LiveOutputStream(const std::string& LiveCachePath, InputStream* InputStream, int KeyframesPerSegment)
    : Stream(), _LiveData(new LiveOutputStreamData(LiveCachePath, InputStream))
{
}

LiveOutputStream::~LiveOutputStream()
{
    Shutdown();
    delete _LiveData;
}

int LiveOutputStream::GetCurrentSegment()
	{
		return _LiveData->_CurrentSegmentIndex;
	}

void LiveOutputStream::GetSegments(std::vector<LiveStreamSegment>& OutSegments )
	{
		const std::lock_guard<std::mutex> guard(*_LiveData->_SegmentsMutex);

		// Prune segments older than 10s so clients don't loop stale data.
		auto now = std::chrono::system_clock::now();
		std::erase_if( *_LiveData->_StreamBacklog, [&now]( const LiveStreamSegment& seg )
		{
			return std::chrono::duration_cast<std::chrono::seconds>(
				now - seg.SegmentTime ).count() > 10;
		});

		OutSegments = *_LiveData->_StreamBacklog;
	}

SegmentBuffer LiveOutputStream::GetInitSegment()
	{
		const std::lock_guard<std::mutex> guard(*_LiveData->_SegmentsMutex);

		return _LiveData->_InitSegmentData;
	}

int LiveOutputStream::GetInitGeneration()
	{
		const std::lock_guard<std::mutex> guard(*_LiveData->_SegmentsMutex);
		return _LiveData->_InitGeneration;
	}

LiveStreamInitSnapshot LiveOutputStream::GetInitSnapshot()
	{
		const std::lock_guard<std::mutex> guard(*_LiveData->_SegmentsMutex);
		return { _LiveData->_InitSegmentData, _LiveData->_InitSegmentGeneration, _LiveData->_InitAudioCodec };
	}

double LiveOutputStream::GetPartialTargetDuration()
	{
		return _LiveData->_PartialTargetDuration;
	}

void LiveOutputStream::SetPartialTargetDuration(double Duration)
	{
		_LiveData->_PartialTargetDuration = Duration;
	}

void LiveOutputStream::SetTimestampNormalizationAllowed(bool Allowed)
	{
		_LiveData->_AllowTimestampNormalization = Allowed;
	}

void LiveOutputStream::SetObservedTimestampRegressionRepairAllowed(bool Allowed)
	{
		_LiveData->_AllowObservedTimestampRegressionRepair = Allowed;
	}

void LiveOutputStream::SetEventCallback(LiveStreamEventCallback Callback)
	{
		const std::lock_guard<std::mutex> guard(*_LiveData->_SegmentsMutex);
		_LiveData->_EventCallback = std::move(Callback);
	}

CameraStreamError LiveOutputStream::Initialize()
{
	return CameraStreamError::Success;
}

CameraStreamError LiveOutputStream::ProcessFrame(const std::shared_ptr<IRecordFilter>& Filter, Stream* TargetStream, Stream* LiveStream)
{
	return CameraStreamError::Success;
}

uint64_t LiveOutputStream::BeginDiagnosticActivity()
{
	_LiveData->_CurrentDiagnosticActivity = ++_LiveData->_DiagnosticActivitySequence;
	return _LiveData->_CurrentDiagnosticActivity;
}

std::shared_ptr<PacketCapture> LiveOutputStream::ActivePacketCapture() const
{
	std::lock_guard lock(_LiveData->_PacketCaptureState->Mutex);
	return _LiveData->_PacketCaptureState->Current;
}

bool LiveOutputStream::StartPacketCapture(int CameraID, const std::string& Tier,
	int DurationSeconds, std::string& Directory)
{
	if ((Tier != "main" && Tier != "preview") ||
		CameraID <= 0 || DurationSeconds < 1 || DurationSeconds > 30)
		return false;
	std::lock_guard captureLock(_LiveData->_PacketCaptureState->Mutex);
	if (_LiveData->_PacketCaptureState->Current && !_LiveData->_PacketCaptureState->Current->Complete())
		return false;
	static std::atomic<uint64_t> NextCapture{ 0 };
	const auto Timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::system_clock::now().time_since_epoch()).count();
	const auto Path = std::filesystem::path(_LiveData->_LiveCachePath) / "packet-captures" /
		std::format("camera-{}-{}-{}-{}", CameraID, Tier,
			Timestamp, ++NextCapture);
	auto Capture = std::make_shared<PacketCapture>(Path, DurationSeconds);
	if (!Capture->Ready()) return false;
	Capture->AddMetadata(std::format(
		"{{\"type\":\"capture\",\"schemaVersion\":1,\"cameraId\":{},\"tier\":\"{}\",\"durationSeconds\":{}",
		CameraID, Tier, DurationSeconds));
	// Put the current init before any media partial. Starting mid-generation is
	// expected; consumers should begin replay at the next independent partial.
	SegmentBuffer Init;
	int Generation = 0;
	{
		std::lock_guard segmentLock(*_LiveData->_SegmentsMutex);
		Init = _LiveData->_InitSegmentData;
		Generation = _LiveData->_InitSegmentGeneration;
	}
	if (Init && !Init->empty())
		Capture->AddOutput(std::format(
			"{{\"type\":\"init\",\"generation\":{}", Generation),
			Init->data(), Init->size());
	_LiveData->_PacketCaptureState->Current = std::move(Capture);
	Directory = Path.string();
	LOG_INFO("[PacketCapture] Camera %d %s: capturing up to %ds in %s",
		CameraID, Tier.c_str(), DurationSeconds, Directory.c_str());
	return true;
}

void LiveOutputStream::CaptureInputPacket(const AVPacket* Packet, uint64_t ActivityID)
{
	auto Capture = ActivePacketCapture();
	if (!Capture || Capture->Complete() || !Packet || !Packet->data ||
		Packet->size <= 0 || !_LiveData->_InputStream)
		return;
	const auto& Data = _LiveData->_InputStream->GetData();
	if (Packet->stream_index != Data.ChosenStreamIndex &&
		Packet->stream_index != Data.ChosenAudioStreamIndex)
		return;
	const AVStream* Stream = Data.FormatContext->streams[Packet->stream_index];
	const int64_t ObservedSteadyUs = std::chrono::duration_cast<std::chrono::microseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
	const bool Audio = Packet->stream_index == Data.ChosenAudioStreamIndex;
	if (Capture->FirstPacketForStream(Packet->stream_index))
	{
		const auto* Parameters = Stream->codecpar;
		Capture->AddMetadata(std::format(
			"{{\"type\":\"stream\",\"streamIndex\":{},\"audio\":{},\"codecId\":{},\"timeBaseNum\":{},\"timeBaseDen\":{}",
			Packet->stream_index, Audio, static_cast<int>(Parameters->codec_id),
			Stream->time_base.num, Stream->time_base.den));
		if (Parameters->extradata && Parameters->extradata_size > 0)
			Capture->AddInput(std::format(
				"{{\"type\":\"extradata\",\"streamIndex\":{}",
				Packet->stream_index), Parameters->extradata,
				static_cast<size_t>(Parameters->extradata_size));
	}
	const uint64_t Hash = HashBytes(Packet->data, static_cast<size_t>(Packet->size));
	Capture->AddInput(std::format(
		"{{\"type\":\"input\",\"activityId\":{},\"audio\":{},\"streamIndex\":{},\"codecId\":{},\"timeBaseNum\":{},\"timeBaseDen\":{},\"dts\":{},\"pts\":{},\"duration\":{},\"flags\":{},\"hashFnv64\":\"{:016x}\",\"observedSteadyUs\":{}",
		ActivityID, Audio, Packet->stream_index,
		static_cast<int>(Stream->codecpar->codec_id), Stream->time_base.num,
		Stream->time_base.den, Packet->dts, Packet->pts, Packet->duration,
		Packet->flags, Hash, ObservedSteadyUs), Packet->data, static_cast<size_t>(Packet->size));
}

uint64_t LiveOutputStream::RecordFFmpegLog( int Level, const char* Phase,
	const char* Component, const char* Message, uint64_t ActivityID )
{
	MediaDiagnosticEvent Event;
	Event.TimestampUnixMs = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::system_clock::now().time_since_epoch()).count();
	Event.ElapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now() - _LiveData->_PacketDiagEpoch).count();
	Event.LastTimestampUnixMs = Event.TimestampUnixMs;
	Event.LastElapsedMs = Event.ElapsedMs;
	Event.ActivityID = ActivityID;
	Event.Category = "ffmpeg";
	Event.Severity = Level <= AV_LOG_ERROR ? "error" : "warning";
	Event.Phase = Phase ? std::string( Phase ).substr( 0, 64 ) : "unknown";
	Event.Component = Component ? std::string( Component ).substr( 0, 128 ) : "unknown";
	Event.Message = RedactUrlCredentials( Message ).substr( 0, 1024 );
	Event.ClusterSignature = FFmpegMessageSignature( Event.Message );

	const std::lock_guard<std::mutex> Guard( *_LiveData->_SegmentsMutex );
	// Keep repeated numeric variants of one FFmpeg warning in a single bounded
	// event, preserving its first message and the latest sample. Other media
	// events can no longer be displaced by hundreds of identical AAC warnings.
	for( int Offset = 0; Offset < _LiveData->_MediaEventRingCount; ++Offset )
	{
		auto& Previous = _LiveData->_MediaEventRing[(_LiveData->_MediaEventRingPos - 1 - Offset) % _LiveData->MEDIA_EVENT_RING_SIZE];
		if( Previous.Category != "ffmpeg" || Previous.Severity != Event.Severity ||
			Previous.Phase != Event.Phase || Previous.Component != Event.Component ||
			Previous.Generation != _LiveData->_InitGeneration ||
			Previous.ClusterSignature != Event.ClusterSignature ) continue;
		const int64_t SinceLast = Event.ElapsedMs - Previous.LastElapsedMs;
		const int64_t ClusterAge = Event.ElapsedMs - Previous.ElapsedMs;
		if( SinceLast < 0 || SinceLast > 60000 || ClusterAge < 0 || ClusterAge > 300000 )
			continue;
		++Previous.Count;
		Previous.LastTimestampUnixMs = Event.TimestampUnixMs;
		Previous.LastElapsedMs = Event.ElapsedMs;
		Previous.LastMessage = Event.Message;
		return Previous.Count;
	}
	Event.Sequence = ++_LiveData->_MediaEventSequence;
	Event.PacketSequence = _LiveData->_PacketDiagSequence;
	Event.Generation = _LiveData->_InitGeneration;
	Event.SegmentIndex = _LiveData->_CurrentSegmentIndex;
	Event.PartialIndex = _LiveData->_CurrentPartialIndex;
	_LiveData->_MediaEventRing[_LiveData->_MediaEventRingPos % _LiveData->MEDIA_EVENT_RING_SIZE] = std::move( Event );
	++_LiveData->_MediaEventRingPos;
	if( _LiveData->_MediaEventRingCount < _LiveData->MEDIA_EVENT_RING_SIZE )
		++_LiveData->_MediaEventRingCount;
	return 1;
}

void LiveOutputStream::Shutdown()
{
	if (_LiveData->_FormatContext)
	{
		if (_LiveData->_FormatContext->pb)
		{
			av_interleaved_write_frame(_LiveData->_FormatContext, nullptr);
			av_write_frame(_LiveData->_FormatContext, nullptr);
			avio_flush(_LiveData->_FormatContext->pb);
		}

		// Detach our custom AVIO so avformat doesn't try to close it
		_LiveData->_FormatContext->pb = nullptr;

		avformat_free_context(_LiveData->_FormatContext);
		_LiveData->_FormatContext = nullptr;
	}

	if (_LiveData->_AVIOContext)
	{
		// Buffer is owned by us, not av_free'd by avio
		av_free(_LiveData->_AVIOContext);
		_LiveData->_AVIOContext = nullptr;
	}

	if (_LiveData->_AVIOBuffer)
	{
		av_free(_LiveData->_AVIOBuffer);
		_LiveData->_AVIOBuffer = nullptr;
	}

	_LiveData->_CurrentBuffer.reset();
	if( _LiveData->_PendingVideoPacket )
		av_packet_free( &_LiveData->_PendingVideoPacket );
}

void LiveOutputStream::ResetForReconnect(InputStream* NewInputStream)
{
	// Tear down FFmpeg state but keep segments/init so HLS.js doesn't lose its reference
	if (_LiveData->_FormatContext)
	{
		// Do NOT call av_write_frame(NULL)/avio_flush here — the muxer may
		// be in an inconsistent state (e.g. audio stream created but no
		// audio packets written), and flushing can cause a crash.
		_LiveData->_FormatContext->pb = nullptr;
		avformat_free_context(_LiveData->_FormatContext);
		_LiveData->_FormatContext = nullptr;
	}

	if (_LiveData->_AVIOContext)
	{
		av_free(_LiveData->_AVIOContext);
		_LiveData->_AVIOContext = nullptr;
	}

	if (_LiveData->_AVIOBuffer)
	{
		av_free(_LiveData->_AVIOBuffer);
		_LiveData->_AVIOBuffer = nullptr;
	}

	_LiveData->_CurrentBuffer.reset();
	if( _LiveData->_PendingVideoPacket )
		av_packet_unref( _LiveData->_PendingVideoPacket );
	_LiveData->_PendingVideoActivityID = 0;
	_LiveData->_InputStream = NewInputStream;
	_LiveData->_HeaderWritten = false;
	_LiveData->_InitSegmentCaptured = false;
	_LiveData->_HasInitialDTS = false;
	_LiveData->_HasBFrames = false;
	_LiveData->_HasAudioStream = false;
	_LiveData->_NormalizeNoBFrameTimestamps = false;
	_LiveData->_TimestampNormalizationRejected = false;
    _LiveData->_TimestampRequalification = {};
    _LiveData->_TimestampPauseRecovery = {};
    _LiveData->_RequalificationArrivalGuard.Reset();
    _LiveData->_RequalificationCollecting = false;
	_LiveData->_InitialDTS = 0;
	_LiveData->_InitialTimestampUs = AV_NOPTS_VALUE;
	_LiveData->_LastInputDTS = AV_NOPTS_VALUE;
	_LiveData->_LastPacketDuration = 0;
	_LiveData->_LastOutputPacketDuration = 0;
	_LiveData->_LastWrittenDTS = AV_NOPTS_VALUE;
	_LiveData->_LastWrittenAudioDTS = AV_NOPTS_VALUE;
	_LiveData->_AacFrameSamples = 0;
	_LiveData->_NextAacSample = -1;
	_LiveData->_AudioInputStreamIndex = -1;
	_LiveData->_OutputSegmentStartDTS = AV_NOPTS_VALUE;
	_LiveData->_TimestampProbeSamples = 0;
	_LiveData->_TimestampProbeOutliers = 0;
	_LiveData->_TimestampProbeInputTicks = 0;
	_LiveData->_TimestampProbeDurationTicks = 0;
	_LiveData->_TimestampCadenceClock.Reset();
	_LiveData->_TimestampDeliveryEvidence = {};
	_LiveData->_TimestampArrivalGuard.Reset();
	_LiveData->_TimestampStableCadenceSamples = 0;
	_LiveData->_TimestampStableCadenceTicks = 0;
	_LiveData->_SourceTimestampOffset = 0;
	_LiveData->_TimestampCorrectionRemainder = 0;
	_LiveData->_LastTimestampPhaseError = 0;
	_LiveData->_LastTimestampCorrection = 0;
	_LiveData->_LastTimestampCorrectionSaturated = false;
	_LiveData->_PartialBufferOffset = 0;
	_LiveData->_CurrentPartialDuration = 0.0;
	_LiveData->_CurrentPartialAudioDuration = 0.0;
	_LiveData->_CurrentSegmentNominalDuration = 0.0;
	_LiveData->_CurrentPartialIsIndependent = false;
	_LiveData->_CurrentPartialHasPacket = false;
	_LiveData->_CurrentPartialKeyframeSeekSafe = false;
	_LiveData->_DiscontinuityPending = true;
	_LiveData->_DecodeCorruptionActive = false;
	_LiveData->_DecodeCorruptionActivityID = 0;
	_LiveData->_RecoveryPendingPublication = false;
	_LiveData->_RecoverySegmentIndex = -1;
	_LiveData->_RecoveryPartialIndex = -1;
	_LiveData->_RecoveryActivityID = 0;
	_LiveData->_RecoveryPacketSequence = 0;
	{
		const std::lock_guard<std::mutex> guard(*_LiveData->_SegmentsMutex);
		_LiveData->_StartupGraceStarted = {};
		_LiveData->_StreamEstablished = false;
		_LiveData->_StartupAcceptedVideoPackets = 0;
		_LiveData->_StartupDroppedVideoPackets = 0;
		_LiveData->_StartupRepairedVideoTimestamps = 0;
		_LiveData->_GenerationAcceptedVideoPacketsBaseline = _LiveData->_DiagAcceptedVideoPackets;
		_LiveData->_GenerationDroppedVideoPacketsBaseline = _LiveData->_DiagDroppedVideoPackets;
		_LiveData->_GenerationRepairedVideoTimestampsBaseline = _LiveData->_DiagRepairedVideoTimestamps;
		_LiveData->_EstablishedAcceptedVideoPacketsBaseline = _LiveData->_DiagAcceptedVideoPackets;
		_LiveData->_EstablishedDroppedVideoPacketsBaseline = _LiveData->_DiagDroppedVideoPackets;
		_LiveData->_EstablishedRepairedVideoTimestampsBaseline = _LiveData->_DiagRepairedVideoTimestamps;
		_LiveData->_InitGeneration++;
        _LiveData->_DiagTimestampRejected = false;
        _LiveData->_DiagTimestampRetryAttempts = 0;
		_LiveData->_DiagTimestampArrivalRateAdjustments = 0;
		_LiveData->_DiagTimestampArrivalDurationMs = 0;
		_LiveData->_DiagVideoPhaseErrorMs = 0.0;
		_LiveData->_DiagVideoCorrectionMs = 0.0;
		_LiveData->_DiagLastVideoOutputUs = 0;
		_LiveData->_DiagLastAudioOutputUs = 0;
		_LiveData->_DiagHasVideoOutputTimestamp = false;
		_LiveData->_DiagHasAudioOutputTimestamp = false;
		_LiveData->_DiagInitStructureObserved = false;
		_LiveData->_DiagInitStructureValid = false;
		_LiveData->_DiagInitBoxCount = 0;
		_LiveData->_DiagInitFtypCount = 0;
		_LiveData->_DiagInitMoovCount = 0;
		_LiveData->_DiagInitStructureError = 0;
		_LiveData->_DiagInitErrorOffset = 0;
	}

	// Notify MSE subscribers of discontinuity before new init segment arrives
	if (_LiveData->_EventCallback)
	{
		LiveStreamEvent Event;
		Event.EventType = LiveStreamEvent::Discontinuity;
		Event.Generation = _LiveData->_InitGeneration;
		_LiveData->_EventCallback(Event);
	}

	LOG_INFO("[HLS] Live stream reconnect (generation %d), segments so far: %d, cumulative drift: %.1fms",
		_LiveData->_InitGeneration, _LiveData->_DiagTotalSegments,
		(_LiveData->_DiagTotalAccumulatedDuration - _LiveData->_DiagTotalDtsDuration) * 1000.0);

	// Remove any orphaned incomplete segment from the backlog —
	// otherwise HLS.js will try to load it and get a 404.
	{
		const std::lock_guard<std::mutex> guard(*_LiveData->_SegmentsMutex);
		if (!_LiveData->_StreamBacklog->empty() && !_LiveData->_StreamBacklog->back().Ready)
		{
			_LiveData->_StreamBacklog->pop_back();
		}
	}
}

int LiveOutputStream::WriteBuffer(void* Opaque, const uint8_t* Buffer, int BufferSize)
{
	LiveOutputStream* Self = static_cast<LiveOutputStream*>(Opaque);

	if (Self->_LiveData->_CurrentBuffer && BufferSize > 0)
	{
		Self->_LiveData->_CurrentBuffer->insert(Self->_LiveData->_CurrentBuffer->end(), Buffer, Buffer + BufferSize);
	}

	return BufferSize;
}

int64_t LiveOutputStream::SeekBuffer(void* Opaque, int64_t Offset, int Origin)
{
	// AVSEEK_SIZE: return total buffer size
	if (Origin == AVSEEK_SIZE)
	{
		LiveOutputStream* Self = static_cast<LiveOutputStream*>(Opaque);
		return Self->_LiveData->_CurrentBuffer ? (int64_t)Self->_LiveData->_CurrentBuffer->size() : 0;
	}

	// For fragmented MP4 with frag_custom, seeks shouldn't happen on the
	// output side after the header. Return 0 to indicate unseekable.
	return -1;
}

void LiveOutputStream::SetupMemoryIO()
{
	_LiveData->_AVIOBuffer = (uint8_t*)av_malloc(AVIOBufferSize);

	_LiveData->_AVIOContext = avio_alloc_context(
		_LiveData->_AVIOBuffer,
		AVIOBufferSize,
		1, // writable
		this,
		nullptr, // no read
		&LiveOutputStream::WriteBuffer,
		&LiveOutputStream::SeekBuffer
	);

	_LiveData->_AVIOContext->seekable = 0;

	_LiveData->_FormatContext->pb = _LiveData->_AVIOContext;
	_LiveData->_FormatContext->flags |= AVFMT_FLAG_CUSTOM_IO;
}

CameraStreamError LiveOutputStream::InitFormatContext()
{
	if (_LiveData->_FormatContext)
	{
		return CameraStreamError::Success;
	}

	_LiveData->_InputStream->Initialize();

	auto& InID = _LiveData->_InputStream->GetData();
	if (!InID.CodecContext)
	{
		STREAM_ERROR(NoStreamInput, 0);
	}

	int Result = avformat_alloc_output_context2(&_LiveData->_FormatContext, nullptr, "mp4", nullptr);
	if (Result < 0 || !_LiveData->_FormatContext)
	{
		STREAM_ERROR(UnknownError, Result);
	}

	AVStream* OutStream = avformat_new_stream(_LiveData->_FormatContext, nullptr);
	if (!OutStream)
	{
		STREAM_ERROR(UnknownError, 0);
	}

	Result = avcodec_parameters_from_context(OutStream->codecpar, InID.CodecContext);
	if (Result < 0)
	{
		STREAM_ERROR(DecoderReceiverError, Result);
	}

	// Detect B-frame usage for conditional PTS=DTS handling
	_LiveData->_HasBFrames = InID.CodecContext->has_b_frames > 0;
	AVStream* VideoInStream = InID.FormatContext->streams[InID.ChosenStreamIndex];
	AVCodecParameters* VideoParams = VideoInStream->codecpar;
	{
		const std::lock_guard<std::mutex> guard(*_LiveData->_SegmentsMutex);
		_LiveData->_DiagVideoCodec = avcodec_get_name(InID.CodecContext->codec_id);
		_LiveData->_DiagAudioCodec.clear();
		_LiveData->_DiagInputFormat = InID.FormatContext->iformat && InID.FormatContext->iformat->name ?
			InID.FormatContext->iformat->name : "";
		_LiveData->_DiagVideoProfile = VideoParams->profile;
		_LiveData->_DiagVideoLevel = VideoParams->level;
		_LiveData->_DiagVideoWidth = VideoParams->width;
		_LiveData->_DiagVideoHeight = VideoParams->height;
		_LiveData->_DiagVideoTimeBaseNum = VideoInStream->time_base.num;
		_LiveData->_DiagVideoTimeBaseDen = VideoInStream->time_base.den;
		_LiveData->_DiagVideoExtradataBytes = VideoParams->extradata_size;
		_LiveData->_DiagVideoExtradataHash = VideoParams->extradata && VideoParams->extradata_size > 0 ?
			HashBytes(VideoParams->extradata, VideoParams->extradata_size) : 0;
		_LiveData->_DiagAudioProfile = 0;
		_LiveData->_DiagAudioSampleRate = 0;
		_LiveData->_DiagAudioChannels = 0;
		_LiveData->_DiagAudioTimeBaseNum = 0;
		_LiveData->_DiagAudioTimeBaseDen = 0;
		_LiveData->_DiagAudioExtradataBytes = 0;
		_LiveData->_DiagAudioExtradataHash = 0;
	}
	LOG_INFO("[HLS] Camera stream %s B-frames (has_b_frames=%d)",
		_LiveData->_HasBFrames ? "has" : "does not have", InID.CodecContext->has_b_frames);

	// Remap deprecated pixel formats
	switch (OutStream->codecpar->format)
	{
	case AV_PIX_FMT_YUVJ420P: OutStream->codecpar->format = AV_PIX_FMT_YUV420P; break;
	case AV_PIX_FMT_YUVJ422P: OutStream->codecpar->format = AV_PIX_FMT_YUV422P; break;
	case AV_PIX_FMT_YUVJ444P: OutStream->codecpar->format = AV_PIX_FMT_YUV444P; break;
	case AV_PIX_FMT_YUVJ440P: OutStream->codecpar->format = AV_PIX_FMT_YUV440P; break;
	}

	OutStream->codecpar->codec_tag = 0;
	OutStream->time_base = InID.FormatContext->streams[InID.ChosenStreamIndex]->time_base;

	if (InID.HasAudio)
	{
		AVStream* AudioInStream = InID.FormatContext->streams[InID.ChosenAudioStreamIndex];
		AVStream* AudioOutStream = avformat_new_stream(_LiveData->_FormatContext, nullptr);
		if (!AudioOutStream || avcodec_parameters_copy(AudioOutStream->codecpar, AudioInStream->codecpar) < 0)
		{
			STREAM_ERROR(DecoderReceiverError, 0);
		}
		AudioOutStream->codecpar->codec_tag = 0;
		AudioOutStream->time_base = AudioInStream->time_base;
		_LiveData->_HasAudioStream = true;
		_LiveData->_AudioInputStreamIndex = InID.ChosenAudioStreamIndex;
		if (_LiveData->_AllowTimestampNormalization && AudioInStream->codecpar->codec_id == AV_CODEC_ID_AAC &&
			AudioInStream->time_base.num == 1 &&
			AudioInStream->time_base.den == AudioInStream->codecpar->sample_rate)
			_LiveData->_AacFrameSamples = AacLcFrameSamples(AudioInStream->codecpar->extradata,
				AudioInStream->codecpar->extradata_size, AudioInStream->codecpar->sample_rate);
		{
			const std::lock_guard<std::mutex> guard(*_LiveData->_SegmentsMutex);
			_LiveData->_DiagAudioCodec = avcodec_get_name(AudioInStream->codecpar->codec_id);
			_LiveData->_DiagAudioProfile = AudioInStream->codecpar->profile;
			_LiveData->_DiagAudioSampleRate = AudioInStream->codecpar->sample_rate;
			_LiveData->_DiagAudioChannels = AudioInStream->codecpar->ch_layout.nb_channels;
			_LiveData->_DiagAudioTimeBaseNum = AudioInStream->time_base.num;
			_LiveData->_DiagAudioTimeBaseDen = AudioInStream->time_base.den;
			_LiveData->_DiagAudioExtradataBytes = AudioInStream->codecpar->extradata_size;
			_LiveData->_DiagAudioExtradataHash = AudioInStream->codecpar->extradata &&
				AudioInStream->codecpar->extradata_size > 0 ?
				HashBytes(AudioInStream->codecpar->extradata,
					AudioInStream->codecpar->extradata_size) : 0;
		}
		LOG_INFO("[HLS] AAC audio passthrough enabled");
	}

	// Set up in-memory I/O
	SetupMemoryIO();

	return CameraStreamError::Success;
}

void LiveOutputStream::NotifyDecodeCorruption(int ErrorFlags)
{
	_LiveData->_DecodeCorruptionActivityID = (std::max)(
		_LiveData->_DecodeCorruptionActivityID, _LiveData->_CurrentDiagnosticActivity);
	_LiveData->_RecoveryPendingPublication = false;
	_LiveData->_RecoverySegmentIndex = -1;
	_LiveData->_RecoveryPartialIndex = -1;
	_LiveData->_RecoveryActivityID = 0;
	_LiveData->_RecoveryPacketSequence = 0;
	if (_LiveData->_DecodeCorruptionActive)
		return;

	_LiveData->_DecodeCorruptionActive = true;
	{
		MediaDiagnosticEvent Event;
		Event.TimestampUnixMs = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::system_clock::now().time_since_epoch()).count();
		Event.ElapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now() - _LiveData->_PacketDiagEpoch).count();
		Event.ActivityID = _LiveData->_CurrentDiagnosticActivity;
		Event.Category = "decode";
		Event.Severity = "error";
		Event.Phase = "presentation";
		Event.Component = "decoder";
		Event.Message = "Corrupt video detected; presentation held until recovery keyframe (flags=" +
			std::to_string( ErrorFlags ) + ")";
		Event.Disposition = "decodeCorruption";
		const std::lock_guard<std::mutex> Guard( *_LiveData->_SegmentsMutex );
		++_LiveData->_DiagDecodeCorruptionEvents;
		Event.Sequence = ++_LiveData->_MediaEventSequence;
		Event.PacketSequence = _LiveData->_PacketDiagSequence;
		Event.Generation = _LiveData->_InitGeneration;
		Event.SegmentIndex = _LiveData->_CurrentSegmentIndex;
		Event.PartialIndex = _LiveData->_CurrentPartialIndex;
		_LiveData->_MediaEventRing[_LiveData->_MediaEventRingPos % _LiveData->MEDIA_EVENT_RING_SIZE] = std::move( Event );
		++_LiveData->_MediaEventRingPos;
		if( _LiveData->_MediaEventRingCount < _LiveData->MEDIA_EVENT_RING_SIZE )
			++_LiveData->_MediaEventRingCount;
	}
	LOG_WARNING("[HLS] Camera %d detected corrupt video; holding presentation until the next keyframe (decode_error_flags=0x%x)",
		_LiveData->_InputStream->GetSourceId(), ErrorFlags);

	if (_LiveData->_EventCallback)
	{
		LiveStreamEvent Event;
		Event.EventType = LiveStreamEvent::DecodeCorruption;
		Event.SegmentIndex = _LiveData->_CurrentSegmentIndex;
		Event.PartIndex = _LiveData->_CurrentPartialIndex;
		Event.Generation = _LiveData->_InitGeneration;
		Event.DecodeErrorFlags = ErrorFlags;
		_LiveData->_EventCallback(Event);
	}
}

CameraStreamError LiveOutputStream::WriteInterleavedPacket(const AVPacket* Packet)
{
	if( !Packet || !_LiveData->_InputStream )
		return CameraStreamError::Success;

	const auto& InputData = _LiveData->_InputStream->GetData();
	const bool IsVideo = Packet->stream_index == InputData.ChosenStreamIndex;

    if (IsVideo && _LiveData->_AllowObservedTimestampRegressionRepair && !_LiveData->_AllowTimestampNormalization &&
        !_LiveData->_HasAudioStream && !_LiveData->_HasBFrames && _LiveData->_NormalizeNoBFrameTimestamps &&
        _LiveData->_TimestampCadenceClock.UseArrival && !_LiveData->_TimestampNormalizationRejected) {
        const auto Timebase = InputData.FormatContext->streams[InputData.ChosenStreamIndex]->time_base;
        const int64_t NowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - _LiveData->_PacketDiagEpoch).count();
        const bool Valid = Packet->dts != AV_NOPTS_VALUE && Packet->pts == Packet->dts &&
            !(Packet->flags & AV_PKT_FLAG_CORRUPT) &&
            (!_LiveData->_DecodeCorruptionActive || _LiveData->_DecodeCorruptionActivityID < _LiveData->_CurrentDiagnosticActivity);
        if (_LiveData->_TimestampPauseRecovery.Observe(NowMs, av_rescale_q(NowMs, AVRational{1,1000}, Timebase),
            Packet->duration, Valid, Valid && (Packet->flags & AV_PKT_FLAG_KEY),
            av_rescale_q(AV_TIME_BASE, AV_TIME_BASE_Q, Timebase))) {
            const auto Retry = _LiveData->_TimestampRequalification;
            const auto GapMs = _LiveData->_TimestampPauseRecovery.LastGapMs;
            // Fresh cadence has been verified on the same declared clock.
            // Reset before retaining this keyframe; never reset inside the
            // pending-packet writer or concatenate incompatible mux epochs.
            ResetForReconnect(_LiveData->_InputStream);
            const auto Result = InitFormatContext();
            if (Result != CameraStreamError::Success) return Result;
            _LiveData->_TimestampRequalification = Retry;
            if (!_LiveData->_HasAudioStream && !_LiveData->_HasBFrames) {
                _LiveData->_NormalizeNoBFrameTimestamps = true;
                _LiveData->_TimestampCadenceClock.StartArrival(
                    av_rescale_q(NowMs, AVRational{1,1000}, Timebase), 0);
            }
            {
                const std::lock_guard<std::mutex> Guard(*_LiveData->_SegmentsMutex);
                ++_LiveData->_DiagTimestampPauseResets;
                _LiveData->_DiagTimestampLastPauseGapMs = GapMs;
                _LiveData->_DiagTimestampRetryAttempts = Retry.Attempts;
            }
            LOG_INFO("[HLS] Source %d recovered %lldms delivery pause at fresh-cadence keyframe",
                _LiveData->_InputStream->GetSourceId(), (long long)GapMs);
        }
    }

    if (IsVideo && _LiveData->_AllowObservedTimestampRegressionRepair && !_LiveData->_AllowTimestampNormalization &&
        !_LiveData->_HasAudioStream && !_LiveData->_HasBFrames && _LiveData->_TimestampNormalizationRejected) {
        const int64_t NowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - _LiveData->_PacketDiagEpoch).count();
        if (_LiveData->_TimestampRequalification.Eligible(NowMs)) {
            if (!_LiveData->_RequalificationCollecting) {
                _LiveData->_RequalificationArrivalGuard.Reset();
                _LiveData->_RequalificationCollecting = true;
            }
            const AVRational Timebase = InputData.FormatContext->streams[InputData.ChosenStreamIndex]->time_base;
            const int64_t ArrivalTicks = av_rescale_q(NowMs, AVRational{1,1000}, Timebase);
            const bool Valid = Packet->dts != AV_NOPTS_VALUE && Packet->pts == Packet->dts &&
                !(Packet->flags & AV_PKT_FLAG_CORRUPT) &&
                (!_LiveData->_DecodeCorruptionActive || _LiveData->_DecodeCorruptionActivityID < _LiveData->_CurrentDiagnosticActivity);
            _LiveData->_RequalificationArrivalGuard.Observe(ArrivalTicks, Packet->duration, Valid);
            const bool CadenceValid = _LiveData->_RequalificationArrivalGuard.Ready(av_rescale_q(
                AV_TIME_BASE, AV_TIME_BASE_Q, Timebase)) && !TimestampCadenceClock::CadenceDrifted(
                _LiveData->_RequalificationArrivalGuard.ArrivalTicks, _LiveData->_RequalificationArrivalGuard.DurationTicks);
            if (_LiveData->_TimestampRequalification.Observe(NowMs, CadenceValid,
                Valid && (Packet->flags & AV_PKT_FLAG_KEY))) {
                auto Retry = _LiveData->_TimestampRequalification;
                Retry.Recover();
                // Re-anchor at the new random-access packet, never splice clocks
                // within a GOP. This retires the pending packet and publishes a
                // discontinuity/new init without restarting the camera connection.
                ResetForReconnect(_LiveData->_InputStream);
                _LiveData->_TimestampRequalification = Retry;
                {
                    const std::lock_guard<std::mutex> Guard(*_LiveData->_SegmentsMutex);
                    _LiveData->_DiagTimestampRetryAttempts = Retry.Attempts;
                }
                LOG_INFO("[HLS] Source %d requalifying preview timestamp repair at clean keyframe (attempt %u/%u)",
                    _LiveData->_InputStream->GetSourceId(), Retry.Attempts, TimestampRequalification::MaxAttempts);
            }
        }
    }
    if (IsVideo) {
        const int64_t NowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - _LiveData->_PacketDiagEpoch).count();
        _LiveData->_TimestampRequalification.Healthy(NowMs, _LiveData->_NormalizeNoBFrameTimestamps &&
            !_LiveData->_DecodeCorruptionActive && !_LiveData->_LastTimestampCorrectionSaturated);
        const std::lock_guard<std::mutex> Guard(*_LiveData->_SegmentsMutex);
        _LiveData->_DiagTimestampRetryAttempts = _LiveData->_TimestampRequalification.Attempts;
    }
	if( !IsVideo || !_LiveData->_PendingVideoPacket )
		return WritePacketWithKnownDuration( Packet, _LiveData->_CurrentDiagnosticActivity );

	{
		const std::lock_guard<std::mutex> Guard(*_LiveData->_SegmentsMutex);
		if( _LiveData->_StartupGraceStarted.time_since_epoch().count() == 0 )
			_LiveData->_StartupGraceStarted = std::chrono::steady_clock::now();
	}

	if( _LiveData->_PendingVideoPacket->data || _LiveData->_PendingVideoPacket->size > 0 )
	{
		const int64_t SourceDuration = _LiveData->_PendingVideoPacket->duration;
		if( _LiveData->_PendingVideoPacket->dts != AV_NOPTS_VALUE && Packet->dts != AV_NOPTS_VALUE )
		{
			const int64_t Delta = Packet->dts - _LiveData->_PendingVideoPacket->dts;
			const AVRational Timebase = InputData.FormatContext->
				streams[InputData.ChosenStreamIndex]->time_base;
			const int64_t MaximumLookahead = av_rescale_q(
				2 * AV_TIME_BASE, AV_TIME_BASE_Q, Timebase );
			// The next raw DTS is useful for generic VFR streams, but it is exactly
			// the noisy signal the Reolink profile is meant to reject. Keep that
			// profile's declared/advertised cadence intact for the PLL below.
			if( !_LiveData->_AllowTimestampNormalization && !_LiveData->_NormalizeNoBFrameTimestamps &&
				Delta > 0 && Delta <= MaximumLookahead )
				_LiveData->_PendingVideoPacket->duration = Delta;
		}

		const uint64_t CurrentActivityID = _LiveData->_CurrentDiagnosticActivity;
		FFmpegLogContextScope PendingLogContext( _LiveData->_InputStream->GetSourceId(),
			"live-mux", this, _LiveData->_PendingVideoActivityID );
		CameraStreamError Result = WritePacketWithKnownDuration(
			_LiveData->_PendingVideoPacket, _LiveData->_PendingVideoActivityID, SourceDuration );
		_LiveData->_CurrentDiagnosticActivity = CurrentActivityID;
		av_packet_unref( _LiveData->_PendingVideoPacket );
		_LiveData->_PendingVideoActivityID = 0;
		if( Result != CameraStreamError::Success )
			return Result;
	}

	const int RefResult = av_packet_ref( _LiveData->_PendingVideoPacket, Packet );
	if( RefResult < 0 )
	{
		STREAM_ERROR( RefError, RefResult );
	}
	_LiveData->_PendingVideoActivityID = _LiveData->_CurrentDiagnosticActivity;
	return CameraStreamError::Success;
}

CameraStreamError LiveOutputStream::WritePacketWithKnownDuration(
	const AVPacket* Packet, uint64_t ActivityID, int64_t SourceDuration )
{
	_LiveData->_CurrentDiagnosticActivity = ActivityID;
	if (!_LiveData->_FormatContext)
	{
		CameraStreamError Result = InitFormatContext();
		if (Result != CameraStreamError::Success)
		{
			return Result;
		}
	}
	const bool IsAudio = _LiveData->_HasAudioStream && Packet->stream_index == _LiveData->_AudioInputStreamIndex;
	const bool IsVideo = Packet->stream_index == _LiveData->_InputStream->GetData().ChosenStreamIndex;
	if (!IsAudio && !IsVideo)
		return CameraStreamError::Success;
	AVRational InputTimebase =
		_LiveData->_InputStream->GetData().FormatContext->streams[Packet->stream_index]->time_base;
	const bool IsVideoKeyframe = IsVideo && (Packet->flags & AV_PKT_FLAG_KEY);
	if (IsVideo && (Packet->flags & AV_PKT_FLAG_CORRUPT))
		NotifyDecodeCorruption(0);
	const bool IsRecoveryCandidate = IsVideoKeyframe && _LiveData->_DecodeCorruptionActive &&
		_LiveData->_CurrentDiagnosticActivity > _LiveData->_DecodeCorruptionActivityID &&
		(Packet->flags & AV_PKT_FLAG_CORRUPT) == 0;
	const bool SourceHasDts = Packet->dts != AV_NOPTS_VALUE;
	const bool SourceHasPts = Packet->pts != AV_NOPTS_VALUE;
	const int64_t SourceDtsUs = SourceHasDts ?
		av_rescale_q(Packet->dts, InputTimebase, AV_TIME_BASE_Q) : 0;
	const int64_t SourcePtsUs = SourceHasPts ?
		av_rescale_q(Packet->pts, InputTimebase, AV_TIME_BASE_Q) : 0;
	const int64_t OriginalSourceDuration = SourceDuration != INT64_MIN ?
		SourceDuration : Packet->duration;
	const int64_t SourceDurationUs = OriginalSourceDuration > 0 ?
		av_rescale_q(OriginalSourceDuration, InputTimebase, AV_TIME_BASE_Q) : 0;
	const uint64_t PayloadHash = Packet->data && Packet->size > 0 ?
		HashBytes(Packet->data, (size_t)Packet->size) : Fnv1aOffsetBasis;
	const AVCodecID PacketCodec = _LiveData->_InputStream->GetData().FormatContext->
		streams[Packet->stream_index]->codecpar->codec_id;
	const PacketStructure Structure = AnalysePacketStructure(
		PacketCodec, Packet->data, Packet->size > 0 ? (size_t)Packet->size : 0);
	const auto MuxObserved = std::chrono::steady_clock::now();
	const int64_t MuxObservedSteadyUs = std::chrono::duration_cast<std::chrono::microseconds>(
		MuxObserved.time_since_epoch()).count();
	const int64_t ArrivalMs = std::chrono::duration_cast<std::chrono::milliseconds>(
		MuxObserved - _LiveData->_PacketDiagEpoch).count();
	if (IsVideo && _LiveData->_NormalizeNoBFrameTimestamps && _LiveData->_TimestampCadenceClock.UseArrival)
		_LiveData->_TimestampDeliveryEvidence.Observe(static_cast<double>(ArrivalMs), OriginalSourceDuration,
			OriginalSourceDuration > 0 ? av_q2d(InputTimebase) * OriginalSourceDuration * 1000.0 : 0,
			(Packet->flags & AV_PKT_FLAG_CORRUPT) != 0, !SourceHasDts || !SourceHasPts || Packet->pts != Packet->dts);
	const uint64_t PacketSequence = ++_LiveData->_PacketDiagSequence;
	bool DtsSynthesized = false;
	bool PtsSynthesized = false;
	bool DurationSynthesized = false;
	bool TimestampNormalized = false;
	bool TimestampRepaired = false;
	bool CorrectionSaturated = false;
	auto RecordPacket = [&](const char* Disposition, bool HasOutput = false,
		int64_t OutputDts = 0, int64_t OutputPts = 0, int64_t OutputDuration = 0,
		int SegmentIndex = -1, int PartialIndex = -1)
	{
		PacketDiagEntry Entry;
		const std::string DispositionText = Disposition ? Disposition : "unknown";
		auto Capture = ActivePacketCapture();
		// Explicit authenticated capture remains usable with passive tracing off.
		// Avoid filling/converting packet observer fields in the baseline path.
		if (_LiveData->_RetainPacketTrace || Capture)
		{
			Entry.Sequence = PacketSequence;
			Entry.Generation = _LiveData->_InitGeneration;
			Entry.SegmentIndex = SegmentIndex >= 0 ? SegmentIndex : _LiveData->_CurrentSegmentIndex;
			Entry.PartialIndex = PartialIndex >= 0 ? PartialIndex : _LiveData->_CurrentPartialIndex;
			Entry.Audio = IsAudio;
			Entry.Keyframe = IsVideoKeyframe;
			Entry.Corrupt = (Packet->flags & AV_PKT_FLAG_CORRUPT) != 0;
			Entry.DtsSynthesized = DtsSynthesized;
			Entry.PtsSynthesized = PtsSynthesized;
			Entry.DurationSynthesized = DurationSynthesized;
			Entry.TimestampNormalized = TimestampNormalized;
			Entry.TimestampRepaired = TimestampRepaired;
			Entry.CorrectionSaturated = CorrectionSaturated;
			Entry.Size = Packet->size;
			Entry.Flags = Packet->flags;
			Entry.SourceDtsUs = SourceDtsUs;
			Entry.SourcePtsUs = SourcePtsUs;
			Entry.SourceDurationUs = SourceDurationUs;
			Entry.HasSourceDts = SourceHasDts;
			Entry.HasSourcePts = SourceHasPts;
			Entry.HasOutputDts = HasOutput;
			Entry.HasOutputPts = HasOutput;
			if (HasOutput)
			{
				Entry.OutputDtsUs = av_rescale_q(OutputDts, InputTimebase, AV_TIME_BASE_Q);
				Entry.OutputPtsUs = av_rescale_q(OutputPts, InputTimebase, AV_TIME_BASE_Q);
				Entry.OutputDurationUs = av_rescale_q(OutputDuration, InputTimebase, AV_TIME_BASE_Q);
			}
			Entry.PayloadHash = PayloadHash;
			Entry.ArrivalMs = ArrivalMs;
			Entry.Packetization = Structure.Packetization;
			Entry.CodecUnitCount = Structure.UnitCount;
			Entry.PrimaryCodecUnitType = Structure.PrimaryUnitType;
			Entry.PayloadPrefixLength = (std::min)(
				(std::max)(Packet->size, 0), (int)sizeof(Entry.PayloadPrefix));
			if (Packet->data && Entry.PayloadPrefixLength > 0)
				memcpy(Entry.PayloadPrefix, Packet->data, Entry.PayloadPrefixLength);
			Entry.Disposition = DispositionText;
			if (Capture)
				Capture->AddMetadata(std::format(
					"{{\"type\":\"decision\",\"activityId\":{},\"packetSequence\":{},\"generation\":{},\"segment\":{},\"part\":{},\"audio\":{},\"disposition\":\"{}\",\"hashFnv64\":\"{:016x}\",\"sourceDtsUs\":{},\"sourcePtsUs\":{},\"sourceDurationUs\":{},\"hasOutput\":{},\"outputDtsUs\":{},\"outputPtsUs\":{},\"outputDurationUs\":{},\"normalized\":{},\"timestampRepaired\":{},\"muxObservedSteadyUs\":{},\"decisionSteadyUs\":{}",
					_LiveData->_CurrentDiagnosticActivity, PacketSequence, Entry.Generation,
					Entry.SegmentIndex, Entry.PartialIndex, Entry.Audio, DispositionText,
					Entry.PayloadHash, Entry.SourceDtsUs, Entry.SourcePtsUs,
					Entry.SourceDurationUs, HasOutput, Entry.OutputDtsUs,
					Entry.OutputPtsUs, Entry.OutputDurationUs,
					Entry.TimestampNormalized, Entry.TimestampRepaired, MuxObservedSteadyUs,
					std::chrono::duration_cast<std::chrono::microseconds>(
						std::chrono::steady_clock::now().time_since_epoch()).count()));
		}
		const std::lock_guard<std::mutex> guard(*_LiveData->_SegmentsMutex);
		_LiveData->_RecentWrittenVideoPackets.Record(!IsAudio && DispositionText == "written", PayloadHash);
		if( DispositionText == "waitingForKeyframe" ) ++_LiveData->_DiagWaitingForKeyframePackets;
		else if( DispositionText == "missingTimestamp" ) ++_LiveData->_DiagMissingTimestampPackets;
		else if( DispositionText == "beforeVideoEpoch" ) ++_LiveData->_DiagBeforeVideoEpochPackets;
		else if( DispositionText == "negativeTimestamp" ) ++_LiveData->_DiagNegativeTimestampPackets;
		else if( DispositionText == "nonMonotonicInput" ) ++_LiveData->_DiagNonMonotonicInputPackets;
		else if( DispositionText == "noMuxBuffer" ) ++_LiveData->_DiagNoMuxBufferPackets;
		else if( DispositionText == "nonMonotonicOutput" ) ++_LiveData->_DiagNonMonotonicOutputPackets;
		else if( DispositionText == "muxError" ) ++_LiveData->_DiagMuxErrorPackets;
		// Keep functional evidence and cheap disposition totals in both phases.
		// Only the optional packet ring and derived packet-detail events stop here.
		if (!_LiveData->_RetainPacketTrace) return;
		_LiveData->_PacketDiagRing[_LiveData->_PacketDiagRingPos % _LiveData->PACKET_DIAG_RING_SIZE] = std::move(Entry);
		++_LiveData->_PacketDiagRingPos;
		if (_LiveData->_PacketDiagRingCount < _LiveData->PACKET_DIAG_RING_SIZE)
			++_LiveData->_PacketDiagRingCount;

		const bool Noteworthy = DispositionText != "written" &&
			DispositionText != "waitingForKeyframe";
		if( Noteworthy )
		{
			MediaDiagnosticEvent Event;
			Event.Sequence = ++_LiveData->_MediaEventSequence;
			Event.ActivityID = _LiveData->_CurrentDiagnosticActivity;
			Event.PacketSequence = PacketSequence;
			Event.TimestampUnixMs = std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::system_clock::now().time_since_epoch()).count();
			Event.ElapsedMs = ArrivalMs;
			Event.Generation = _LiveData->_InitGeneration;
			Event.SegmentIndex = SegmentIndex >= 0 ? SegmentIndex : _LiveData->_CurrentSegmentIndex;
			Event.PartialIndex = PartialIndex >= 0 ? PartialIndex : _LiveData->_CurrentPartialIndex;
			Event.Category = "packet";
			Event.Severity = DispositionText == "muxError" ? "error" : "warning";
			Event.Phase = "live-mux";
			Event.Component = IsAudio ? "audio-packet" : "video-packet";
			Event.Message = "Packet disposition: " + DispositionText;
			Event.Disposition = DispositionText;
			Event.Audio = IsAudio;
			Event.Keyframe = IsVideoKeyframe;
			Event.Corrupt = (Packet->flags & AV_PKT_FLAG_CORRUPT) != 0;
			Event.PacketSize = Packet->size;
			Event.SourceDtsUs = SourceDtsUs;
			Event.SourcePtsUs = SourcePtsUs;
			Event.HasSourceDts = SourceHasDts;
			Event.HasSourcePts = SourceHasPts;
			_LiveData->_MediaEventRing[_LiveData->_MediaEventRingPos % _LiveData->MEDIA_EVENT_RING_SIZE] = std::move( Event );
			++_LiveData->_MediaEventRingPos;
			if( _LiveData->_MediaEventRingCount < _LiveData->MEDIA_EVENT_RING_SIZE )
				++_LiveData->_MediaEventRingCount;
		}
	};
	// Never mutate segment state around a keyframe that cannot be placed on the
	// decode timeline. Continue waiting for the next usable random-access point.
	if (IsVideo && Packet->dts == AV_NOPTS_VALUE)
	{
		{
			const std::lock_guard<std::mutex> guard(*_LiveData->_SegmentsMutex);
			++_LiveData->_DiagMissingVideoDtsPackets;
			++_LiveData->_DiagDroppedVideoPackets;
		}
		++_LiveData->_SegmentMissingVideoDtsPackets;
		++_LiveData->_SegmentDroppedVideoPackets;
		RecordPacket("missingVideoDts");
		NotifyDecodeCorruption(0);
		return CameraStreamError::Success;
	}

	// Until the first usable random-access frame, ignore both tracks. This keeps
	// the initial timestamp anchored to video and preserves the old join behavior.
	if (!_LiveData->_HeaderWritten && !IsVideoKeyframe)
	{
		RecordPacket("waitingForKeyframe");
		return CameraStreamError::Success;
	}

	AVPacket PacketCopy;
	memset(&PacketCopy, 0, sizeof(PacketCopy));
	int Result = av_packet_ref(&PacketCopy, Packet);
	if (Result < 0)
	{
		STREAM_ERROR(RefError, Result);
	}

	if (PacketCopy.dts == AV_NOPTS_VALUE)
	{
		// AAC from some RTSP cameras carries only PTS. A missing timestamp on
		// one optional audio packet must not tear down the whole camera session.
		if (!IsAudio || PacketCopy.pts == AV_NOPTS_VALUE)
		{
			RecordPacket("missingTimestamp");
			av_packet_unref(&PacketCopy);
			return CameraStreamError::Success;
		}
		PacketCopy.dts = PacketCopy.pts;
		DtsSynthesized = true;
	}
	if (PacketCopy.pts == AV_NOPTS_VALUE)
	{
		PacketCopy.pts = PacketCopy.dts;
		PtsSynthesized = true;
	}

	int64_t PacketTimestampUs = av_rescale_q(PacketCopy.dts, InputTimebase, AV_TIME_BASE_Q);
	if (_LiveData->_InitialTimestampUs == AV_NOPTS_VALUE && IsVideo)
		_LiveData->_InitialTimestampUs = PacketTimestampUs;
	// A qualified preview may see a small backward clock step cross its new
	// source epoch immediately after recovery. Preserve the reference picture;
	// the independent output clock will place it safely. Unqualified/audio/
	// B-frame streams and unbounded source resets retain their existing guards.
	const bool BoundedArrivalEpochRegression = IsVideo && _LiveData->_NormalizeNoBFrameTimestamps &&
		_LiveData->_TimestampCadenceClock.UseArrival && !_LiveData->_TimestampNormalizationRejected &&
		!_LiveData->_HasAudioStream && !_LiveData->_HasBFrames && _LiveData->_LastWrittenDTS != AV_NOPTS_VALUE &&
		PacketCopy.pts == PacketCopy.dts && !(PacketCopy.flags & AV_PKT_FLAG_CORRUPT) &&
		_LiveData->_InitialTimestampUs != AV_NOPTS_VALUE && PacketTimestampUs < _LiveData->_InitialTimestampUs &&
		SourceDurationUs > 0 && SourceDurationUs <= AV_TIME_BASE &&
		TimestampArrivalGuard::SameDeclaration(OriginalSourceDuration, _LiveData->_LastPacketDuration) &&
		static_cast<long double>(_LiveData->_InitialTimestampUs) - PacketTimestampUs <=
			(std::min<int64_t>)(AV_TIME_BASE, SourceDurationUs * 20);
	if (_LiveData->_InitialTimestampUs == AV_NOPTS_VALUE ||
		(PacketTimestampUs < _LiveData->_InitialTimestampUs && !BoundedArrivalEpochRegression))
	{
		if (IsVideo)
		{
			const std::lock_guard<std::mutex> guard(*_LiveData->_SegmentsMutex);
			++_LiveData->_DiagDroppedVideoPackets;
			++_LiveData->_SegmentDroppedVideoPackets;
		}
		RecordPacket("beforeVideoEpoch");
		av_packet_unref(&PacketCopy);
		return CameraStreamError::Success;
	}
	PacketCopy.dts = av_rescale_q(PacketTimestampUs - _LiveData->_InitialTimestampUs, AV_TIME_BASE_Q, InputTimebase);
	PacketCopy.pts = av_rescale_q(
		av_rescale_q(PacketCopy.pts, InputTimebase, AV_TIME_BASE_Q) - _LiveData->_InitialTimestampUs,
		AV_TIME_BASE_Q, InputTimebase);

	// Guard against negative timestamps from B-frame reordering at stream start
	if (PacketCopy.dts != AV_NOPTS_VALUE && PacketCopy.dts < 0 && !BoundedArrivalEpochRegression)
	{
		if (IsVideo)
		{
			const std::lock_guard<std::mutex> guard(*_LiveData->_SegmentsMutex);
			++_LiveData->_DiagDroppedVideoPackets;
			++_LiveData->_SegmentDroppedVideoPackets;
		}
		RecordPacket("negativeTimestamp");
		av_packet_unref(&PacketCopy);
		return CameraStreamError::Success;
	}

	// Clamp invalid durations before they participate in timestamp repair.
	if (PacketCopy.duration < 0 || PacketCopy.duration > INT_MAX)
	{
		PacketCopy.duration = 0;
		DurationSynthesized = true;
	}
	if( IsVideo && !_LiveData->_HasBFrames && _LiveData->_AllowTimestampNormalization )
	{
		const AVRational Framerate = _LiveData->_InputStream->GetData().CodecContext->framerate;
		if( Framerate.num > 0 && Framerate.den > 0 )
		{
			const double FramesPerSecond = av_q2d(Framerate);
			if( FramesPerSecond >= 1.0 && FramesPerSecond <= 120.0 )
			{
				const int64_t AdvertisedDuration = av_rescale_q(
					1, av_inv_q(Framerate), InputTimebase);
				if( AdvertisedDuration > 0 && (PacketCopy.duration <= 0 ||
					PacketCopy.duration * 2 < AdvertisedDuration ||
					PacketCopy.duration > AdvertisedDuration * 2) )
				{
					PacketCopy.duration = AdvertisedDuration;
					DurationSynthesized = true;
				}
			}
		}
	}

	// Reject duplicate or out-of-order input packets unless this source has
	// already qualified for duration-derived timestamp repair. In that case the
	// packets are distinct access units in arrival order; discarding one can lose
	// an HEVC reference picture and visibly corrupt dependent frames.
	bool RepairedRegressingVideoTimestamp = false;
	bool StartedArrivalThisPacket = false;
	// Keep declared-duration evidence separate from source-DTS lookahead. Sample
	// every eligible packet, including timestamp drops, so the raw high-water
	// mark cannot distort the measured arrival cadence.
	if (IsVideo && _LiveData->_AllowObservedTimestampRegressionRepair && !_LiveData->_AllowTimestampNormalization &&
		!_LiveData->_HasAudioStream && !_LiveData->_HasBFrames && !_LiveData->_NormalizeNoBFrameTimestamps &&
		!_LiveData->_TimestampNormalizationRejected &&
		(PacketCodec == AV_CODEC_ID_H264 || PacketCodec == AV_CODEC_ID_HEVC))
	{
		_LiveData->_TimestampArrivalGuard.Observe(av_rescale_q(
			ArrivalMs, AVRational{1, 1000}, InputTimebase), OriginalSourceDuration,
			PacketCopy.pts == PacketCopy.dts && (PacketCopy.flags & AV_PKT_FLAG_CORRUPT) == 0);
	}
	// Also sample the relationship between source DTS deltas
	// and declared frame durations. We only replace the source clock when it is
	// demonstrably jittery but agrees with the durations over the whole window;
	// genuine variable-frame-rate streams therefore retain their source timing.
	if (IsVideo && PacketCopy.dts != AV_NOPTS_VALUE)
	{
		if (_LiveData->_LastInputDTS != AV_NOPTS_VALUE && PacketCopy.dts <= _LiveData->_LastInputDTS)
		{
			bool PayloadSeenRecently = false;
			{
				const std::lock_guard<std::mutex> Guard(*_LiveData->_SegmentsMutex);
				PayloadSeenRecently = _LiveData->_RecentWrittenVideoPackets.Contains(PayloadHash);
			}
			if (!_LiveData->_HasBFrames && _LiveData->_AllowObservedTimestampRegressionRepair &&
				!_LiveData->_TimestampNormalizationRejected && !_LiveData->_NormalizeNoBFrameTimestamps &&
				(PacketCodec == AV_CODEC_ID_H264 || PacketCodec == AV_CODEC_ID_HEVC))
			{
				TimestampRegressionGuard RegressionGuard;
				RegressionGuard.StableSamples = _LiveData->_TimestampStableCadenceSamples;
				RegressionGuard.StableDurationTicks = _LiveData->_TimestampStableCadenceTicks;
				const int64_t MinimumEvidenceTicks = av_rescale_q(
					AV_TIME_BASE / 4, AV_TIME_BASE_Q, InputTimebase);
				const int64_t MaximumTimeRegressionTicks = av_rescale_q(
					AV_TIME_BASE, AV_TIME_BASE_Q, InputTimebase);
				const int64_t MaximumFrameRegressionTicks =
					PacketCopy.duration > 0 &&
					PacketCopy.duration <= (std::numeric_limits<int64_t>::max)() / 20 ?
					PacketCopy.duration * 20 : MaximumTimeRegressionTicks;
				const int64_t MaximumRegressionTicks = (std::min)(
					MaximumTimeRegressionTicks, MaximumFrameRegressionTicks);
				const bool SourceEvidenceQualifies = RegressionGuard.CanRepair(PacketCopy.dts, PacketCopy.pts,
					_LiveData->_LastInputDTS, _LiveData->_LastPacketDuration, PacketCopy.duration,
					PayloadSeenRecently, MinimumEvidenceTicks,
					MaximumRegressionTicks);
				const int64_t MinimumArrivalEvidenceTicks = av_rescale_q(
					AV_TIME_BASE, AV_TIME_BASE_Q, InputTimebase);
				TimestampRegressionGuard ArrivalEvidence;
				ArrivalEvidence.StableSamples = _LiveData->_TimestampArrivalGuard.SampleCount;
				ArrivalEvidence.StableDurationTicks = _LiveData->_TimestampArrivalGuard.DurationTicks;
				const int64_t DeclaredDuration = _LiveData->_TimestampArrivalGuard.DeclaredDuration;
				const bool ArrivalEvidenceQualifies = _LiveData->_TimestampArrivalGuard.Ready(MinimumArrivalEvidenceTicks) &&
					ArrivalEvidence.CanRepair(PacketCopy.dts, PacketCopy.pts, _LiveData->_LastInputDTS,
						DeclaredDuration, OriginalSourceDuration, PayloadSeenRecently,
						MinimumArrivalEvidenceTicks, (std::min)(MaximumTimeRegressionTicks, DeclaredDuration * 20));
				if (SourceEvidenceQualifies || ArrivalEvidenceQualifies)
				{
					_LiveData->_NormalizeNoBFrameTimestamps = true;
					_LiveData->_SourceTimestampOffset = 0;
					if (ArrivalEvidenceQualifies)
						PacketCopy.duration = OriginalSourceDuration;
					// A preview without audio needs no source-clock synchronization.
					// Its regressing clock must not also be the repair's reference.
					if (!_LiveData->_AllowTimestampNormalization && !_LiveData->_HasAudioStream)
					{
						_LiveData->_TimestampCadenceClock.StartArrival(av_rescale_q(
							ArrivalMs, AVRational{1, 1000}, InputTimebase),
							_LiveData->_LastWrittenDTS + _LiveData->_LastOutputPacketDuration);
						_LiveData->_TimestampDeliveryEvidence = {};
						StartedArrivalThisPacket = true;
					}
					_LiveData->_TimestampProbeSamples = 0;
					_LiveData->_TimestampProbeOutliers = 0;
					_LiveData->_TimestampProbeInputTicks = 0;
					_LiveData->_TimestampProbeDurationTicks = 0;
					_LiveData->_TimestampStableCadenceSamples = 0;
					_LiveData->_TimestampStableCadenceTicks = 0;
					const double RegressionMs =
						(double)(_LiveData->_LastInputDTS - PacketCopy.dts) *
						InputTimebase.num * 1000.0 / InputTimebase.den;
					LOG_INFO(
						"[HLS] Source %d enabled bounded no-B-frame timestamp repair after %.1fms DTS regression (%s clock, %s evidence)",
						_LiveData->_InputStream->GetSourceId(), RegressionMs,
						_LiveData->_TimestampCadenceClock.UseArrival ? "arrival" : "source",
						ArrivalEvidenceQualifies ? "declared/arrival" : "source-DTS");
				}
			}

			const bool CanRepairRegression =
				!_LiveData->_HasBFrames &&
				(_LiveData->_AllowTimestampNormalization || _LiveData->_AllowObservedTimestampRegressionRepair) &&
				!_LiveData->_TimestampNormalizationRejected && _LiveData->_NormalizeNoBFrameTimestamps &&
				_LiveData->_LastWrittenDTS != AV_NOPTS_VALUE && _LiveData->_LastPacketDuration > 0;
			if (!CanRepairRegression)
			{
				uint64_t DroppedVideoPackets;
				{
					const std::lock_guard<std::mutex> guard(*_LiveData->_SegmentsMutex);
					DroppedVideoPackets = ++_LiveData->_DiagDroppedVideoPackets;
				}
				++_LiveData->_SegmentDroppedVideoPackets;
				if (DroppedVideoPackets <= 5 || (DroppedVideoPackets % 100) == 0)
				{
					LOG_WARNING(
						"[HLS] Source %d dropped non-monotonic video packet: previous DTS=%lld, DTS=%lld, PTS=%lld, duration=%lld, flags=0x%x, size=%d, normalizing=%d",
						_LiveData->_InputStream->GetSourceId(), (long long)_LiveData->_LastInputDTS,
						(long long)PacketCopy.dts, (long long)PacketCopy.pts,
						(long long)PacketCopy.duration, PacketCopy.flags, PacketCopy.size,
						_LiveData->_NormalizeNoBFrameTimestamps ? 1 : 0);
				}
				RecordPacket("nonMonotonicInput");
				NotifyDecodeCorruption(0);
				av_packet_unref(&PacketCopy);
				return CameraStreamError::Success;
			}

			RepairedRegressingVideoTimestamp = true;
			TimestampRepaired = true;
			uint64_t RepairedVideoTimestamps;
			{
				const std::lock_guard<std::mutex> guard(*_LiveData->_SegmentsMutex);
				RepairedVideoTimestamps = ++_LiveData->_DiagRepairedVideoTimestamps;
			}
			if (RepairedVideoTimestamps <= 5 || (RepairedVideoTimestamps % 100) == 0)
			{
				LOG_WARNING(
					"[HLS] Source %d repaired non-monotonic video timestamp: previous DTS=%lld, DTS=%lld, PTS=%lld, duration=%lld, flags=0x%x, size=%d",
					_LiveData->_InputStream->GetSourceId(), (long long)_LiveData->_LastInputDTS,
					(long long)PacketCopy.dts, (long long)PacketCopy.pts,
					(long long)PacketCopy.duration, PacketCopy.flags, PacketCopy.size);
			}
		}

		if (!_LiveData->_NormalizeNoBFrameTimestamps && _LiveData->_AllowObservedTimestampRegressionRepair &&
			!_LiveData->_TimestampNormalizationRejected && !_LiveData->_HasBFrames &&
			_LiveData->_LastInputDTS != AV_NOPTS_VALUE && PacketCopy.dts > _LiveData->_LastInputDTS)
		{
			TimestampRegressionGuard RegressionGuard;
			RegressionGuard.StableSamples = _LiveData->_TimestampStableCadenceSamples;
			RegressionGuard.StableDurationTicks = _LiveData->_TimestampStableCadenceTicks;
			RegressionGuard.ObserveMonotonic(PacketCopy.dts - _LiveData->_LastInputDTS,
				_LiveData->_LastPacketDuration, PacketCopy.duration,
				PacketCopy.pts == PacketCopy.dts);
			_LiveData->_TimestampStableCadenceSamples = RegressionGuard.StableSamples;
			_LiveData->_TimestampStableCadenceTicks = RegressionGuard.StableDurationTicks;
		}

		const bool ProbeTimestampNormalization = _LiveData->_AllowTimestampNormalization ||
			(_LiveData->_AllowObservedTimestampRegressionRepair && _LiveData->_NormalizeNoBFrameTimestamps);
		if (!_LiveData->_HasBFrames && ProbeTimestampNormalization && !StartedArrivalThisPacket && !_LiveData->_TimestampNormalizationRejected &&
			_LiveData->_LastInputDTS != AV_NOPTS_VALUE && _LiveData->_LastPacketDuration > 0)
		{
			const int64_t InputDelta = PacketCopy.dts - _LiveData->_LastInputDTS;
			int64_t DeltaError = InputDelta - _LiveData->_LastPacketDuration;
			if (DeltaError < 0)
				DeltaError = -DeltaError;

			++_LiveData->_TimestampProbeSamples;
			_LiveData->_TimestampProbeInputTicks += InputDelta;
			_LiveData->_TimestampProbeDurationTicks += _LiveData->_LastPacketDuration;
			if (DeltaError * 2 > _LiveData->_LastPacketDuration)
				++_LiveData->_TimestampProbeOutliers;
			if (!_LiveData->_NormalizeNoBFrameTimestamps && _LiveData->_TimestampProbeSamples >= 20)
			{
				int64_t TotalError = _LiveData->_TimestampProbeInputTicks - _LiveData->_TimestampProbeDurationTicks;
				if (TotalError < 0)
					TotalError = -TotalError;

				const bool AverageCadenceMatches =
					TotalError * 100 <= _LiveData->_TimestampProbeDurationTicks * 15;
				const bool SourceClockIsJittery =
					_LiveData->_TimestampProbeOutliers * 5 >= _LiveData->_TimestampProbeSamples;
				if (AverageCadenceMatches && SourceClockIsJittery)
				{
					_LiveData->_NormalizeNoBFrameTimestamps = true;
					_LiveData->_SourceTimestampOffset = 0;
					LOG_INFO("[HLS] Normalizing jittery no-B-frame timestamps (%d/%d outliers)",
						_LiveData->_TimestampProbeOutliers, _LiveData->_TimestampProbeSamples);
					_LiveData->_TimestampProbeSamples = 0;
					_LiveData->_TimestampProbeOutliers = 0;
					_LiveData->_TimestampProbeInputTicks = 0;
					_LiveData->_TimestampProbeDurationTicks = 0;
				}
				else if (_LiveData->_TimestampProbeSamples >= 120)
				{
					_LiveData->_TimestampProbeSamples = 0;
					_LiveData->_TimestampProbeOutliers = 0;
					_LiveData->_TimestampProbeInputTicks = 0;
					_LiveData->_TimestampProbeDurationTicks = 0;
				}
			}
			else if (_LiveData->_NormalizeNoBFrameTimestamps)
			{
				// Reolink DTS can wander by several seconds and then converge again.
				// Judge cadence over a long interval rather than using a fixed
				// phase-error budget, which incorrectly rejected camera 11.
				const int64_t ProbeDurationUs = av_rescale_q(
					_LiveData->_TimestampProbeDurationTicks, InputTimebase, AV_TIME_BASE_Q);
				const bool ArrivalRateEligible = _LiveData->_TimestampCadenceClock.UseArrival &&
					_LiveData->_AllowObservedTimestampRegressionRepair && !_LiveData->_AllowTimestampNormalization &&
					!_LiveData->_HasAudioStream;
				const int64_t ValidationWindowUs = (ArrivalRateEligible ? 20LL : 120LL) * AV_TIME_BASE;
				const int64_t ArrivalTicks = av_rescale_q(
					ArrivalMs, AVRational{1, 1000}, InputTimebase);
				const int64_t ReferenceTicks = _LiveData->_TimestampCadenceClock.WindowElapsed(
					_LiveData->_TimestampProbeInputTicks, ArrivalTicks);
				const int64_t ReferenceUs = av_rescale_q(ReferenceTicks, InputTimebase, AV_TIME_BASE_Q);
				if ((ArrivalRateEligible ? ReferenceUs : ProbeDurationUs) >= ValidationWindowUs)
				{
					const int64_t SignedError = ReferenceTicks - _LiveData->_TimestampProbeDurationTicks;
					const bool WindowCadenceDrifted =
						TimestampCadenceClock::CadenceDrifted(ReferenceTicks, _LiveData->_TimestampProbeDurationTicks);
					const bool BoundedArrivalRate = ArrivalRateEligible &&
						!_LiveData->_TimestampPauseRecovery.Pending() &&
						_LiveData->_TimestampCadenceClock.LearnArrivalDuration(ReferenceTicks,
							_LiveData->_TimestampProbeDurationTicks, _LiveData->_TimestampProbeSamples,
							av_rescale_q(20 * AV_TIME_BASE, AV_TIME_BASE_Q, InputTimebase),
							_LiveData->_TimestampDeliveryEvidence);
					if (BoundedArrivalRate) {
						const std::lock_guard<std::mutex> Guard(*_LiveData->_SegmentsMutex);
						_LiveData->_DiagTimestampArrivalRateAdjustments = _LiveData->_TimestampCadenceClock.RateAdjustments;
						_LiveData->_DiagTimestampArrivalDurationMs =
							av_q2d(InputTimebase) * _LiveData->_TimestampCadenceClock.LearnedDurationTicks * 1000.0;
					}
					const double DriftMs =
						(double)SignedError * InputTimebase.num * 1000.0 / InputTimebase.den;

					// A buffered pause can straddle the validation boundary before
					// its burst drains. Retain the qualified repair until fresh
					// cadence/keyframe recovery, bounded to 30 seconds by the policy.
					if (WindowCadenceDrifted && !BoundedArrivalRate &&
						!(_LiveData->_TimestampCadenceClock.UseArrival && _LiveData->_TimestampPauseRecovery.Pending()))
					{
						_LiveData->_NormalizeNoBFrameTimestamps = false;
						_LiveData->_TimestampNormalizationRejected = true;
                        _LiveData->_TimestampRequalification.Reject(ArrivalMs);
                        _LiveData->_RequalificationCollecting = false;
                        {
                            const std::lock_guard<std::mutex> Guard(*_LiveData->_SegmentsMutex);
                            _LiveData->_DiagTimestampRejected = true;
                            _LiveData->_DiagTimestampRetryAttempts = _LiveData->_TimestampRequalification.Attempts;
                            _LiveData->_DiagTimestampRejectedUnixMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now().time_since_epoch()).count();
                            _LiveData->_DiagTimestampDeclaredWindowMs = ProbeDurationUs / 1000.0;
                            _LiveData->_DiagTimestampReferenceWindowMs = av_q2d(InputTimebase) * ReferenceTicks * 1000.0;
                            _LiveData->_DiagTimestampRejectionUsesArrival = _LiveData->_TimestampCadenceClock.UseArrival;
                            _LiveData->_DiagTimestampDeliveryEvidence = _LiveData->_TimestampDeliveryEvidence;
                        }
						_LiveData->_SourceTimestampOffset =
							(_LiveData->_LastWrittenDTS + _LiveData->_LastOutputPacketDuration) - PacketCopy.dts;
						LOG_WARNING(
							"[HLS] Source %d stopped timestamp normalization: %.1fms drift over %.1fs (%d samples, %s clock)",
							_LiveData->_InputStream->GetSourceId(), DriftMs, ProbeDurationUs / 1000000.0,
							_LiveData->_TimestampProbeSamples, _LiveData->_TimestampCadenceClock.UseArrival ? "arrival" : "source");
					}

					_LiveData->_TimestampProbeSamples = 0;
					_LiveData->_TimestampProbeOutliers = 0;
					_LiveData->_TimestampProbeInputTicks = 0;
					_LiveData->_TimestampProbeDurationTicks = 0;
					_LiveData->_TimestampCadenceClock.NextWindow(ArrivalTicks);
					_LiveData->_TimestampDeliveryEvidence.NextWindow();
				}
			}
		}

		_LiveData->_LastInputDTS = PacketCopy.dts;
	}

	// Segment state must only change after the keyframe has passed timestamp
	// acceptance. Otherwise a rejected keyframe can advertise dependent video as
	// an independent fragment.
	if (IsVideoKeyframe)
	{
		// Enforce a minimum segment duration of 1 second. Cameras like Tapo send
		// keyframes every ~50-100ms, which would create unusable micro-segments.
		const bool ShouldSplit = !_LiveData->_HeaderWritten || _LiveData->_CurrentSegmentDuration >= 1.0;
		// A recovery random-access point must be the first video sample in the
		// partial that releases presentation. Do not label a mixed partial as
		// independent merely because it contains a later keyframe.
		if( IsRecoveryCandidate && _LiveData->_HeaderWritten && !ShouldSplit &&
			_LiveData->_CurrentPartialHasPacket )
		{
			if( !FlushPartialSegment( _LiveData->_CurrentPartialIsIndependent ) )
			{
				av_packet_unref(&PacketCopy);
				return CameraStreamError::WriteFailed;
			}
		}
		if (ShouldSplit)
		{
			if (_LiveData->_HeaderWritten)
			{
				CameraStreamError FinishResult = FinishCurrentSegment(PacketCopy.dts);
				if( FinishResult != CameraStreamError::Success )
				{
					av_packet_unref(&PacketCopy);
					return FinishResult;
				}
			}

			CameraStreamError StartResult = StartNewSegment(&PacketCopy);
			if (StartResult != CameraStreamError::Success)
			{
				av_packet_unref(&PacketCopy);
				return StartResult;
			}
		}
	}

	if (!_LiveData->_HeaderWritten || !_LiveData->_CurrentBuffer)
	{
		RecordPacket("noMuxBuffer");
		av_packet_unref(&PacketCopy);
		return CameraStreamError::Success;
	}
	if (RepairedRegressingVideoTimestamp)
		++_LiveData->_SegmentRepairedVideoTimestamps;

	// A missing duration must not switch a normalized stream back to its raw
	// timestamp domain for one packet. Reuse the last validated duration.
	if (IsVideo && _LiveData->_NormalizeNoBFrameTimestamps && PacketCopy.duration == 0 && _LiveData->_LastPacketDuration > 0)
	{
		PacketCopy.duration = _LiveData->_LastPacketDuration;
		DurationSynthesized = true;
	}
	const int64_t DeclaredVideoDuration = IsVideo ? PacketCopy.duration : 0;
	const int64_t NominalVideoDuration = IsVideo && _LiveData->_NormalizeNoBFrameTimestamps &&
		_LiveData->_TimestampCadenceClock.UseArrival && _LiveData->_TimestampCadenceClock.LearnedDurationTicks > 0 ?
		_LiveData->_TimestampCadenceClock.LearnedDurationTicks : DeclaredVideoDuration;
	const int64_t SourceNominalVideoDuration = IsVideo && OriginalSourceDuration > 0 &&
		OriginalSourceDuration <= INT_MAX ?
		OriginalSourceDuration : DeclaredVideoDuration;

	// Once the Reolink source has met the guarded jitter test, build a continuous
	// output clock from declared durations, then gently steer it towards the raw
	// source clock shared with audio (or steady arrival time for qualified
	// video-only previews). A duration-only clock runs at a measurably
	// different rate on some cameras, eventually placing AAC seconds ahead of
	// video. The ten-second response window rejects short DTS wander while the 5%
	// limit keeps every output interval positive and close to the nominal cadence.
	// Otherwise preserve source deltas, applying an offset only when transitioning
	// out of normalized mode.
	if (IsVideo && !_LiveData->_HasBFrames)
	{
		_LiveData->_LastTimestampPhaseError = 0;
		_LiveData->_LastTimestampCorrection = 0;
		_LiveData->_LastTimestampCorrectionSaturated = false;
		if (_LiveData->_NormalizeNoBFrameTimestamps && _LiveData->_LastWrittenDTS != AV_NOPTS_VALUE &&
			_LiveData->_LastPacketDuration > 0 && _LiveData->_LastOutputPacketDuration > 0)
		{
			const int64_t RawDTS = PacketCopy.dts;
			// The previous packet advertised this duration, so using it here keeps
			// FFmpeg's fragment boundary rewrite exactly on the same timeline.
			const int64_t ExpectedDTS = _LiveData->_LastWrittenDTS + _LiveData->_LastOutputPacketDuration;
			const int64_t TargetDTS = _LiveData->_TimestampCadenceClock.Target(
				RawDTS + _LiveData->_SourceTimestampOffset,
				av_rescale_q(ArrivalMs, AVRational{1, 1000}, InputTimebase));
			const int64_t PhaseError = TargetDTS - ExpectedDTS;
			const int64_t ResponseWindowTicks = av_rescale_q(
				10 * AV_TIME_BASE, AV_TIME_BASE_Q, InputTimebase);
			const int64_t MaxCorrection = (std::min)(
				NominalVideoDuration / 20, (int64_t)INT_MAX - NominalVideoDuration);
			int64_t Correction = 0;
			if (ResponseWindowTicks > 0)
			{
				// Anything beyond one second of phase error is already far into the
				// 5% clamp. Bound before multiplying to avoid signed overflow on fine
				// timebases or corrupt source timestamps.
				const int64_t PhaseLimit = (std::max<int64_t>)(1, ResponseWindowTicks / 10);
				const int64_t BoundedPhaseError = (std::max)(-PhaseLimit,
					(std::min)(PhaseError, PhaseLimit));
				const int64_t Numerator =
					BoundedPhaseError * NominalVideoDuration + _LiveData->_TimestampCorrectionRemainder;
				Correction = Numerator / ResponseWindowTicks;
				_LiveData->_TimestampCorrectionRemainder = Numerator % ResponseWindowTicks;
			}
			const int64_t UnclampedCorrection = Correction;
			Correction = (std::max)(-MaxCorrection, (std::min)(Correction, MaxCorrection));
			PacketCopy.dts = ExpectedDTS;
			PacketCopy.duration = NominalVideoDuration + Correction;
			_LiveData->_LastTimestampPhaseError = PhaseError;
			_LiveData->_LastTimestampCorrection = Correction;
			_LiveData->_LastTimestampCorrectionSaturated = Correction != UnclampedCorrection ||
				PhaseError > ResponseWindowTicks / 10 || PhaseError < -ResponseWindowTicks / 10;
			TimestampNormalized = true;
			CorrectionSaturated = _LiveData->_LastTimestampCorrectionSaturated;
		}
		else if (_LiveData->_SourceTimestampOffset != 0 && PacketCopy.dts != AV_NOPTS_VALUE)
			PacketCopy.dts += _LiveData->_SourceTimestampOffset;
		PacketCopy.pts = PacketCopy.dts;
	}

	PacketCopy.stream_index = IsAudio ? 1 : 0;
	PacketCopy.pos = -1;
	if (IsAudio && _LiveData->_AacFrameSamples > 0)
	{
		const int SampleRate = _LiveData->_FormatContext->streams[1]->codecpar->sample_rate;
		const int64_t SourceSample = av_rescale_q(PacketCopy.dts, InputTimebase, AVRational{1, SampleRate});
		int64_t OutputSample = 0;
		if (!AudioSampleClock::Normalize(SourceSample, _LiveData->_AacFrameSamples, _LiveData->_NextAacSample, OutputSample))
		{
			RecordPacket("nonMonotonicOutput", true,
				av_rescale_q(_LiveData->_NextAacSample, AVRational{1, SampleRate}, InputTimebase),
				av_rescale_q(_LiveData->_NextAacSample, AVRational{1, SampleRate}, InputTimebase), _LiveData->_AacFrameSamples);
			av_packet_unref(&PacketCopy);
			return CameraStreamError::Success;
		}
		// RTSP AAC DTS can alternate ~50ms / ~100ms / near-zero despite each
		// access unit decoding to 1024 samples (64ms at 16kHz). MP4 compares the
		// next fragment's DTS with the previous sample's end, not just its DTS.
		// Use sample-exact timestamps; do not let movenc clamp overlap to one tick.
		const int64_t EndSample = OutputSample + _LiveData->_AacFrameSamples;
		PacketCopy.dts = av_rescale_q(OutputSample, AVRational{1, SampleRate}, InputTimebase);
		PacketCopy.pts = PacketCopy.dts;
		PacketCopy.duration = av_rescale_q(EndSample, AVRational{1, SampleRate}, InputTimebase) - PacketCopy.dts;
		PtsSynthesized = true;
		DurationSynthesized = true;
		TimestampNormalized = true;
	}

	// B-frame streams retain their source timing, so keep an output-side
	// monotonicity check as a final muxer safety net.
	const int64_t LastStreamDTS = IsAudio ? _LiveData->_LastWrittenAudioDTS : _LiveData->_LastWrittenDTS;
	if (LastStreamDTS != AV_NOPTS_VALUE && PacketCopy.dts <= LastStreamDTS)
	{
		if (IsVideo)
		{
			const std::lock_guard<std::mutex> guard(*_LiveData->_SegmentsMutex);
			++_LiveData->_DiagDroppedVideoPackets;
			++_LiveData->_SegmentDroppedVideoPackets;
		}
		RecordPacket("nonMonotonicOutput", true,
			PacketCopy.dts, PacketCopy.pts, PacketCopy.duration);
		if (IsVideo)
			NotifyDecodeCorruption(0);
		av_packet_unref(&PacketCopy);
		return CameraStreamError::Success;
	}
	if (IsVideo)
	{
		_LiveData->_LastWrittenDTS = PacketCopy.dts;
		{
			const std::lock_guard<std::mutex> guard(*_LiveData->_SegmentsMutex);
			++_LiveData->_DiagAcceptedVideoPackets;
			if (IsVideoKeyframe)
				++_LiveData->_DiagAcceptedVideoKeyframes;
			if (PacketCopy.flags & AV_PKT_FLAG_CORRUPT)
				++_LiveData->_DiagCorruptVideoPackets;
			_LiveData->_DiagVideoPhaseErrorMs = (double)_LiveData->_LastTimestampPhaseError *
				InputTimebase.num * 1000.0 / InputTimebase.den;
			_LiveData->_DiagVideoCorrectionMs = (double)_LiveData->_LastTimestampCorrection *
				InputTimebase.num * 1000.0 / InputTimebase.den;
			_LiveData->_DiagLastVideoOutputUs = av_rescale_q(PacketCopy.dts, InputTimebase, AV_TIME_BASE_Q);
			_LiveData->_DiagHasVideoOutputTimestamp = true;
			if (_LiveData->_LastTimestampCorrectionSaturated)
				++_LiveData->_DiagTimestampCorrectionSaturatedPackets;
		}
		++_LiveData->_SegmentAcceptedVideoPackets;
		if (IsVideoKeyframe)
		{
			++_LiveData->_SegmentAcceptedVideoKeyframes;
		}
		if (PacketCopy.flags & AV_PKT_FLAG_CORRUPT)
		{
			++_LiveData->_SegmentCorruptVideoPackets;
		}
		if (_LiveData->_NormalizeNoBFrameTimestamps)
			_LiveData->_SegmentTimestampNormalizationActive = true;
		if (DeclaredVideoDuration > 0)
			_LiveData->_LastPacketDuration = DeclaredVideoDuration;
		if (PacketCopy.duration > 0)
			_LiveData->_LastOutputPacketDuration = PacketCopy.duration;
	}
	else
	{
		_LiveData->_LastWrittenAudioDTS = PacketCopy.dts;
		const std::lock_guard<std::mutex> guard(*_LiveData->_SegmentsMutex);
		_LiveData->_DiagLastAudioOutputUs = av_rescale_q(PacketCopy.dts, InputTimebase, AV_TIME_BASE_Q);
		_LiveData->_DiagHasAudioOutputTimestamp = true;
	}

	if (IsVideo && _LiveData->_OutputSegmentStartDTS == AV_NOPTS_VALUE)
		_LiveData->_OutputSegmentStartDTS = PacketCopy.dts;

	if (IsVideo)
	{
		AVRational TimeBase = _LiveData->_FormatContext->streams[0]->time_base;
		double PacketDurationSec = (double)(PacketCopy.duration * TimeBase.num) / TimeBase.den;
		_LiveData->_CurrentSegmentDuration += PacketDurationSec;
		const double SourceNominalPacketDurationSec =
			(double)(SourceNominalVideoDuration * TimeBase.num) / TimeBase.den;
		_LiveData->_CurrentSegmentNominalDuration += SourceNominalPacketDurationSec;
		_LiveData->_CurrentPartialDuration += PacketDurationSec;

		// With multiplexed audio, SourceBuffer range boundaries are not guaranteed
		// to identify the exact video RAP timestamp.
		const bool PacketKeyframeSeekSafe =
			!_LiveData->_HasAudioStream && !_LiveData->_HasBFrames && _LiveData->_NormalizeNoBFrameTimestamps;
		if (!_LiveData->_CurrentPartialHasPacket)
		{
			_LiveData->_CurrentPartialHasPacket = true;
			_LiveData->_CurrentPartialIsIndependent = IsVideoKeyframe;
			_LiveData->_CurrentPartialKeyframeSeekSafe = PacketKeyframeSeekSafe;
		}
		else
		{
			_LiveData->_CurrentPartialKeyframeSeekSafe =
				_LiveData->_CurrentPartialKeyframeSeekSafe && PacketKeyframeSeekSafe;
		}
	}
	else if (IsAudio)
	{
		double PacketDurationSec = (double)(PacketCopy.duration * InputTimebase.num) / InputTimebase.den;
		_LiveData->_CurrentPartialAudioDuration += PacketDurationSec;
	}

	const int TraceSegmentIndex = _LiveData->_CurrentSegmentIndex;
	const int TracePartialIndex = _LiveData->_CurrentPartialIndex;
	const int64_t TraceOutputDts = PacketCopy.dts;
	const int64_t TraceOutputPts = PacketCopy.pts;
	const int64_t TraceOutputDuration = PacketCopy.duration;
	av_packet_rescale_ts(&PacketCopy, InputTimebase,
		_LiveData->_FormatContext->streams[PacketCopy.stream_index]->time_base);
	// Multi-track fMP4 needs FFmpeg's interleaver to emit a valid fragment.
	Result = av_interleaved_write_frame(_LiveData->_FormatContext, &PacketCopy);
	av_packet_unref(&PacketCopy);
	if (Result < 0)
	{
		RecordPacket("muxError", true, TraceOutputDts, TraceOutputPts,
			TraceOutputDuration, TraceSegmentIndex, TracePartialIndex);
		STREAM_ERROR(WriteFailed, Result);
	}
	RecordPacket("written", true, TraceOutputDts, TraceOutputPts,
		TraceOutputDuration, TraceSegmentIndex, TracePartialIndex);
	if( IsRecoveryCandidate )
	{
		_LiveData->_RecoveryPendingPublication = true;
		_LiveData->_RecoverySegmentIndex = TraceSegmentIndex;
		_LiveData->_RecoveryPartialIndex = TracePartialIndex;
		_LiveData->_RecoveryActivityID = _LiveData->_CurrentDiagnosticActivity;
		_LiveData->_RecoveryPacketSequence = PacketSequence;
	}
	if( IsVideoKeyframe )
		MarkStreamEstablished();
	_LiveData->_SegmentPacketPayloadHash = HashBytes(
		reinterpret_cast<const uint8_t*>(&PayloadHash), sizeof(PayloadHash),
		_LiveData->_SegmentPacketPayloadHash);

	// Flush a partial segment when we've accumulated enough duration
	if (IsVideo && _LiveData->_CurrentPartialDuration >= _LiveData->_PartialTargetDuration)
	{
		if( !FlushPartialSegment(_LiveData->_CurrentPartialIsIndependent) )
			return CameraStreamError::WriteFailed;
	}

	return CameraStreamError::Success;
}

bool LiveOutputStream::FlushPartialSegment(bool IsIndependent)
{
	if (!_LiveData->_FormatContext || !_LiveData->_CurrentBuffer)
		return true;

	// Flush current fragment data into the buffer
	const int InterleaveResult = av_interleaved_write_frame(_LiveData->_FormatContext, nullptr);
	const int FragmentResult = av_write_frame(_LiveData->_FormatContext, nullptr);
	avio_flush(_LiveData->_FormatContext->pb);
	if( InterleaveResult < 0 || FragmentResult < 0 )
	{
		const int Error = InterleaveResult < 0 ? InterleaveResult : FragmentResult;
		LOG_WARNING("[HLS] Camera %d failed to flush partial %d/%d: ffmpeg error %d",
			_LiveData->_InputStream ? _LiveData->_InputStream->GetSourceId() : -1,
			_LiveData->_CurrentSegmentIndex, _LiveData->_CurrentPartialIndex, Error);
		CaptureDiagnosticAnomaly("fragmentFlushError");
		_LiveData->_RecoveryPendingPublication = false;
		return false;
	}

	// Only create a partial if we actually accumulated data since the last flush
	size_t CurrentSize = _LiveData->_CurrentBuffer->size();
	// windows.h defines max as a macro in this translation unit.
	double PartialDuration = (std::max)(_LiveData->_CurrentPartialDuration, _LiveData->_CurrentPartialAudioDuration);
	if (CurrentSize <= _LiveData->_PartialBufferOffset)
		return true;
	// Some RTSP sources omit AAC packet durations. The fragment still needs to
	// be delivered; use the configured target as a conservative playlist value.
	if (PartialDuration <= 0.0)
		PartialDuration = _LiveData->_PartialTargetDuration;

	// Create a partial that references the byte range [_LiveData->_PartialBufferOffset, CurrentSize)
	// within the single segment buffer
	auto PartialData = std::make_shared<std::vector<uint8_t>>(
		_LiveData->_CurrentBuffer->begin() + _LiveData->_PartialBufferOffset,
		_LiveData->_CurrentBuffer->begin() + CurrentSize
	);

	_LiveData->_PartialBufferOffset = CurrentSize;

	LiveStreamPartialSegment Partial;
	Partial.Data = PartialData;
	Partial.Duration = PartialDuration;
	Partial.PartIndex = _LiveData->_CurrentPartialIndex;
	Partial.Independent = IsIndependent;
	const bool KeyframeSeekSafe = _LiveData->_CurrentPartialKeyframeSeekSafe;
	const IsoBmffStructure PartialStructure = AnalyseIsoBmff(
		PartialData->data(), PartialData->size(), false);
	FragmentDiagEntry FragmentDiag;
	FragmentDiag.Generation = _LiveData->_InitGeneration;
	FragmentDiag.SegmentIndex = _LiveData->_CurrentSegmentIndex;
	FragmentDiag.PartIndex = Partial.PartIndex;
	FragmentDiag.Independent = Partial.Independent;
	FragmentDiag.KeyframeSeekSafe = KeyframeSeekSafe;
	FragmentDiag.Bytes = PartialData->size();
	FragmentDiag.Hash = HashBytes(PartialData->data(), PartialData->size());
	FragmentDiag.StructureValid = PartialStructure.Valid;
	FragmentDiag.BoxCount = PartialStructure.BoxCount;
	FragmentDiag.MoofCount = PartialStructure.MoofCount;
	FragmentDiag.MdatCount = PartialStructure.MdatCount;
	FragmentDiag.StructureError = PartialStructure.Error;
	FragmentDiag.ErrorOffset = PartialStructure.ErrorOffset;

	{
		const std::lock_guard<std::mutex> guard(*_LiveData->_SegmentsMutex);

		if (!_LiveData->_StreamBacklog->empty())
		{
			_LiveData->_StreamBacklog->back().Partials.push_back(Partial);
		}
		if (_LiveData->_RetainMuxTrace)
		{
			_LiveData->_FragmentDiagRing[_LiveData->_FragmentDiagRingPos % _LiveData->FRAGMENT_DIAG_RING_SIZE] = FragmentDiag;
			++_LiveData->_FragmentDiagRingPos;
			if (_LiveData->_FragmentDiagRingCount < _LiveData->FRAGMENT_DIAG_RING_SIZE)
				++_LiveData->_FragmentDiagRingCount;
		}
	}
	if (!PartialStructure.Valid)
		CaptureDiagnosticAnomaly("invalidFragmentStructure");
	const bool PublishesRecovery = _LiveData->_RecoveryPendingPublication &&
		_LiveData->_RecoverySegmentIndex == _LiveData->_CurrentSegmentIndex &&
		_LiveData->_RecoveryPartialIndex == Partial.PartIndex && Partial.Independent &&
		PartialStructure.Valid;

	_LiveData->_CurrentPartialIndex++;
	_LiveData->_CurrentPartialDuration = 0.0;
	_LiveData->_CurrentPartialAudioDuration = 0.0;
	_LiveData->_CurrentPartialIsIndependent = false;
	_LiveData->_CurrentPartialHasPacket = false;
	_LiveData->_CurrentPartialKeyframeSeekSafe = false;
	// Arm the browser before the matching partial metadata is delivered. The
	// subsequent control and binary messages are queued on the same WebSocket,
	// so releaseRendering is captured on this exact independent fragment.
	if( PublishesRecovery )
		PublishDecodeRecovery( FragmentDiag.SegmentIndex, FragmentDiag.PartIndex );
	if (auto Capture = ActivePacketCapture())
		Capture->AddOutput(std::format(
			"{{\"type\":\"partial\",\"generation\":{},\"segment\":{},\"part\":{},\"independent\":{},\"hashFnv64\":\"{:016x}\",\"structureValid\":{}",
			FragmentDiag.Generation, FragmentDiag.SegmentIndex,
			FragmentDiag.PartIndex, FragmentDiag.Independent,
			FragmentDiag.Hash, FragmentDiag.StructureValid),
			PartialData->data(), PartialData->size());

	// Notify MSE subscribers of new partial
	if (_LiveData->_EventCallback)
	{
		LiveStreamEvent Event;
		Event.EventType = LiveStreamEvent::PartialReady;
		Event.SegmentIndex = _LiveData->_CurrentSegmentIndex;
		Event.PartIndex = Partial.PartIndex;
		Event.Data = Partial.Data;
		Event.Duration = Partial.Duration;
		Event.Independent = Partial.Independent;
		Event.KeyframeSeekSafe = KeyframeSeekSafe;
		Event.Generation = _LiveData->_InitGeneration;
		Event.ByteSize = Event.Data ? Event.Data->size() : 0;
		Event.TransportHash = Event.ByteSize > 0 ?
			HashBytes32(Event.Data->data(), Event.Data->size()) : 0;
		_LiveData->_EventCallback(Event);
	}
	return true;
}

void LiveOutputStream::PublishDecodeRecovery(int SegmentIndex, int PartialIndex)
{
	if( !_LiveData->_RecoveryPendingPublication || !_LiveData->_DecodeCorruptionActive )
		return;

	LiveStreamEvent Event;
	Event.EventType = LiveStreamEvent::DecodeRecovery;
	Event.SegmentIndex = SegmentIndex;
	Event.PartIndex = PartialIndex;
	Event.Generation = _LiveData->_InitGeneration;
	{
		MediaDiagnosticEvent Diagnostic;
		Diagnostic.TimestampUnixMs = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::system_clock::now().time_since_epoch()).count();
		Diagnostic.ElapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now() - _LiveData->_PacketDiagEpoch).count();
		Diagnostic.ActivityID = _LiveData->_RecoveryActivityID;
		Diagnostic.PacketSequence = _LiveData->_RecoveryPacketSequence;
		Diagnostic.Generation = _LiveData->_InitGeneration;
		Diagnostic.SegmentIndex = SegmentIndex;
		Diagnostic.PartialIndex = PartialIndex;
		Diagnostic.Category = "decode";
		Diagnostic.Severity = "info";
		Diagnostic.Phase = "presentation";
		Diagnostic.Component = "muxer";
		Diagnostic.Message = "Published an independently decodable recovery fragment";
		Diagnostic.Disposition = "decodeRecovery";
		const std::lock_guard<std::mutex> Guard( *_LiveData->_SegmentsMutex );
		++_LiveData->_DiagDecodeRecoveryEvents;
		Diagnostic.Sequence = ++_LiveData->_MediaEventSequence;
		_LiveData->_MediaEventRing[_LiveData->_MediaEventRingPos % _LiveData->MEDIA_EVENT_RING_SIZE] = std::move(Diagnostic);
		++_LiveData->_MediaEventRingPos;
		if( _LiveData->_MediaEventRingCount < _LiveData->MEDIA_EVENT_RING_SIZE )
			++_LiveData->_MediaEventRingCount;
	}
	_LiveData->_DecodeCorruptionActive = false;
	_LiveData->_RecoveryPendingPublication = false;
	_LiveData->_RecoverySegmentIndex = -1;
	_LiveData->_RecoveryPartialIndex = -1;
	_LiveData->_RecoveryActivityID = 0;
	_LiveData->_RecoveryPacketSequence = 0;
	if( _LiveData->_EventCallback )
		_LiveData->_EventCallback(Event);
}

void LiveOutputStream::MarkStreamEstablished()
{
	const std::lock_guard<std::mutex> Guard( *_LiveData->_SegmentsMutex );
	if( _LiveData->_StreamEstablished || _LiveData->_StartupGraceStarted.time_since_epoch().count() == 0 )
		return;
	const auto Elapsed = std::chrono::steady_clock::now() - _LiveData->_StartupGraceStarted;
	if( Elapsed < std::chrono::seconds(5) )
		return;
	_LiveData->_StartupAcceptedVideoPackets = _LiveData->_DiagAcceptedVideoPackets -
		_LiveData->_GenerationAcceptedVideoPacketsBaseline;
	_LiveData->_StartupDroppedVideoPackets = _LiveData->_DiagDroppedVideoPackets -
		_LiveData->_GenerationDroppedVideoPacketsBaseline;
	_LiveData->_StartupRepairedVideoTimestamps = _LiveData->_DiagRepairedVideoTimestamps -
		_LiveData->_GenerationRepairedVideoTimestampsBaseline;
	_LiveData->_EstablishedAcceptedVideoPacketsBaseline = _LiveData->_DiagAcceptedVideoPackets;
	_LiveData->_EstablishedDroppedVideoPacketsBaseline = _LiveData->_DiagDroppedVideoPackets;
	_LiveData->_EstablishedRepairedVideoTimestampsBaseline = _LiveData->_DiagRepairedVideoTimestamps;
	_LiveData->_StreamEstablished = true;
}

CameraStreamError LiveOutputStream::StartNewSegment(const AVPacket* Packet)
{
	if (!_LiveData->_InitSegmentCaptured)
	{
		// Write the init segment to an in-memory buffer
		_LiveData->_CurrentBuffer = std::make_shared<std::vector<uint8_t>>();

		AVDictionary* options = nullptr;
		av_dict_set(&options, "movflags", "empty_moov+frag_custom+dash+default_base_moof", 0);
		av_dict_set(&options, "brand", "iso6", 0);

		int Result = avformat_write_header(_LiveData->_FormatContext, &options);
		av_dict_free(&options);
		if (Result < 0)
		{
			STREAM_ERROR(WriteFailed, Result);
		}

		_LiveData->_HeaderWritten = true;

		avio_flush(_LiveData->_FormatContext->pb);
		const IsoBmffStructure InitStructure = AnalyseIsoBmff(
			_LiveData->_CurrentBuffer->data(), _LiveData->_CurrentBuffer->size(), true);

		// Store the init segment (ftyp + moov)
		LiveStreamEvent InitEvent;
		{
			const std::lock_guard<std::mutex> guard(*_LiveData->_SegmentsMutex);
			_LiveData->_InitSegmentData = _LiveData->_CurrentBuffer;
			_LiveData->_InitSegmentGeneration = _LiveData->_InitGeneration;
			_LiveData->_InitAudioCodec = _LiveData->_HasAudioStream ? "mp4a.40.2" : "";
			InitEvent.EventType = LiveStreamEvent::InitSegmentReady;
			InitEvent.Data = _LiveData->_InitSegmentData;
			InitEvent.Generation = _LiveData->_InitGeneration;
			InitEvent.AudioCodec = _LiveData->_InitAudioCodec;
			InitEvent.ByteSize = _LiveData->_InitSegmentData ? _LiveData->_InitSegmentData->size() : 0;
			InitEvent.TransportHash = InitEvent.ByteSize > 0 ?
				HashBytes32(_LiveData->_InitSegmentData->data(), _LiveData->_InitSegmentData->size()) : 0;
			_LiveData->_DiagInitStructureValid = InitStructure.Valid;
			_LiveData->_DiagInitStructureObserved = true;
			_LiveData->_DiagInitBoxCount = InitStructure.BoxCount;
			_LiveData->_DiagInitFtypCount = InitStructure.FtypCount;
			_LiveData->_DiagInitMoovCount = InitStructure.MoovCount;
			_LiveData->_DiagInitStructureError = InitStructure.Error;
			_LiveData->_DiagInitErrorOffset = InitStructure.ErrorOffset;
		}

		_LiveData->_InitSegmentCaptured = true;
		if (auto Capture = ActivePacketCapture())
			Capture->AddOutput(std::format(
				"{{\"type\":\"init\",\"generation\":{}", _LiveData->_InitGeneration),
				_LiveData->_InitSegmentData->data(), _LiveData->_InitSegmentData->size());
		if (!InitStructure.Valid)
			CaptureDiagnosticAnomaly("invalidInitStructure");

		// Notify MSE subscribers of init segment
		if (_LiveData->_EventCallback)
		{
			_LiveData->_EventCallback(InitEvent);
		}
	}

	// Start a fresh buffer for this segment
	_LiveData->_CurrentBuffer = std::make_shared<std::vector<uint8_t>>();

	_LiveData->_SegmentStartDTS = Packet->dts;
	_LiveData->_OutputSegmentStartDTS = AV_NOPTS_VALUE;
	_LiveData->_CurrentSegmentDuration = 0.0;
	_LiveData->_CurrentSegmentNominalDuration = 0.0;
	_LiveData->_CurrentPartialIndex = 0;
	_LiveData->_CurrentPartialDuration = 0.0;
	_LiveData->_CurrentPartialAudioDuration = 0.0;
	_LiveData->_CurrentPartialIsIndependent = true; // first partial starts with keyframe
	_LiveData->_CurrentPartialHasPacket = false;
	_LiveData->_CurrentPartialKeyframeSeekSafe = false;
	_LiveData->_SegmentTimestampNormalizationActive = _LiveData->_NormalizeNoBFrameTimestamps;
	_LiveData->_PartialBufferOffset = 0;
	_LiveData->_SegmentAcceptedVideoPackets = 0;
	_LiveData->_SegmentAcceptedVideoKeyframes = 0;
	_LiveData->_SegmentRepairedVideoTimestamps = 0;
	_LiveData->_SegmentDroppedVideoPackets = 0;
	_LiveData->_SegmentMissingVideoDtsPackets = 0;
	_LiveData->_SegmentCorruptVideoPackets = 0;
	_LiveData->_SegmentPacketPayloadHash = Fnv1aOffsetBasis;
	_LiveData->_CurrentSegmentWallTime = std::chrono::system_clock::now();

	{
		const std::lock_guard<std::mutex> guard(*_LiveData->_SegmentsMutex);

		// Trim old segments
		while (_LiveData->_StreamBacklog->size() >= 15)
		{
			_LiveData->_StreamBacklog->erase(_LiveData->_StreamBacklog->begin());
		}

		LiveStreamSegment NewSegment;
		NewSegment.Data = nullptr; // Will be set when finished
		NewSegment.Duration = 0.0;
		NewSegment.SegmentIndex = _LiveData->_CurrentSegmentIndex;
		NewSegment.SegmentTime = _LiveData->_CurrentSegmentWallTime;
		NewSegment.Ready = false;
		NewSegment.Discontinuity = _LiveData->_DiscontinuityPending;
		_LiveData->_DiscontinuityPending = false;

		_LiveData->_StreamBacklog->push_back(NewSegment);
	}

	return CameraStreamError::Success;
}

CameraStreamError LiveOutputStream::FinishCurrentSegment(int64_t NextKeyframeDTS)
{
	if (!_LiveData->_FormatContext || !_LiveData->_CurrentBuffer)
	{
		return CameraStreamError::Success;
	}

	// Flush remaining data as the final partial of this segment
	if( !FlushPartialSegment(_LiveData->_CurrentPartialIsIndependent) )
		return CameraStreamError::WriteFailed;

	// Compute accurate segment duration from DTS span instead of
	// accumulated packet durations. Accumulated durations drift over
	// thousands of segments because AVPacket.duration values don't
	// exactly match the DTS delta between keyframes.
	AVRational TimeBase = _LiveData->_FormatContext->streams[0]->time_base;
	double DtsDuration = (double)(NextKeyframeDTS - _LiveData->_SegmentStartDTS) * TimeBase.num / TimeBase.den;
	double OutputDuration = _LiveData->_CurrentSegmentDuration;
	if (_LiveData->_OutputSegmentStartDTS != AV_NOPTS_VALUE && _LiveData->_LastWrittenDTS != AV_NOPTS_VALUE && _LiveData->_LastOutputPacketDuration > 0)
	{
		OutputDuration = (double)(_LiveData->_LastWrittenDTS + _LiveData->_LastOutputPacketDuration - _LiveData->_OutputSegmentStartDTS) *
			TimeBase.num / TimeBase.den;
	}

	// Sanity: if DTS duration is clearly wrong, fall back to accumulated
	if (DtsDuration <= 0.0 || DtsDuration > 30.0)
		DtsDuration = _LiveData->_CurrentSegmentNominalDuration;

	// Record diagnostics
	double DriftMs = (_LiveData->_CurrentSegmentNominalDuration - DtsDuration) * 1000.0;
	SegmentDiagEntry Entry;
	Entry.SegmentIndex = _LiveData->_CurrentSegmentIndex;
	Entry.DtsDuration = DtsDuration;
	Entry.OutputDuration = OutputDuration;
	Entry.AccumulatedDuration = _LiveData->_CurrentSegmentNominalDuration;
	Entry.DriftMs = DriftMs;
	Entry.TimestampNormalizationActive = _LiveData->_SegmentTimestampNormalizationActive;
	Entry.AcceptedVideoPackets = _LiveData->_SegmentAcceptedVideoPackets;
	Entry.AcceptedVideoKeyframes = _LiveData->_SegmentAcceptedVideoKeyframes;
	Entry.RepairedVideoTimestamps = _LiveData->_SegmentRepairedVideoTimestamps;
	Entry.DroppedVideoPackets = _LiveData->_SegmentDroppedVideoPackets;
	Entry.MissingVideoDtsPackets = _LiveData->_SegmentMissingVideoDtsPackets;
	Entry.CorruptVideoPackets = _LiveData->_SegmentCorruptVideoPackets;
	Entry.PacketPayloadHash = _LiveData->_SegmentPacketPayloadHash;
	Entry.FragmentBytes = _LiveData->_CurrentBuffer ? (uint64_t)_LiveData->_CurrentBuffer->size() : 0;
	Entry.FragmentHash = Entry.FragmentBytes > 0 ?
		HashBytes(_LiveData->_CurrentBuffer->data(), _LiveData->_CurrentBuffer->size()) : Fnv1aOffsetBasis;
	const IsoBmffStructure FragmentStructure = AnalyseIsoBmff(
		_LiveData->_CurrentBuffer->data(), _LiveData->_CurrentBuffer->size(), false);
	Entry.FragmentStructureValid = FragmentStructure.Valid;
	Entry.FragmentBoxCount = FragmentStructure.BoxCount;
	Entry.FragmentMoofCount = FragmentStructure.MoofCount;
	Entry.FragmentMdatCount = FragmentStructure.MdatCount;
	Entry.FragmentStructureError = FragmentStructure.Error;
	Entry.FragmentErrorOffset = FragmentStructure.ErrorOffset;
	{
		const std::lock_guard<std::mutex> guard(*_LiveData->_SegmentsMutex);
		if (_LiveData->_RetainMuxTrace)
		{
			_LiveData->_DiagRing[_LiveData->_DiagRingPos % _LiveData->DIAG_RING_SIZE] = Entry;
			_LiveData->_DiagRingPos++;
			if (_LiveData->_DiagRingCount < _LiveData->DIAG_RING_SIZE) _LiveData->_DiagRingCount++;
		}
		_LiveData->_LastCompletedSegmentNormalizationActive = Entry.TimestampNormalizationActive;
		_LiveData->_DiagTotalSegments++;
		_LiveData->_DiagTotalDtsDuration += DtsDuration;
		_LiveData->_DiagTotalAccumulatedDuration += _LiveData->_CurrentSegmentNominalDuration;
		if (std::abs(DriftMs) > std::abs(_LiveData->_DiagMaxDriftMs))
			_LiveData->_DiagMaxDriftMs = DriftMs;

		if (!_LiveData->_StreamBacklog->empty())
		{
			auto& Seg = _LiveData->_StreamBacklog->back();
			Seg.Data = _LiveData->_CurrentBuffer;
			// Use accumulated packet duration for EXTINF — this matches the
			// actual sample durations in the fMP4 container. Using DTS span
			// causes playlist/media divergence: DTS may differ from packet
			// duration sums by ~0.002s/segment, which over thousands of
			// segments makes the playhead outrun buffered media.
			Seg.Duration = _LiveData->_CurrentSegmentDuration;
			Seg.Ready = true;
		}
		_LiveData->_CurrentSegmentIndex++;
	}

	_LiveData->_CurrentBuffer.reset();

	// Notify MSE subscribers that segment is complete
	// (MSE clients primarily use partials for low latency, but this
	// signals segment boundaries for buffer management)
	if (_LiveData->_EventCallback)
	{
		LiveStreamEvent Event;
		Event.EventType = LiveStreamEvent::SegmentReady;
		Event.SegmentIndex = _LiveData->_CurrentSegmentIndex - 1;
		Event.Duration = _LiveData->_CurrentSegmentDuration;
		Event.Generation = _LiveData->_InitGeneration;
		_LiveData->_EventCallback(Event);
	}
	return CameraStreamError::Success;
}

LiveOutputStream::StreamingDiagnostics LiveOutputStream::GetStreamingDiagnostics( bool IncludeHistory ) const
{
	StreamingDiagnostics Diag;
	const std::lock_guard<std::mutex> guard(*_LiveData->_SegmentsMutex);
	Diag.PacketTraceEnabled = _LiveData->_RetainPacketTrace;
	Diag.MuxTraceEnabled = _LiveData->_RetainMuxTrace;
	Diag.TotalSegments = _LiveData->_DiagTotalSegments;
	Diag.ReconnectCount = _LiveData->_InitGeneration;
	Diag.TotalDtsDuration = _LiveData->_DiagTotalDtsDuration;
	Diag.TotalAccumulatedDuration = _LiveData->_DiagTotalAccumulatedDuration;
	Diag.MaxDriftMs = _LiveData->_DiagMaxDriftMs;
	Diag.CurrentSegmentIndex = _LiveData->_CurrentSegmentIndex;
	Diag.InitGeneration = _LiveData->_InitGeneration;
	Diag.AcceptedVideoPackets = _LiveData->_DiagAcceptedVideoPackets;
	Diag.AcceptedVideoKeyframes = _LiveData->_DiagAcceptedVideoKeyframes;
	Diag.RepairedVideoTimestamps = _LiveData->_DiagRepairedVideoTimestamps;
    Diag.TimestampNormalizationRejected = _LiveData->_DiagTimestampRejected;
    Diag.TimestampRetryAttempts = _LiveData->_DiagTimestampRetryAttempts;
    Diag.TimestampPauseResets = _LiveData->_DiagTimestampPauseResets;
    Diag.TimestampLastPauseGapMs = _LiveData->_DiagTimestampLastPauseGapMs;
    Diag.TimestampRejectedUnixMs = _LiveData->_DiagTimestampRejectedUnixMs;
    Diag.TimestampDeclaredWindowMs = _LiveData->_DiagTimestampDeclaredWindowMs;
    Diag.TimestampReferenceWindowMs = _LiveData->_DiagTimestampReferenceWindowMs;
    Diag.TimestampDelivery = _LiveData->_DiagTimestampDeliveryEvidence;
    Diag.TimestampRejectionClock = _LiveData->_DiagTimestampRejectedUnixMs ?
        (_LiveData->_DiagTimestampRejectionUsesArrival ? "arrival" : "source") : "";
	Diag.DroppedVideoPackets = _LiveData->_DiagDroppedVideoPackets;
	Diag.MissingVideoDtsPackets = _LiveData->_DiagMissingVideoDtsPackets;
	Diag.CorruptVideoPackets = _LiveData->_DiagCorruptVideoPackets;
	Diag.WaitingForKeyframePackets = _LiveData->_DiagWaitingForKeyframePackets;
	Diag.MissingTimestampPackets = _LiveData->_DiagMissingTimestampPackets;
	Diag.BeforeVideoEpochPackets = _LiveData->_DiagBeforeVideoEpochPackets;
	Diag.NegativeTimestampPackets = _LiveData->_DiagNegativeTimestampPackets;
	Diag.NonMonotonicInputPackets = _LiveData->_DiagNonMonotonicInputPackets;
	Diag.NoMuxBufferPackets = _LiveData->_DiagNoMuxBufferPackets;
	Diag.NonMonotonicOutputPackets = _LiveData->_DiagNonMonotonicOutputPackets;
	Diag.MuxErrorPackets = _LiveData->_DiagMuxErrorPackets;
	Diag.DecodeCorruptionEvents = _LiveData->_DiagDecodeCorruptionEvents;
	Diag.DecodeRecoveryEvents = _LiveData->_DiagDecodeRecoveryEvents;
	Diag.StreamEstablished = _LiveData->_StreamEstablished;
	if( _LiveData->_StartupGraceStarted.time_since_epoch().count() != 0 )
		Diag.StartupGraceElapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now() - _LiveData->_StartupGraceStarted).count();
	if( _LiveData->_StreamEstablished )
	{
		Diag.StartupAcceptedVideoPackets = _LiveData->_StartupAcceptedVideoPackets;
		Diag.StartupDroppedVideoPackets = _LiveData->_StartupDroppedVideoPackets;
		Diag.StartupRepairedVideoTimestamps = _LiveData->_StartupRepairedVideoTimestamps;
		Diag.EstablishedAcceptedVideoPackets = _LiveData->_DiagAcceptedVideoPackets -
			_LiveData->_EstablishedAcceptedVideoPacketsBaseline;
		Diag.EstablishedDroppedVideoPackets = _LiveData->_DiagDroppedVideoPackets -
			_LiveData->_EstablishedDroppedVideoPacketsBaseline;
		Diag.EstablishedRepairedVideoTimestamps = _LiveData->_DiagRepairedVideoTimestamps -
			_LiveData->_EstablishedRepairedVideoTimestampsBaseline;
	}
	else
	{
		Diag.StartupAcceptedVideoPackets = _LiveData->_DiagAcceptedVideoPackets -
			_LiveData->_GenerationAcceptedVideoPacketsBaseline;
		Diag.StartupDroppedVideoPackets = _LiveData->_DiagDroppedVideoPackets -
			_LiveData->_GenerationDroppedVideoPacketsBaseline;
		Diag.StartupRepairedVideoTimestamps = _LiveData->_DiagRepairedVideoTimestamps -
			_LiveData->_GenerationRepairedVideoTimestampsBaseline;
	}
	Diag.VideoPhaseErrorMs = _LiveData->_DiagVideoPhaseErrorMs;
	Diag.VideoCorrectionMs = _LiveData->_DiagVideoCorrectionMs;
	Diag.HasAudioVideoSkew = _LiveData->_DiagHasAudioOutputTimestamp && _LiveData->_DiagHasVideoOutputTimestamp;
	if (Diag.HasAudioVideoSkew)
		Diag.AudioVideoSkewMs = (_LiveData->_DiagLastAudioOutputUs - _LiveData->_DiagLastVideoOutputUs) / 1000.0;
	Diag.TimestampCorrectionSaturatedPackets = _LiveData->_DiagTimestampCorrectionSaturatedPackets;
	Diag.TimestampArrivalRateAdjustments = _LiveData->_DiagTimestampArrivalRateAdjustments;
	Diag.TimestampArrivalDurationMs = _LiveData->_DiagTimestampArrivalDurationMs;
	Diag.VideoCodec = _LiveData->_DiagVideoCodec;
	Diag.AudioCodec = _LiveData->_DiagAudioCodec;
	Diag.InputFormat = _LiveData->_DiagInputFormat;
	Diag.VideoProfile = _LiveData->_DiagVideoProfile;
	Diag.VideoLevel = _LiveData->_DiagVideoLevel;
	Diag.VideoWidth = _LiveData->_DiagVideoWidth;
	Diag.VideoHeight = _LiveData->_DiagVideoHeight;
	Diag.VideoTimeBaseNum = _LiveData->_DiagVideoTimeBaseNum;
	Diag.VideoTimeBaseDen = _LiveData->_DiagVideoTimeBaseDen;
	Diag.VideoExtradataBytes = _LiveData->_DiagVideoExtradataBytes;
	Diag.VideoExtradataHash = _LiveData->_DiagVideoExtradataHash;
	Diag.AudioProfile = _LiveData->_DiagAudioProfile;
	Diag.AudioSampleRate = _LiveData->_DiagAudioSampleRate;
	Diag.AudioChannels = _LiveData->_DiagAudioChannels;
	Diag.AudioTimeBaseNum = _LiveData->_DiagAudioTimeBaseNum;
	Diag.AudioTimeBaseDen = _LiveData->_DiagAudioTimeBaseDen;
	Diag.AudioExtradataBytes = _LiveData->_DiagAudioExtradataBytes;
	Diag.AudioExtradataHash = _LiveData->_DiagAudioExtradataHash;
	Diag.InitStructureValid = _LiveData->_DiagInitStructureValid;
	Diag.InitStructureObserved = _LiveData->_DiagInitStructureObserved;
	Diag.InitBoxCount = _LiveData->_DiagInitBoxCount;
	Diag.InitFtypCount = _LiveData->_DiagInitFtypCount;
	Diag.InitMoovCount = _LiveData->_DiagInitMoovCount;
	Diag.InitStructureError = _LiveData->_DiagInitStructureError;
	Diag.InitErrorOffset = _LiveData->_DiagInitErrorOffset;
	Diag.BacklogSize = (int)_LiveData->_StreamBacklog->size();
	const int count = _LiveData->_DiagRingCount;
	if( _LiveData->_DiagTotalSegments > 0 )
		Diag.TimestampNormalizationActive = _LiveData->_LastCompletedSegmentNormalizationActive;
	// A repeated FFmpeg cluster keeps its original ring slot. Rank by its last
	// occurrence so a still-active cluster remains visible in the health export.
	std::vector<const MediaDiagnosticEvent*> RecentMediaEvents;
	RecentMediaEvents.reserve( _LiveData->_MediaEventRingCount );
	const int MediaEventStart = _LiveData->_MediaEventRingPos - _LiveData->_MediaEventRingCount;
	for( int Index = 0; Index < _LiveData->_MediaEventRingCount; ++Index )
		RecentMediaEvents.push_back( &_LiveData->_MediaEventRing[(MediaEventStart + Index) % _LiveData->MEDIA_EVENT_RING_SIZE] );
	auto EventTime = []( const MediaDiagnosticEvent* Event )
	{
		return Event->LastElapsedMs ? Event->LastElapsedMs : Event->ElapsedMs;
	};
	std::sort( RecentMediaEvents.begin(), RecentMediaEvents.end(), [&]( const auto* Left, const auto* Right )
	{
		return EventTime( Left ) > EventTime( Right );
	} );
	if( RecentMediaEvents.size() > 48 ) RecentMediaEvents.resize( 48 );
	Diag.RecentMediaEvents.reserve( RecentMediaEvents.size() );
	for( const auto* Event : RecentMediaEvents ) Diag.RecentMediaEvents.push_back( *Event );

	if( !IncludeHistory )
		return Diag;

	// Copy recent segment entries from ring buffer
	int start = (_LiveData->_DiagRingPos - count);
	if (start < 0) start += _LiveData->DIAG_RING_SIZE;
	for (int i = 0; i < count; i++)
	{
		Diag.RecentSegments.push_back(_LiveData->_DiagRing[(start + i) % _LiveData->DIAG_RING_SIZE]);
	}
	const int FragmentStart = _LiveData->_FragmentDiagRingPos - _LiveData->_FragmentDiagRingCount;
	Diag.RecentFragments.reserve(_LiveData->_FragmentDiagRingCount);
	for (int Index = 0; Index < _LiveData->_FragmentDiagRingCount; ++Index)
	{
		Diag.RecentFragments.push_back(
			_LiveData->_FragmentDiagRing[(FragmentStart + Index) % _LiveData->FRAGMENT_DIAG_RING_SIZE]);
	}
	const int PacketStart = _LiveData->_PacketDiagRingPos - _LiveData->_PacketDiagRingCount;
	Diag.RecentPackets.reserve(_LiveData->_PacketDiagRingCount);
	for (int Index = 0; Index < _LiveData->_PacketDiagRingCount; ++Index)
	{
		Diag.RecentPackets.push_back(
			_LiveData->_PacketDiagRing[(PacketStart + Index) % _LiveData->PACKET_DIAG_RING_SIZE]);
	}
	Diag.Anomalies = _LiveData->_DiagAnomalies;

	return Diag;
}

void LiveOutputStream::CaptureDiagnosticAnomaly(const std::string& Reason)
{
	// No passive history exists to snapshot in the fully disabled ring mode.
	// Explicit authenticated packet capture remains separate and operational.
	if (!_LiveData->_RetainMuxTrace && !_LiveData->_RetainPacketTrace) return;
	const int64_t CapturedAtMs = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now() - _LiveData->_PacketDiagEpoch).count();
	const std::lock_guard<std::mutex> guard(*_LiveData->_SegmentsMutex);
	if (!_LiveData->_DiagAnomalies.empty() && _LiveData->_DiagAnomalies.back().Reason == Reason &&
		CapturedAtMs - _LiveData->_DiagAnomalies.back().CapturedAtMs < 5000)
	{
		return;
	}

	StreamingDiagnostics::AnomalyCapture Capture;
	Capture.Sequence = ++_LiveData->_DiagAnomalySequence;
	Capture.CapturedAtMs = CapturedAtMs;
	Capture.Generation = _LiveData->_InitGeneration;
	Capture.SegmentIndex = _LiveData->_CurrentSegmentIndex;
	Capture.Reason = Reason.substr(0, 64);

	const int FragmentCount = (std::min)(_LiveData->_FragmentDiagRingCount, 60);
	const int FragmentStart = _LiveData->_FragmentDiagRingPos - FragmentCount;
	Capture.Fragments.reserve(FragmentCount);
	for (int Index = 0; Index < FragmentCount; ++Index)
	{
		Capture.Fragments.push_back(
			_LiveData->_FragmentDiagRing[(FragmentStart + Index) % _LiveData->FRAGMENT_DIAG_RING_SIZE]);
	}

	const int PacketCount = (std::min)(_LiveData->_PacketDiagRingCount, 256);
	const int PacketStart = _LiveData->_PacketDiagRingPos - PacketCount;
	Capture.Packets.reserve(PacketCount);
	for (int Index = 0; Index < PacketCount; ++Index)
	{
		Capture.Packets.push_back(
			_LiveData->_PacketDiagRing[(PacketStart + Index) % _LiveData->PACKET_DIAG_RING_SIZE]);
	}

	if (_LiveData->_DiagAnomalies.size() >= 3)
		_LiveData->_DiagAnomalies.erase(_LiveData->_DiagAnomalies.begin());
	_LiveData->_DiagAnomalies.push_back(std::move(Capture));
}

}}
