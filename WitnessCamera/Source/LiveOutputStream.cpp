#include "LiveOutputStream.h"
#include "OutputStream.h"
#include "InputStream.h"
#include "StreamData.h"
#include "PacketCapture.h"

#include <Log.h>
#include <algorithm>
#include <sstream>
#include <chrono>
#include <cmath>
#include <climits>
#include <filesystem>
#include <format>
#include <atomic>

namespace Witness{
namespace Camera{

static const int AVIOBufferSize = 64 * 1024;
static const uint64_t Fnv1aOffsetBasis = 14695981039346656037ULL;
static const uint64_t Fnv1aPrime = 1099511628211ULL;

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

LiveOutputStream::LiveOutputStream(const std::string& LiveCachePath, InputStream* InputStream, int KeyframesPerSegment)
	: Stream()
	, _LiveCachePath( LiveCachePath )
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
}

LiveOutputStream::~LiveOutputStream()
{
	Shutdown();
	delete _PacketCaptureState;
	_PacketCaptureState = nullptr;

	delete _StreamBacklog;
	_StreamBacklog = nullptr;

	delete _SegmentsMutex;
	_SegmentsMutex = nullptr;
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
	_CurrentDiagnosticActivity = ++_DiagnosticActivitySequence;
	return _CurrentDiagnosticActivity;
}

std::shared_ptr<PacketCapture> LiveOutputStream::ActivePacketCapture() const
{
	std::lock_guard lock(_PacketCaptureState->Mutex);
	return _PacketCaptureState->Current;
}

bool LiveOutputStream::StartPacketCapture(int CameraID, const std::string& Tier,
	int DurationSeconds, std::string& Directory)
{
	if ((Tier != "main" && Tier != "preview") ||
		CameraID <= 0 || DurationSeconds < 1 || DurationSeconds > 30)
		return false;
	std::lock_guard captureLock(_PacketCaptureState->Mutex);
	if (_PacketCaptureState->Current && !_PacketCaptureState->Current->Complete())
		return false;
	static std::atomic<uint64_t> NextCapture{ 0 };
	const auto Timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::system_clock::now().time_since_epoch()).count();
	const auto Path = std::filesystem::path(_LiveCachePath) / "packet-captures" /
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
		std::lock_guard segmentLock(*_SegmentsMutex);
		Init = _InitSegmentData;
		Generation = _InitSegmentGeneration;
	}
	if (Init && !Init->empty())
		Capture->AddOutput(std::format(
			"{{\"type\":\"init\",\"generation\":{}", Generation),
			Init->data(), Init->size());
	_PacketCaptureState->Current = std::move(Capture);
	Directory = Path.string();
	LOG_INFO("[PacketCapture] Camera %d %s: capturing up to %ds in %s",
		CameraID, Tier.c_str(), DurationSeconds, Directory.c_str());
	return true;
}

void LiveOutputStream::CaptureInputPacket(const AVPacket* Packet, uint64_t ActivityID)
{
	auto Capture = ActivePacketCapture();
	if (!Capture || Capture->Complete() || !Packet || !Packet->data ||
		Packet->size <= 0 || !_InputStream)
		return;
	const auto& Data = _InputStream->GetData();
	if (Packet->stream_index != Data.ChosenStreamIndex &&
		Packet->stream_index != Data.ChosenAudioStreamIndex)
		return;
	const AVStream* Stream = Data.FormatContext->streams[Packet->stream_index];
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
		"{{\"type\":\"input\",\"activityId\":{},\"audio\":{},\"streamIndex\":{},\"codecId\":{},\"timeBaseNum\":{},\"timeBaseDen\":{},\"dts\":{},\"pts\":{},\"duration\":{},\"flags\":{},\"hashFnv64\":\"{:016x}\"",
		ActivityID, Audio, Packet->stream_index,
		static_cast<int>(Stream->codecpar->codec_id), Stream->time_base.num,
		Stream->time_base.den, Packet->dts, Packet->pts, Packet->duration,
		Packet->flags, Hash), Packet->data, static_cast<size_t>(Packet->size));
}

uint64_t LiveOutputStream::RecordFFmpegLog( int Level, const char* Phase,
	const char* Component, const char* Message, uint64_t ActivityID )
{
	MediaDiagnosticEvent Event;
	Event.TimestampUnixMs = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::system_clock::now().time_since_epoch()).count();
	Event.ElapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now() - _PacketDiagEpoch).count();
	Event.LastTimestampUnixMs = Event.TimestampUnixMs;
	Event.LastElapsedMs = Event.ElapsedMs;
	Event.ActivityID = ActivityID;
	Event.Category = "ffmpeg";
	Event.Severity = Level <= AV_LOG_ERROR ? "error" : "warning";
	Event.Phase = Phase ? std::string( Phase ).substr( 0, 64 ) : "unknown";
	Event.Component = Component ? std::string( Component ).substr( 0, 128 ) : "unknown";
	Event.Message = RedactUrlCredentials( Message ).substr( 0, 1024 );
	Event.ClusterSignature = FFmpegMessageSignature( Event.Message );

	const std::lock_guard<std::mutex> Guard( *_SegmentsMutex );
	// Keep repeated numeric variants of one FFmpeg warning in a single bounded
	// event, preserving its first message and the latest sample. Other media
	// events can no longer be displaced by hundreds of identical AAC warnings.
	for( int Offset = 0; Offset < _MediaEventRingCount; ++Offset )
	{
		auto& Previous = _MediaEventRing[(_MediaEventRingPos - 1 - Offset) % MEDIA_EVENT_RING_SIZE];
		if( Previous.Category != "ffmpeg" || Previous.Severity != Event.Severity ||
			Previous.Phase != Event.Phase || Previous.Component != Event.Component ||
			Previous.Generation != _InitGeneration ||
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
	Event.Sequence = ++_MediaEventSequence;
	Event.PacketSequence = _PacketDiagSequence;
	Event.Generation = _InitGeneration;
	Event.SegmentIndex = _CurrentSegmentIndex;
	Event.PartialIndex = _CurrentPartialIndex;
	_MediaEventRing[_MediaEventRingPos % MEDIA_EVENT_RING_SIZE] = std::move( Event );
	++_MediaEventRingPos;
	if( _MediaEventRingCount < MEDIA_EVENT_RING_SIZE )
		++_MediaEventRingCount;
	return 1;
}

void LiveOutputStream::Shutdown()
{
	if (_FormatContext)
	{
		if (_FormatContext->pb)
		{
			av_interleaved_write_frame(_FormatContext, nullptr);
			av_write_frame(_FormatContext, nullptr);
			avio_flush(_FormatContext->pb);
		}

		// Detach our custom AVIO so avformat doesn't try to close it
		_FormatContext->pb = nullptr;

		avformat_free_context(_FormatContext);
		_FormatContext = nullptr;
	}

	if (_AVIOContext)
	{
		// Buffer is owned by us, not av_free'd by avio
		av_free(_AVIOContext);
		_AVIOContext = nullptr;
	}

	if (_AVIOBuffer)
	{
		av_free(_AVIOBuffer);
		_AVIOBuffer = nullptr;
	}

	_CurrentBuffer.reset();
	if( _PendingVideoPacket )
		av_packet_free( &_PendingVideoPacket );
}

void LiveOutputStream::ResetForReconnect(InputStream* NewInputStream)
{
	// Tear down FFmpeg state but keep segments/init so HLS.js doesn't lose its reference
	if (_FormatContext)
	{
		// Do NOT call av_write_frame(NULL)/avio_flush here — the muxer may
		// be in an inconsistent state (e.g. audio stream created but no
		// audio packets written), and flushing can cause a crash.
		_FormatContext->pb = nullptr;
		avformat_free_context(_FormatContext);
		_FormatContext = nullptr;
	}

	if (_AVIOContext)
	{
		av_free(_AVIOContext);
		_AVIOContext = nullptr;
	}

	if (_AVIOBuffer)
	{
		av_free(_AVIOBuffer);
		_AVIOBuffer = nullptr;
	}

	_CurrentBuffer.reset();
	if( _PendingVideoPacket )
		av_packet_unref( _PendingVideoPacket );
	_PendingVideoActivityID = 0;
	_InputStream = NewInputStream;
	_HeaderWritten = false;
	_InitSegmentCaptured = false;
	_HasInitialDTS = false;
	_HasBFrames = false;
	_HasAudioStream = false;
	_NormalizeNoBFrameTimestamps = false;
	_TimestampNormalizationRejected = false;
	_InitialDTS = 0;
	_InitialTimestampUs = AV_NOPTS_VALUE;
	_LastInputDTS = AV_NOPTS_VALUE;
	_LastPacketDuration = 0;
	_LastOutputPacketDuration = 0;
	_LastWrittenDTS = AV_NOPTS_VALUE;
	_LastWrittenAudioDTS = AV_NOPTS_VALUE;
	_AudioInputStreamIndex = -1;
	_OutputSegmentStartDTS = AV_NOPTS_VALUE;
	_TimestampProbeSamples = 0;
	_TimestampProbeOutliers = 0;
	_TimestampProbeInputTicks = 0;
	_TimestampProbeDurationTicks = 0;
	_SourceTimestampOffset = 0;
	_TimestampCorrectionRemainder = 0;
	_LastTimestampPhaseError = 0;
	_LastTimestampCorrection = 0;
	_LastTimestampCorrectionSaturated = false;
	_PartialBufferOffset = 0;
	_CurrentPartialDuration = 0.0;
	_CurrentPartialAudioDuration = 0.0;
	_CurrentSegmentNominalDuration = 0.0;
	_CurrentPartialIsIndependent = false;
	_CurrentPartialHasPacket = false;
	_CurrentPartialKeyframeSeekSafe = false;
	_DiscontinuityPending = true;
	_DecodeCorruptionActive = false;
	_DecodeCorruptionActivityID = 0;
	_RecoveryPendingPublication = false;
	_RecoverySegmentIndex = -1;
	_RecoveryPartialIndex = -1;
	_RecoveryActivityID = 0;
	_RecoveryPacketSequence = 0;
	{
		const std::lock_guard<std::mutex> guard(*_SegmentsMutex);
		_StartupGraceStarted = {};
		_StreamEstablished = false;
		_StartupAcceptedVideoPackets = 0;
		_StartupDroppedVideoPackets = 0;
		_StartupRepairedVideoTimestamps = 0;
		_GenerationAcceptedVideoPacketsBaseline = _DiagAcceptedVideoPackets;
		_GenerationDroppedVideoPacketsBaseline = _DiagDroppedVideoPackets;
		_GenerationRepairedVideoTimestampsBaseline = _DiagRepairedVideoTimestamps;
		_EstablishedAcceptedVideoPacketsBaseline = _DiagAcceptedVideoPackets;
		_EstablishedDroppedVideoPacketsBaseline = _DiagDroppedVideoPackets;
		_EstablishedRepairedVideoTimestampsBaseline = _DiagRepairedVideoTimestamps;
		_InitGeneration++;
		_DiagVideoPhaseErrorMs = 0.0;
		_DiagVideoCorrectionMs = 0.0;
		_DiagLastVideoOutputUs = 0;
		_DiagLastAudioOutputUs = 0;
		_DiagHasVideoOutputTimestamp = false;
		_DiagHasAudioOutputTimestamp = false;
		_DiagInitStructureObserved = false;
		_DiagInitStructureValid = false;
		_DiagInitBoxCount = 0;
		_DiagInitFtypCount = 0;
		_DiagInitMoovCount = 0;
		_DiagInitStructureError = 0;
		_DiagInitErrorOffset = 0;
	}

	// Notify MSE subscribers of discontinuity before new init segment arrives
	if (_EventCallback)
	{
		LiveStreamEvent Event;
		Event.EventType = LiveStreamEvent::Discontinuity;
		Event.Generation = _InitGeneration;
		_EventCallback(Event);
	}

	LOG_INFO("[HLS] Live stream reconnect (generation %d), segments so far: %d, cumulative drift: %.1fms",
		_InitGeneration, _DiagTotalSegments,
		(_DiagTotalAccumulatedDuration - _DiagTotalDtsDuration) * 1000.0);

	// Remove any orphaned incomplete segment from the backlog —
	// otherwise HLS.js will try to load it and get a 404.
	{
		const std::lock_guard<std::mutex> guard(*_SegmentsMutex);
		if (!_StreamBacklog->empty() && !_StreamBacklog->back().Ready)
		{
			_StreamBacklog->pop_back();
		}
	}
}

int LiveOutputStream::WriteBuffer(void* Opaque, const uint8_t* Buffer, int BufferSize)
{
	LiveOutputStream* Self = static_cast<LiveOutputStream*>(Opaque);

	if (Self->_CurrentBuffer && BufferSize > 0)
	{
		Self->_CurrentBuffer->insert(Self->_CurrentBuffer->end(), Buffer, Buffer + BufferSize);
	}

	return BufferSize;
}

int64_t LiveOutputStream::SeekBuffer(void* Opaque, int64_t Offset, int Origin)
{
	// AVSEEK_SIZE: return total buffer size
	if (Origin == AVSEEK_SIZE)
	{
		LiveOutputStream* Self = static_cast<LiveOutputStream*>(Opaque);
		return Self->_CurrentBuffer ? (int64_t)Self->_CurrentBuffer->size() : 0;
	}

	// For fragmented MP4 with frag_custom, seeks shouldn't happen on the
	// output side after the header. Return 0 to indicate unseekable.
	return -1;
}

void LiveOutputStream::SetupMemoryIO()
{
	_AVIOBuffer = (uint8_t*)av_malloc(AVIOBufferSize);

	_AVIOContext = avio_alloc_context(
		_AVIOBuffer,
		AVIOBufferSize,
		1, // writable
		this,
		nullptr, // no read
		&LiveOutputStream::WriteBuffer,
		&LiveOutputStream::SeekBuffer
	);

	_AVIOContext->seekable = 0;

	_FormatContext->pb = _AVIOContext;
	_FormatContext->flags |= AVFMT_FLAG_CUSTOM_IO;
}

CameraStreamError LiveOutputStream::InitFormatContext()
{
	if (_FormatContext)
	{
		return CameraStreamError::Success;
	}

	_InputStream->Initialize();

	auto& InID = _InputStream->GetData();
	if (!InID.CodecContext)
	{
		STREAM_ERROR(NoStreamInput, 0);
	}

	int Result = avformat_alloc_output_context2(&_FormatContext, nullptr, "mp4", nullptr);
	if (Result < 0 || !_FormatContext)
	{
		STREAM_ERROR(UnknownError, Result);
	}

	AVStream* OutStream = avformat_new_stream(_FormatContext, nullptr);
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
	_HasBFrames = InID.CodecContext->has_b_frames > 0;
	AVStream* VideoInStream = InID.FormatContext->streams[InID.ChosenStreamIndex];
	AVCodecParameters* VideoParams = VideoInStream->codecpar;
	{
		const std::lock_guard<std::mutex> guard(*_SegmentsMutex);
		_DiagVideoCodec = avcodec_get_name(InID.CodecContext->codec_id);
		_DiagAudioCodec.clear();
		_DiagInputFormat = InID.FormatContext->iformat && InID.FormatContext->iformat->name ?
			InID.FormatContext->iformat->name : "";
		_DiagVideoProfile = VideoParams->profile;
		_DiagVideoLevel = VideoParams->level;
		_DiagVideoWidth = VideoParams->width;
		_DiagVideoHeight = VideoParams->height;
		_DiagVideoTimeBaseNum = VideoInStream->time_base.num;
		_DiagVideoTimeBaseDen = VideoInStream->time_base.den;
		_DiagVideoExtradataBytes = VideoParams->extradata_size;
		_DiagVideoExtradataHash = VideoParams->extradata && VideoParams->extradata_size > 0 ?
			HashBytes(VideoParams->extradata, VideoParams->extradata_size) : 0;
		_DiagAudioProfile = 0;
		_DiagAudioSampleRate = 0;
		_DiagAudioChannels = 0;
		_DiagAudioTimeBaseNum = 0;
		_DiagAudioTimeBaseDen = 0;
		_DiagAudioExtradataBytes = 0;
		_DiagAudioExtradataHash = 0;
	}
	LOG_INFO("[HLS] Camera stream %s B-frames (has_b_frames=%d)",
		_HasBFrames ? "has" : "does not have", InID.CodecContext->has_b_frames);

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
		AVStream* AudioOutStream = avformat_new_stream(_FormatContext, nullptr);
		if (!AudioOutStream || avcodec_parameters_copy(AudioOutStream->codecpar, AudioInStream->codecpar) < 0)
		{
			STREAM_ERROR(DecoderReceiverError, 0);
		}
		AudioOutStream->codecpar->codec_tag = 0;
		AudioOutStream->time_base = AudioInStream->time_base;
		_HasAudioStream = true;
		_AudioInputStreamIndex = InID.ChosenAudioStreamIndex;
		{
			const std::lock_guard<std::mutex> guard(*_SegmentsMutex);
			_DiagAudioCodec = avcodec_get_name(AudioInStream->codecpar->codec_id);
			_DiagAudioProfile = AudioInStream->codecpar->profile;
			_DiagAudioSampleRate = AudioInStream->codecpar->sample_rate;
			_DiagAudioChannels = AudioInStream->codecpar->ch_layout.nb_channels;
			_DiagAudioTimeBaseNum = AudioInStream->time_base.num;
			_DiagAudioTimeBaseDen = AudioInStream->time_base.den;
			_DiagAudioExtradataBytes = AudioInStream->codecpar->extradata_size;
			_DiagAudioExtradataHash = AudioInStream->codecpar->extradata &&
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
	_DecodeCorruptionActivityID = (std::max)(
		_DecodeCorruptionActivityID, _CurrentDiagnosticActivity);
	_RecoveryPendingPublication = false;
	_RecoverySegmentIndex = -1;
	_RecoveryPartialIndex = -1;
	_RecoveryActivityID = 0;
	_RecoveryPacketSequence = 0;
	if (_DecodeCorruptionActive)
		return;

	_DecodeCorruptionActive = true;
	{
		MediaDiagnosticEvent Event;
		Event.TimestampUnixMs = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::system_clock::now().time_since_epoch()).count();
		Event.ElapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now() - _PacketDiagEpoch).count();
		Event.ActivityID = _CurrentDiagnosticActivity;
		Event.Category = "decode";
		Event.Severity = "error";
		Event.Phase = "presentation";
		Event.Component = "decoder";
		Event.Message = "Corrupt video detected; presentation held until recovery keyframe (flags=" +
			std::to_string( ErrorFlags ) + ")";
		Event.Disposition = "decodeCorruption";
		const std::lock_guard<std::mutex> Guard( *_SegmentsMutex );
		++_DiagDecodeCorruptionEvents;
		Event.Sequence = ++_MediaEventSequence;
		Event.PacketSequence = _PacketDiagSequence;
		Event.Generation = _InitGeneration;
		Event.SegmentIndex = _CurrentSegmentIndex;
		Event.PartialIndex = _CurrentPartialIndex;
		_MediaEventRing[_MediaEventRingPos % MEDIA_EVENT_RING_SIZE] = std::move( Event );
		++_MediaEventRingPos;
		if( _MediaEventRingCount < MEDIA_EVENT_RING_SIZE )
			++_MediaEventRingCount;
	}
	LOG_WARNING("[HLS] Camera %d detected corrupt video; holding presentation until the next keyframe (decode_error_flags=0x%x)",
		_InputStream->GetSourceId(), ErrorFlags);

	if (_EventCallback)
	{
		LiveStreamEvent Event;
		Event.EventType = LiveStreamEvent::DecodeCorruption;
		Event.SegmentIndex = _CurrentSegmentIndex;
		Event.PartIndex = _CurrentPartialIndex;
		Event.Generation = _InitGeneration;
		Event.DecodeErrorFlags = ErrorFlags;
		_EventCallback(Event);
	}
}

CameraStreamError LiveOutputStream::WriteInterleavedPacket(const AVPacket* Packet)
{
	if( !Packet || !_InputStream )
		return CameraStreamError::Success;

	const auto& InputData = _InputStream->GetData();
	const bool IsVideo = Packet->stream_index == InputData.ChosenStreamIndex;
	if( !IsVideo || !_PendingVideoPacket )
		return WritePacketWithKnownDuration( Packet, _CurrentDiagnosticActivity );

	{
		const std::lock_guard<std::mutex> Guard(*_SegmentsMutex);
		if( _StartupGraceStarted.time_since_epoch().count() == 0 )
			_StartupGraceStarted = std::chrono::steady_clock::now();
	}

	if( _PendingVideoPacket->data || _PendingVideoPacket->size > 0 )
	{
		const int64_t SourceDuration = _PendingVideoPacket->duration;
		if( _PendingVideoPacket->dts != AV_NOPTS_VALUE && Packet->dts != AV_NOPTS_VALUE )
		{
			const int64_t Delta = Packet->dts - _PendingVideoPacket->dts;
			const AVRational Timebase = InputData.FormatContext->
				streams[InputData.ChosenStreamIndex]->time_base;
			const int64_t MaximumLookahead = av_rescale_q(
				2 * AV_TIME_BASE, AV_TIME_BASE_Q, Timebase );
			// The next raw DTS is useful for generic VFR streams, but it is exactly
			// the noisy signal the Reolink profile is meant to reject. Keep that
			// profile's declared/advertised cadence intact for the PLL below.
			if( !_AllowTimestampNormalization && Delta > 0 && Delta <= MaximumLookahead )
				_PendingVideoPacket->duration = Delta;
		}

		const uint64_t CurrentActivityID = _CurrentDiagnosticActivity;
		FFmpegLogContextScope PendingLogContext( _InputStream->GetSourceId(),
			"live-mux", this, _PendingVideoActivityID );
		CameraStreamError Result = WritePacketWithKnownDuration(
			_PendingVideoPacket, _PendingVideoActivityID, SourceDuration );
		_CurrentDiagnosticActivity = CurrentActivityID;
		av_packet_unref( _PendingVideoPacket );
		_PendingVideoActivityID = 0;
		if( Result != CameraStreamError::Success )
			return Result;
	}

	const int RefResult = av_packet_ref( _PendingVideoPacket, Packet );
	if( RefResult < 0 )
	{
		STREAM_ERROR( RefError, RefResult );
	}
	_PendingVideoActivityID = _CurrentDiagnosticActivity;
	return CameraStreamError::Success;
}

CameraStreamError LiveOutputStream::WritePacketWithKnownDuration(
	const AVPacket* Packet, uint64_t ActivityID, int64_t SourceDuration )
{
	_CurrentDiagnosticActivity = ActivityID;
	if (!_FormatContext)
	{
		CameraStreamError Result = InitFormatContext();
		if (Result != CameraStreamError::Success)
		{
			return Result;
		}
	}
	const bool IsAudio = _HasAudioStream && Packet->stream_index == _AudioInputStreamIndex;
	const bool IsVideo = Packet->stream_index == _InputStream->GetData().ChosenStreamIndex;
	if (!IsAudio && !IsVideo)
		return CameraStreamError::Success;
	AVRational InputTimebase =
		_InputStream->GetData().FormatContext->streams[Packet->stream_index]->time_base;
	const bool IsVideoKeyframe = IsVideo && (Packet->flags & AV_PKT_FLAG_KEY);
	if (IsVideo && (Packet->flags & AV_PKT_FLAG_CORRUPT))
		NotifyDecodeCorruption(0);
	const bool IsRecoveryCandidate = IsVideoKeyframe && _DecodeCorruptionActive &&
		_CurrentDiagnosticActivity > _DecodeCorruptionActivityID &&
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
	const AVCodecID PacketCodec = _InputStream->GetData().FormatContext->
		streams[Packet->stream_index]->codecpar->codec_id;
	const PacketStructure Structure = AnalysePacketStructure(
		PacketCodec, Packet->data, Packet->size > 0 ? (size_t)Packet->size : 0);
	const int64_t ArrivalMs = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now() - _PacketDiagEpoch).count();
	const uint64_t PacketSequence = ++_PacketDiagSequence;
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
		Entry.Sequence = PacketSequence;
		Entry.Generation = _InitGeneration;
		Entry.SegmentIndex = SegmentIndex >= 0 ? SegmentIndex : _CurrentSegmentIndex;
		Entry.PartialIndex = PartialIndex >= 0 ? PartialIndex : _CurrentPartialIndex;
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
		const std::string DispositionText = Disposition ? Disposition : "unknown";
		Entry.Disposition = DispositionText;
		if (auto Capture = ActivePacketCapture())
			Capture->AddMetadata(std::format(
				"{{\"type\":\"decision\",\"activityId\":{},\"packetSequence\":{},\"generation\":{},\"segment\":{},\"part\":{},\"audio\":{},\"disposition\":\"{}\",\"hashFnv64\":\"{:016x}\",\"sourceDtsUs\":{},\"sourcePtsUs\":{},\"sourceDurationUs\":{},\"hasOutput\":{},\"outputDtsUs\":{},\"outputPtsUs\":{},\"outputDurationUs\":{},\"normalized\":{},\"timestampRepaired\":{}",
				_CurrentDiagnosticActivity, PacketSequence, Entry.Generation,
				Entry.SegmentIndex, Entry.PartialIndex, Entry.Audio, DispositionText,
				Entry.PayloadHash, Entry.SourceDtsUs, Entry.SourcePtsUs,
				Entry.SourceDurationUs, HasOutput, Entry.OutputDtsUs,
				Entry.OutputPtsUs, Entry.OutputDurationUs,
				Entry.TimestampNormalized, Entry.TimestampRepaired));
		const std::lock_guard<std::mutex> guard(*_SegmentsMutex);
		if( DispositionText == "waitingForKeyframe" ) ++_DiagWaitingForKeyframePackets;
		else if( DispositionText == "missingTimestamp" ) ++_DiagMissingTimestampPackets;
		else if( DispositionText == "beforeVideoEpoch" ) ++_DiagBeforeVideoEpochPackets;
		else if( DispositionText == "negativeTimestamp" ) ++_DiagNegativeTimestampPackets;
		else if( DispositionText == "nonMonotonicInput" ) ++_DiagNonMonotonicInputPackets;
		else if( DispositionText == "noMuxBuffer" ) ++_DiagNoMuxBufferPackets;
		else if( DispositionText == "nonMonotonicOutput" ) ++_DiagNonMonotonicOutputPackets;
		else if( DispositionText == "muxError" ) ++_DiagMuxErrorPackets;
		_PacketDiagRing[_PacketDiagRingPos % PACKET_DIAG_RING_SIZE] = std::move(Entry);
		++_PacketDiagRingPos;
		if (_PacketDiagRingCount < PACKET_DIAG_RING_SIZE)
			++_PacketDiagRingCount;

		const bool Noteworthy = DispositionText != "written" &&
			DispositionText != "waitingForKeyframe";
		if( Noteworthy )
		{
			MediaDiagnosticEvent Event;
			Event.Sequence = ++_MediaEventSequence;
			Event.ActivityID = _CurrentDiagnosticActivity;
			Event.PacketSequence = PacketSequence;
			Event.TimestampUnixMs = std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::system_clock::now().time_since_epoch()).count();
			Event.ElapsedMs = ArrivalMs;
			Event.Generation = _InitGeneration;
			Event.SegmentIndex = SegmentIndex >= 0 ? SegmentIndex : _CurrentSegmentIndex;
			Event.PartialIndex = PartialIndex >= 0 ? PartialIndex : _CurrentPartialIndex;
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
			_MediaEventRing[_MediaEventRingPos % MEDIA_EVENT_RING_SIZE] = std::move( Event );
			++_MediaEventRingPos;
			if( _MediaEventRingCount < MEDIA_EVENT_RING_SIZE )
				++_MediaEventRingCount;
		}
	};
	// Never mutate segment state around a keyframe that cannot be placed on the
	// decode timeline. Continue waiting for the next usable random-access point.
	if (IsVideo && Packet->dts == AV_NOPTS_VALUE)
	{
		{
			const std::lock_guard<std::mutex> guard(*_SegmentsMutex);
			++_DiagMissingVideoDtsPackets;
			++_DiagDroppedVideoPackets;
		}
		++_SegmentMissingVideoDtsPackets;
		++_SegmentDroppedVideoPackets;
		RecordPacket("missingVideoDts");
		NotifyDecodeCorruption(0);
		return CameraStreamError::Success;
	}

	// Until the first usable random-access frame, ignore both tracks. This keeps
	// the initial timestamp anchored to video and preserves the old join behavior.
	if (!_HeaderWritten && !IsVideoKeyframe)
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
	if (_InitialTimestampUs == AV_NOPTS_VALUE && IsVideo)
		_InitialTimestampUs = PacketTimestampUs;
	if (_InitialTimestampUs == AV_NOPTS_VALUE || PacketTimestampUs < _InitialTimestampUs)
	{
		if (IsVideo)
		{
			const std::lock_guard<std::mutex> guard(*_SegmentsMutex);
			++_DiagDroppedVideoPackets;
			++_SegmentDroppedVideoPackets;
		}
		RecordPacket("beforeVideoEpoch");
		av_packet_unref(&PacketCopy);
		return CameraStreamError::Success;
	}
	PacketCopy.dts = av_rescale_q(PacketTimestampUs - _InitialTimestampUs, AV_TIME_BASE_Q, InputTimebase);
	PacketCopy.pts = av_rescale_q(
		av_rescale_q(PacketCopy.pts, InputTimebase, AV_TIME_BASE_Q) - _InitialTimestampUs,
		AV_TIME_BASE_Q, InputTimebase);

	// Guard against negative timestamps from B-frame reordering at stream start
	if (PacketCopy.dts != AV_NOPTS_VALUE && PacketCopy.dts < 0)
	{
		if (IsVideo)
		{
			const std::lock_guard<std::mutex> guard(*_SegmentsMutex);
			++_DiagDroppedVideoPackets;
			++_SegmentDroppedVideoPackets;
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
	if( IsVideo && !_HasBFrames && _AllowTimestampNormalization )
	{
		const AVRational Framerate = _InputStream->GetData().CodecContext->framerate;
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
	// Also sample the relationship between source DTS deltas
	// and declared frame durations. We only replace the source clock when it is
	// demonstrably jittery but agrees with the durations over the whole window;
	// genuine variable-frame-rate streams therefore retain their source timing.
	if (IsVideo && PacketCopy.dts != AV_NOPTS_VALUE)
	{
		if (_LastInputDTS != AV_NOPTS_VALUE && PacketCopy.dts <= _LastInputDTS)
		{
			const bool CanRepairRegression =
				!_HasBFrames && _AllowTimestampNormalization &&
				!_TimestampNormalizationRejected && _NormalizeNoBFrameTimestamps &&
				_LastWrittenDTS != AV_NOPTS_VALUE && _LastPacketDuration > 0;
			if (!CanRepairRegression)
			{
				uint64_t DroppedVideoPackets;
				{
					const std::lock_guard<std::mutex> guard(*_SegmentsMutex);
					DroppedVideoPackets = ++_DiagDroppedVideoPackets;
				}
				++_SegmentDroppedVideoPackets;
				if (DroppedVideoPackets <= 5 || (DroppedVideoPackets % 100) == 0)
				{
					LOG_WARNING(
						"[HLS] Source %d dropped non-monotonic video packet: previous DTS=%lld, DTS=%lld, PTS=%lld, duration=%lld, flags=0x%x, size=%d, normalizing=%d",
						_InputStream->GetSourceId(), (long long)_LastInputDTS,
						(long long)PacketCopy.dts, (long long)PacketCopy.pts,
						(long long)PacketCopy.duration, PacketCopy.flags, PacketCopy.size,
						_NormalizeNoBFrameTimestamps ? 1 : 0);
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
				const std::lock_guard<std::mutex> guard(*_SegmentsMutex);
				RepairedVideoTimestamps = ++_DiagRepairedVideoTimestamps;
			}
			if (RepairedVideoTimestamps <= 5 || (RepairedVideoTimestamps % 100) == 0)
			{
				LOG_WARNING(
					"[HLS] Source %d repaired non-monotonic video timestamp: previous DTS=%lld, DTS=%lld, PTS=%lld, duration=%lld, flags=0x%x, size=%d",
					_InputStream->GetSourceId(), (long long)_LastInputDTS,
					(long long)PacketCopy.dts, (long long)PacketCopy.pts,
					(long long)PacketCopy.duration, PacketCopy.flags, PacketCopy.size);
			}
		}

		if (!_HasBFrames && _AllowTimestampNormalization && !_TimestampNormalizationRejected &&
			_LastInputDTS != AV_NOPTS_VALUE && _LastPacketDuration > 0)
		{
			const int64_t InputDelta = PacketCopy.dts - _LastInputDTS;
			int64_t DeltaError = InputDelta - _LastPacketDuration;
			if (DeltaError < 0)
				DeltaError = -DeltaError;

			++_TimestampProbeSamples;
			_TimestampProbeInputTicks += InputDelta;
			_TimestampProbeDurationTicks += _LastPacketDuration;
			if (DeltaError * 2 > _LastPacketDuration)
				++_TimestampProbeOutliers;
			if (!_NormalizeNoBFrameTimestamps && _TimestampProbeSamples >= 20)
			{
				int64_t TotalError = _TimestampProbeInputTicks - _TimestampProbeDurationTicks;
				if (TotalError < 0)
					TotalError = -TotalError;

				const bool AverageCadenceMatches =
					TotalError * 100 <= _TimestampProbeDurationTicks * 15;
				const bool SourceClockIsJittery =
					_TimestampProbeOutliers * 5 >= _TimestampProbeSamples;
				if (AverageCadenceMatches && SourceClockIsJittery)
				{
					_NormalizeNoBFrameTimestamps = true;
					_SourceTimestampOffset = 0;
					LOG_INFO("[HLS] Normalizing jittery no-B-frame timestamps (%d/%d outliers)",
						_TimestampProbeOutliers, _TimestampProbeSamples);
					_TimestampProbeSamples = 0;
					_TimestampProbeOutliers = 0;
					_TimestampProbeInputTicks = 0;
					_TimestampProbeDurationTicks = 0;
				}
				else if (_TimestampProbeSamples >= 120)
				{
					_TimestampProbeSamples = 0;
					_TimestampProbeOutliers = 0;
					_TimestampProbeInputTicks = 0;
					_TimestampProbeDurationTicks = 0;
				}
			}
			else if (_NormalizeNoBFrameTimestamps)
			{
				// Reolink DTS can wander by several seconds and then converge again.
				// Judge cadence over a long interval rather than using a fixed
				// phase-error budget, which incorrectly rejected camera 11.
				const int64_t ProbeDurationUs = av_rescale_q(
					_TimestampProbeDurationTicks, InputTimebase, AV_TIME_BASE_Q);
				if (ProbeDurationUs >= 120 * AV_TIME_BASE)
				{
					const int64_t SignedError =
						_TimestampProbeInputTicks - _TimestampProbeDurationTicks;
					const int64_t AbsoluteError = SignedError < 0 ? -SignedError : SignedError;
					const bool WindowCadenceDrifted =
						AbsoluteError * 100 > _TimestampProbeDurationTicks * 5;
					const double DriftMs =
						(double)SignedError * InputTimebase.num * 1000.0 / InputTimebase.den;

					if (WindowCadenceDrifted)
					{
						_NormalizeNoBFrameTimestamps = false;
						_TimestampNormalizationRejected = true;
						_SourceTimestampOffset =
							(_LastWrittenDTS + _LastOutputPacketDuration) - PacketCopy.dts;
						LOG_WARNING(
							"[HLS] Source %d stopped timestamp normalization: %.1fms drift over %.1fs (%d samples)",
							_InputStream->GetSourceId(), DriftMs, ProbeDurationUs / 1000000.0,
							_TimestampProbeSamples);
					}

					_TimestampProbeSamples = 0;
					_TimestampProbeOutliers = 0;
					_TimestampProbeInputTicks = 0;
					_TimestampProbeDurationTicks = 0;
				}
			}
		}

		_LastInputDTS = PacketCopy.dts;
	}

	// Segment state must only change after the keyframe has passed timestamp
	// acceptance. Otherwise a rejected keyframe can advertise dependent video as
	// an independent fragment.
	if (IsVideoKeyframe)
	{
		// Enforce a minimum segment duration of 1 second. Cameras like Tapo send
		// keyframes every ~50-100ms, which would create unusable micro-segments.
		const bool ShouldSplit = !_HeaderWritten || _CurrentSegmentDuration >= 1.0;
		// A recovery random-access point must be the first video sample in the
		// partial that releases presentation. Do not label a mixed partial as
		// independent merely because it contains a later keyframe.
		if( IsRecoveryCandidate && _HeaderWritten && !ShouldSplit &&
			_CurrentPartialHasPacket )
		{
			if( !FlushPartialSegment( _CurrentPartialIsIndependent ) )
			{
				av_packet_unref(&PacketCopy);
				return CameraStreamError::WriteFailed;
			}
		}
		if (ShouldSplit)
		{
			if (_HeaderWritten)
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

	if (!_HeaderWritten || !_CurrentBuffer)
	{
		RecordPacket("noMuxBuffer");
		av_packet_unref(&PacketCopy);
		return CameraStreamError::Success;
	}
	if (RepairedRegressingVideoTimestamp)
		++_SegmentRepairedVideoTimestamps;

	// A missing duration must not switch a normalized stream back to its raw
	// timestamp domain for one packet. Reuse the last validated duration.
	if (IsVideo && _NormalizeNoBFrameTimestamps && PacketCopy.duration == 0 && _LastPacketDuration > 0)
	{
		PacketCopy.duration = _LastPacketDuration;
		DurationSynthesized = true;
	}
	const int64_t NominalVideoDuration = IsVideo ? PacketCopy.duration : 0;
	const int64_t SourceNominalVideoDuration = IsVideo && OriginalSourceDuration > 0 &&
		OriginalSourceDuration <= INT_MAX ?
		OriginalSourceDuration : NominalVideoDuration;

	// Once the Reolink source has met the guarded jitter test, build a continuous
	// output clock from declared durations, then gently steer it towards the raw
	// source clock shared with audio. A duration-only clock runs at a measurably
	// different rate on some cameras, eventually placing AAC seconds ahead of
	// video. The ten-second response window rejects short DTS wander while the 5%
	// limit keeps every output interval positive and close to the nominal cadence.
	// Otherwise preserve source deltas, applying an offset only when transitioning
	// out of normalized mode.
	if (IsVideo && !_HasBFrames)
	{
		_LastTimestampPhaseError = 0;
		_LastTimestampCorrection = 0;
		_LastTimestampCorrectionSaturated = false;
		if (_NormalizeNoBFrameTimestamps && _LastWrittenDTS != AV_NOPTS_VALUE &&
			_LastPacketDuration > 0 && _LastOutputPacketDuration > 0)
		{
			const int64_t RawDTS = PacketCopy.dts;
			// The previous packet advertised this duration, so using it here keeps
			// FFmpeg's fragment boundary rewrite exactly on the same timeline.
			const int64_t ExpectedDTS = _LastWrittenDTS + _LastOutputPacketDuration;
			const int64_t TargetDTS = RawDTS + _SourceTimestampOffset;
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
					BoundedPhaseError * NominalVideoDuration + _TimestampCorrectionRemainder;
				Correction = Numerator / ResponseWindowTicks;
				_TimestampCorrectionRemainder = Numerator % ResponseWindowTicks;
			}
			const int64_t UnclampedCorrection = Correction;
			Correction = (std::max)(-MaxCorrection, (std::min)(Correction, MaxCorrection));
			PacketCopy.dts = ExpectedDTS;
			PacketCopy.duration = NominalVideoDuration + Correction;
			_LastTimestampPhaseError = PhaseError;
			_LastTimestampCorrection = Correction;
			_LastTimestampCorrectionSaturated = Correction != UnclampedCorrection ||
				PhaseError > ResponseWindowTicks / 10 || PhaseError < -ResponseWindowTicks / 10;
			TimestampNormalized = true;
			CorrectionSaturated = _LastTimestampCorrectionSaturated;
		}
		else if (_SourceTimestampOffset != 0 && PacketCopy.dts != AV_NOPTS_VALUE)
			PacketCopy.dts += _SourceTimestampOffset;
		PacketCopy.pts = PacketCopy.dts;
	}

	PacketCopy.stream_index = IsAudio ? 1 : 0;
	PacketCopy.pos = -1;

	// B-frame streams retain their source timing, so keep an output-side
	// monotonicity check as a final muxer safety net.
	const int64_t LastStreamDTS = IsAudio ? _LastWrittenAudioDTS : _LastWrittenDTS;
	if (LastStreamDTS != AV_NOPTS_VALUE && PacketCopy.dts <= LastStreamDTS)
	{
		if (IsVideo)
		{
			const std::lock_guard<std::mutex> guard(*_SegmentsMutex);
			++_DiagDroppedVideoPackets;
			++_SegmentDroppedVideoPackets;
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
		_LastWrittenDTS = PacketCopy.dts;
		{
			const std::lock_guard<std::mutex> guard(*_SegmentsMutex);
			++_DiagAcceptedVideoPackets;
			if (IsVideoKeyframe)
				++_DiagAcceptedVideoKeyframes;
			if (PacketCopy.flags & AV_PKT_FLAG_CORRUPT)
				++_DiagCorruptVideoPackets;
			_DiagVideoPhaseErrorMs = (double)_LastTimestampPhaseError *
				InputTimebase.num * 1000.0 / InputTimebase.den;
			_DiagVideoCorrectionMs = (double)_LastTimestampCorrection *
				InputTimebase.num * 1000.0 / InputTimebase.den;
			_DiagLastVideoOutputUs = av_rescale_q(PacketCopy.dts, InputTimebase, AV_TIME_BASE_Q);
			_DiagHasVideoOutputTimestamp = true;
			if (_LastTimestampCorrectionSaturated)
				++_DiagTimestampCorrectionSaturatedPackets;
		}
		++_SegmentAcceptedVideoPackets;
		if (IsVideoKeyframe)
		{
			++_SegmentAcceptedVideoKeyframes;
		}
		if (PacketCopy.flags & AV_PKT_FLAG_CORRUPT)
		{
			++_SegmentCorruptVideoPackets;
		}
		if (_NormalizeNoBFrameTimestamps)
			_SegmentTimestampNormalizationActive = true;
		if (NominalVideoDuration > 0)
			_LastPacketDuration = NominalVideoDuration;
		if (PacketCopy.duration > 0)
			_LastOutputPacketDuration = PacketCopy.duration;
	}
	else
	{
		_LastWrittenAudioDTS = PacketCopy.dts;
		const std::lock_guard<std::mutex> guard(*_SegmentsMutex);
		_DiagLastAudioOutputUs = av_rescale_q(PacketCopy.dts, InputTimebase, AV_TIME_BASE_Q);
		_DiagHasAudioOutputTimestamp = true;
	}

	if (IsVideo && _OutputSegmentStartDTS == AV_NOPTS_VALUE)
		_OutputSegmentStartDTS = PacketCopy.dts;

	if (IsVideo)
	{
		AVRational TimeBase = _FormatContext->streams[0]->time_base;
		double PacketDurationSec = (double)(PacketCopy.duration * TimeBase.num) / TimeBase.den;
		_CurrentSegmentDuration += PacketDurationSec;
		const double SourceNominalPacketDurationSec =
			(double)(SourceNominalVideoDuration * TimeBase.num) / TimeBase.den;
		_CurrentSegmentNominalDuration += SourceNominalPacketDurationSec;
		_CurrentPartialDuration += PacketDurationSec;

		// With multiplexed audio, SourceBuffer range boundaries are not guaranteed
		// to identify the exact video RAP timestamp.
		const bool PacketKeyframeSeekSafe =
			!_HasAudioStream && !_HasBFrames && _NormalizeNoBFrameTimestamps;
		if (!_CurrentPartialHasPacket)
		{
			_CurrentPartialHasPacket = true;
			_CurrentPartialIsIndependent = IsVideoKeyframe;
			_CurrentPartialKeyframeSeekSafe = PacketKeyframeSeekSafe;
		}
		else
		{
			_CurrentPartialKeyframeSeekSafe =
				_CurrentPartialKeyframeSeekSafe && PacketKeyframeSeekSafe;
		}
	}
	else if (IsAudio)
	{
		double PacketDurationSec = (double)(PacketCopy.duration * InputTimebase.num) / InputTimebase.den;
		_CurrentPartialAudioDuration += PacketDurationSec;
	}

	const int TraceSegmentIndex = _CurrentSegmentIndex;
	const int TracePartialIndex = _CurrentPartialIndex;
	const int64_t TraceOutputDts = PacketCopy.dts;
	const int64_t TraceOutputPts = PacketCopy.pts;
	const int64_t TraceOutputDuration = PacketCopy.duration;
	av_packet_rescale_ts(&PacketCopy, InputTimebase,
		_FormatContext->streams[PacketCopy.stream_index]->time_base);
	// Multi-track fMP4 needs FFmpeg's interleaver to emit a valid fragment.
	Result = av_interleaved_write_frame(_FormatContext, &PacketCopy);
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
		_RecoveryPendingPublication = true;
		_RecoverySegmentIndex = TraceSegmentIndex;
		_RecoveryPartialIndex = TracePartialIndex;
		_RecoveryActivityID = _CurrentDiagnosticActivity;
		_RecoveryPacketSequence = PacketSequence;
	}
	if( IsVideoKeyframe )
		MarkStreamEstablished();
	_SegmentPacketPayloadHash = HashBytes(
		reinterpret_cast<const uint8_t*>(&PayloadHash), sizeof(PayloadHash),
		_SegmentPacketPayloadHash);

	// Flush a partial segment when we've accumulated enough duration
	if (IsVideo && _CurrentPartialDuration >= _PartialTargetDuration)
	{
		if( !FlushPartialSegment(_CurrentPartialIsIndependent) )
			return CameraStreamError::WriteFailed;
	}

	return CameraStreamError::Success;
}

bool LiveOutputStream::FlushPartialSegment(bool IsIndependent)
{
	if (!_FormatContext || !_CurrentBuffer)
		return true;

	// Flush current fragment data into the buffer
	const int InterleaveResult = av_interleaved_write_frame(_FormatContext, nullptr);
	const int FragmentResult = av_write_frame(_FormatContext, nullptr);
	avio_flush(_FormatContext->pb);
	if( InterleaveResult < 0 || FragmentResult < 0 )
	{
		const int Error = InterleaveResult < 0 ? InterleaveResult : FragmentResult;
		LOG_WARNING("[HLS] Camera %d failed to flush partial %d/%d: ffmpeg error %d",
			_InputStream ? _InputStream->GetSourceId() : -1,
			_CurrentSegmentIndex, _CurrentPartialIndex, Error);
		CaptureDiagnosticAnomaly("fragmentFlushError");
		_RecoveryPendingPublication = false;
		return false;
	}

	// Only create a partial if we actually accumulated data since the last flush
	size_t CurrentSize = _CurrentBuffer->size();
	// windows.h defines max as a macro in this translation unit.
	double PartialDuration = (std::max)(_CurrentPartialDuration, _CurrentPartialAudioDuration);
	if (CurrentSize <= _PartialBufferOffset)
		return true;
	// Some RTSP sources omit AAC packet durations. The fragment still needs to
	// be delivered; use the configured target as a conservative playlist value.
	if (PartialDuration <= 0.0)
		PartialDuration = _PartialTargetDuration;

	// Create a partial that references the byte range [_PartialBufferOffset, CurrentSize)
	// within the single segment buffer
	auto PartialData = std::make_shared<std::vector<uint8_t>>(
		_CurrentBuffer->begin() + _PartialBufferOffset,
		_CurrentBuffer->begin() + CurrentSize
	);

	_PartialBufferOffset = CurrentSize;

	LiveStreamPartialSegment Partial;
	Partial.Data = PartialData;
	Partial.Duration = PartialDuration;
	Partial.PartIndex = _CurrentPartialIndex;
	Partial.Independent = IsIndependent;
	const bool KeyframeSeekSafe = _CurrentPartialKeyframeSeekSafe;
	const IsoBmffStructure PartialStructure = AnalyseIsoBmff(
		PartialData->data(), PartialData->size(), false);
	FragmentDiagEntry FragmentDiag;
	FragmentDiag.Generation = _InitGeneration;
	FragmentDiag.SegmentIndex = _CurrentSegmentIndex;
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
		const std::lock_guard<std::mutex> guard(*_SegmentsMutex);

		if (!_StreamBacklog->empty())
		{
			_StreamBacklog->back().Partials.push_back(Partial);
		}
		_FragmentDiagRing[_FragmentDiagRingPos % FRAGMENT_DIAG_RING_SIZE] = FragmentDiag;
		++_FragmentDiagRingPos;
		if (_FragmentDiagRingCount < FRAGMENT_DIAG_RING_SIZE)
			++_FragmentDiagRingCount;
	}
	if (!PartialStructure.Valid)
		CaptureDiagnosticAnomaly("invalidFragmentStructure");
	const bool PublishesRecovery = _RecoveryPendingPublication &&
		_RecoverySegmentIndex == _CurrentSegmentIndex &&
		_RecoveryPartialIndex == Partial.PartIndex && Partial.Independent &&
		PartialStructure.Valid;

	_CurrentPartialIndex++;
	_CurrentPartialDuration = 0.0;
	_CurrentPartialAudioDuration = 0.0;
	_CurrentPartialIsIndependent = false;
	_CurrentPartialHasPacket = false;
	_CurrentPartialKeyframeSeekSafe = false;
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
	if (_EventCallback)
	{
		LiveStreamEvent Event;
		Event.EventType = LiveStreamEvent::PartialReady;
		Event.SegmentIndex = _CurrentSegmentIndex;
		Event.PartIndex = Partial.PartIndex;
		Event.Data = Partial.Data;
		Event.Duration = Partial.Duration;
		Event.Independent = Partial.Independent;
		Event.KeyframeSeekSafe = KeyframeSeekSafe;
		Event.Generation = _InitGeneration;
		Event.ByteSize = Event.Data ? Event.Data->size() : 0;
		Event.TransportHash = Event.ByteSize > 0 ?
			HashBytes32(Event.Data->data(), Event.Data->size()) : 0;
		_EventCallback(Event);
	}
	return true;
}

void LiveOutputStream::PublishDecodeRecovery(int SegmentIndex, int PartialIndex)
{
	if( !_RecoveryPendingPublication || !_DecodeCorruptionActive )
		return;

	LiveStreamEvent Event;
	Event.EventType = LiveStreamEvent::DecodeRecovery;
	Event.SegmentIndex = SegmentIndex;
	Event.PartIndex = PartialIndex;
	Event.Generation = _InitGeneration;
	{
		MediaDiagnosticEvent Diagnostic;
		Diagnostic.TimestampUnixMs = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::system_clock::now().time_since_epoch()).count();
		Diagnostic.ElapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now() - _PacketDiagEpoch).count();
		Diagnostic.ActivityID = _RecoveryActivityID;
		Diagnostic.PacketSequence = _RecoveryPacketSequence;
		Diagnostic.Generation = _InitGeneration;
		Diagnostic.SegmentIndex = SegmentIndex;
		Diagnostic.PartialIndex = PartialIndex;
		Diagnostic.Category = "decode";
		Diagnostic.Severity = "info";
		Diagnostic.Phase = "presentation";
		Diagnostic.Component = "muxer";
		Diagnostic.Message = "Published an independently decodable recovery fragment";
		Diagnostic.Disposition = "decodeRecovery";
		const std::lock_guard<std::mutex> Guard( *_SegmentsMutex );
		++_DiagDecodeRecoveryEvents;
		Diagnostic.Sequence = ++_MediaEventSequence;
		_MediaEventRing[_MediaEventRingPos % MEDIA_EVENT_RING_SIZE] = std::move(Diagnostic);
		++_MediaEventRingPos;
		if( _MediaEventRingCount < MEDIA_EVENT_RING_SIZE )
			++_MediaEventRingCount;
	}
	_DecodeCorruptionActive = false;
	_RecoveryPendingPublication = false;
	_RecoverySegmentIndex = -1;
	_RecoveryPartialIndex = -1;
	_RecoveryActivityID = 0;
	_RecoveryPacketSequence = 0;
	if( _EventCallback )
		_EventCallback(Event);
}

void LiveOutputStream::MarkStreamEstablished()
{
	const std::lock_guard<std::mutex> Guard( *_SegmentsMutex );
	if( _StreamEstablished || _StartupGraceStarted.time_since_epoch().count() == 0 )
		return;
	const auto Elapsed = std::chrono::steady_clock::now() - _StartupGraceStarted;
	if( Elapsed < std::chrono::seconds(5) )
		return;
	_StartupAcceptedVideoPackets = _DiagAcceptedVideoPackets -
		_GenerationAcceptedVideoPacketsBaseline;
	_StartupDroppedVideoPackets = _DiagDroppedVideoPackets -
		_GenerationDroppedVideoPacketsBaseline;
	_StartupRepairedVideoTimestamps = _DiagRepairedVideoTimestamps -
		_GenerationRepairedVideoTimestampsBaseline;
	_EstablishedAcceptedVideoPacketsBaseline = _DiagAcceptedVideoPackets;
	_EstablishedDroppedVideoPacketsBaseline = _DiagDroppedVideoPackets;
	_EstablishedRepairedVideoTimestampsBaseline = _DiagRepairedVideoTimestamps;
	_StreamEstablished = true;
}

CameraStreamError LiveOutputStream::StartNewSegment(const AVPacket* Packet)
{
	if (!_InitSegmentCaptured)
	{
		// Write the init segment to an in-memory buffer
		_CurrentBuffer = std::make_shared<std::vector<uint8_t>>();

		AVDictionary* options = nullptr;
		av_dict_set(&options, "movflags", "empty_moov+frag_custom+dash+default_base_moof", 0);
		av_dict_set(&options, "brand", "iso6", 0);

		int Result = avformat_write_header(_FormatContext, &options);
		av_dict_free(&options);
		if (Result < 0)
		{
			STREAM_ERROR(WriteFailed, Result);
		}

		_HeaderWritten = true;

		avio_flush(_FormatContext->pb);
		const IsoBmffStructure InitStructure = AnalyseIsoBmff(
			_CurrentBuffer->data(), _CurrentBuffer->size(), true);

		// Store the init segment (ftyp + moov)
		LiveStreamEvent InitEvent;
		{
			const std::lock_guard<std::mutex> guard(*_SegmentsMutex);
			_InitSegmentData = _CurrentBuffer;
			_InitSegmentGeneration = _InitGeneration;
			_InitAudioCodec = _HasAudioStream ? "mp4a.40.2" : "";
			InitEvent.EventType = LiveStreamEvent::InitSegmentReady;
			InitEvent.Data = _InitSegmentData;
			InitEvent.Generation = _InitGeneration;
			InitEvent.AudioCodec = _InitAudioCodec;
			InitEvent.ByteSize = _InitSegmentData ? _InitSegmentData->size() : 0;
			InitEvent.TransportHash = InitEvent.ByteSize > 0 ?
				HashBytes32(_InitSegmentData->data(), _InitSegmentData->size()) : 0;
			_DiagInitStructureValid = InitStructure.Valid;
			_DiagInitStructureObserved = true;
			_DiagInitBoxCount = InitStructure.BoxCount;
			_DiagInitFtypCount = InitStructure.FtypCount;
			_DiagInitMoovCount = InitStructure.MoovCount;
			_DiagInitStructureError = InitStructure.Error;
			_DiagInitErrorOffset = InitStructure.ErrorOffset;
		}

		_InitSegmentCaptured = true;
		if (auto Capture = ActivePacketCapture())
			Capture->AddOutput(std::format(
				"{{\"type\":\"init\",\"generation\":{}", _InitGeneration),
				_InitSegmentData->data(), _InitSegmentData->size());
		if (!InitStructure.Valid)
			CaptureDiagnosticAnomaly("invalidInitStructure");

		// Notify MSE subscribers of init segment
		if (_EventCallback)
		{
			_EventCallback(InitEvent);
		}
	}

	// Start a fresh buffer for this segment
	_CurrentBuffer = std::make_shared<std::vector<uint8_t>>();

	_SegmentStartDTS = Packet->dts;
	_OutputSegmentStartDTS = AV_NOPTS_VALUE;
	_CurrentSegmentDuration = 0.0;
	_CurrentSegmentNominalDuration = 0.0;
	_CurrentPartialIndex = 0;
	_CurrentPartialDuration = 0.0;
	_CurrentPartialAudioDuration = 0.0;
	_CurrentPartialIsIndependent = true; // first partial starts with keyframe
	_CurrentPartialHasPacket = false;
	_CurrentPartialKeyframeSeekSafe = false;
	_SegmentTimestampNormalizationActive = _NormalizeNoBFrameTimestamps;
	_PartialBufferOffset = 0;
	_SegmentAcceptedVideoPackets = 0;
	_SegmentAcceptedVideoKeyframes = 0;
	_SegmentRepairedVideoTimestamps = 0;
	_SegmentDroppedVideoPackets = 0;
	_SegmentMissingVideoDtsPackets = 0;
	_SegmentCorruptVideoPackets = 0;
	_SegmentPacketPayloadHash = Fnv1aOffsetBasis;
	_CurrentSegmentWallTime = std::chrono::system_clock::now();

	{
		const std::lock_guard<std::mutex> guard(*_SegmentsMutex);

		// Trim old segments
		while (_StreamBacklog->size() >= 15)
		{
			_StreamBacklog->erase(_StreamBacklog->begin());
		}

		LiveStreamSegment NewSegment;
		NewSegment.Data = nullptr; // Will be set when finished
		NewSegment.Duration = 0.0;
		NewSegment.SegmentIndex = _CurrentSegmentIndex;
		NewSegment.SegmentTime = _CurrentSegmentWallTime;
		NewSegment.Ready = false;
		NewSegment.Discontinuity = _DiscontinuityPending;
		_DiscontinuityPending = false;

		_StreamBacklog->push_back(NewSegment);
	}

	return CameraStreamError::Success;
}

CameraStreamError LiveOutputStream::FinishCurrentSegment(int64_t NextKeyframeDTS)
{
	if (!_FormatContext || !_CurrentBuffer)
	{
		return CameraStreamError::Success;
	}

	// Flush remaining data as the final partial of this segment
	if( !FlushPartialSegment(_CurrentPartialIsIndependent) )
		return CameraStreamError::WriteFailed;

	// Compute accurate segment duration from DTS span instead of
	// accumulated packet durations. Accumulated durations drift over
	// thousands of segments because AVPacket.duration values don't
	// exactly match the DTS delta between keyframes.
	AVRational TimeBase = _FormatContext->streams[0]->time_base;
	double DtsDuration = (double)(NextKeyframeDTS - _SegmentStartDTS) * TimeBase.num / TimeBase.den;
	double OutputDuration = _CurrentSegmentDuration;
	if (_OutputSegmentStartDTS != AV_NOPTS_VALUE && _LastWrittenDTS != AV_NOPTS_VALUE && _LastOutputPacketDuration > 0)
	{
		OutputDuration = (double)(_LastWrittenDTS + _LastOutputPacketDuration - _OutputSegmentStartDTS) *
			TimeBase.num / TimeBase.den;
	}

	// Sanity: if DTS duration is clearly wrong, fall back to accumulated
	if (DtsDuration <= 0.0 || DtsDuration > 30.0)
		DtsDuration = _CurrentSegmentNominalDuration;

	// Record diagnostics
	double DriftMs = (_CurrentSegmentNominalDuration - DtsDuration) * 1000.0;
	SegmentDiagEntry Entry;
	Entry.SegmentIndex = _CurrentSegmentIndex;
	Entry.DtsDuration = DtsDuration;
	Entry.OutputDuration = OutputDuration;
	Entry.AccumulatedDuration = _CurrentSegmentNominalDuration;
	Entry.DriftMs = DriftMs;
	Entry.TimestampNormalizationActive = _SegmentTimestampNormalizationActive;
	Entry.AcceptedVideoPackets = _SegmentAcceptedVideoPackets;
	Entry.AcceptedVideoKeyframes = _SegmentAcceptedVideoKeyframes;
	Entry.RepairedVideoTimestamps = _SegmentRepairedVideoTimestamps;
	Entry.DroppedVideoPackets = _SegmentDroppedVideoPackets;
	Entry.MissingVideoDtsPackets = _SegmentMissingVideoDtsPackets;
	Entry.CorruptVideoPackets = _SegmentCorruptVideoPackets;
	Entry.PacketPayloadHash = _SegmentPacketPayloadHash;
	Entry.FragmentBytes = _CurrentBuffer ? (uint64_t)_CurrentBuffer->size() : 0;
	Entry.FragmentHash = Entry.FragmentBytes > 0 ?
		HashBytes(_CurrentBuffer->data(), _CurrentBuffer->size()) : Fnv1aOffsetBasis;
	const IsoBmffStructure FragmentStructure = AnalyseIsoBmff(
		_CurrentBuffer->data(), _CurrentBuffer->size(), false);
	Entry.FragmentStructureValid = FragmentStructure.Valid;
	Entry.FragmentBoxCount = FragmentStructure.BoxCount;
	Entry.FragmentMoofCount = FragmentStructure.MoofCount;
	Entry.FragmentMdatCount = FragmentStructure.MdatCount;
	Entry.FragmentStructureError = FragmentStructure.Error;
	Entry.FragmentErrorOffset = FragmentStructure.ErrorOffset;
	{
		const std::lock_guard<std::mutex> guard(*_SegmentsMutex);
		_DiagRing[_DiagRingPos % DIAG_RING_SIZE] = Entry;
		_DiagRingPos++;
		if (_DiagRingCount < DIAG_RING_SIZE) _DiagRingCount++;
		_DiagTotalSegments++;
		_DiagTotalDtsDuration += DtsDuration;
		_DiagTotalAccumulatedDuration += _CurrentSegmentNominalDuration;
		if (std::abs(DriftMs) > std::abs(_DiagMaxDriftMs))
			_DiagMaxDriftMs = DriftMs;

		if (!_StreamBacklog->empty())
		{
			auto& Seg = _StreamBacklog->back();
			Seg.Data = _CurrentBuffer;
			// Use accumulated packet duration for EXTINF — this matches the
			// actual sample durations in the fMP4 container. Using DTS span
			// causes playlist/media divergence: DTS may differ from packet
			// duration sums by ~0.002s/segment, which over thousands of
			// segments makes the playhead outrun buffered media.
			Seg.Duration = _CurrentSegmentDuration;
			Seg.Ready = true;
		}
		_CurrentSegmentIndex++;
	}

	_CurrentBuffer.reset();

	// Notify MSE subscribers that segment is complete
	// (MSE clients primarily use partials for low latency, but this
	// signals segment boundaries for buffer management)
	if (_EventCallback)
	{
		LiveStreamEvent Event;
		Event.EventType = LiveStreamEvent::SegmentReady;
		Event.SegmentIndex = _CurrentSegmentIndex - 1;
		Event.Duration = _CurrentSegmentDuration;
		Event.Generation = _InitGeneration;
		_EventCallback(Event);
	}
	return CameraStreamError::Success;
}

LiveOutputStream::StreamingDiagnostics LiveOutputStream::GetStreamingDiagnostics( bool IncludeHistory ) const
{
	StreamingDiagnostics Diag;
	const std::lock_guard<std::mutex> guard(*_SegmentsMutex);
	Diag.TotalSegments = _DiagTotalSegments;
	Diag.ReconnectCount = _InitGeneration;
	Diag.TotalDtsDuration = _DiagTotalDtsDuration;
	Diag.TotalAccumulatedDuration = _DiagTotalAccumulatedDuration;
	Diag.MaxDriftMs = _DiagMaxDriftMs;
	Diag.CurrentSegmentIndex = _CurrentSegmentIndex;
	Diag.InitGeneration = _InitGeneration;
	Diag.AcceptedVideoPackets = _DiagAcceptedVideoPackets;
	Diag.AcceptedVideoKeyframes = _DiagAcceptedVideoKeyframes;
	Diag.RepairedVideoTimestamps = _DiagRepairedVideoTimestamps;
	Diag.DroppedVideoPackets = _DiagDroppedVideoPackets;
	Diag.MissingVideoDtsPackets = _DiagMissingVideoDtsPackets;
	Diag.CorruptVideoPackets = _DiagCorruptVideoPackets;
	Diag.WaitingForKeyframePackets = _DiagWaitingForKeyframePackets;
	Diag.MissingTimestampPackets = _DiagMissingTimestampPackets;
	Diag.BeforeVideoEpochPackets = _DiagBeforeVideoEpochPackets;
	Diag.NegativeTimestampPackets = _DiagNegativeTimestampPackets;
	Diag.NonMonotonicInputPackets = _DiagNonMonotonicInputPackets;
	Diag.NoMuxBufferPackets = _DiagNoMuxBufferPackets;
	Diag.NonMonotonicOutputPackets = _DiagNonMonotonicOutputPackets;
	Diag.MuxErrorPackets = _DiagMuxErrorPackets;
	Diag.DecodeCorruptionEvents = _DiagDecodeCorruptionEvents;
	Diag.DecodeRecoveryEvents = _DiagDecodeRecoveryEvents;
	Diag.StreamEstablished = _StreamEstablished;
	if( _StartupGraceStarted.time_since_epoch().count() != 0 )
		Diag.StartupGraceElapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now() - _StartupGraceStarted).count();
	if( _StreamEstablished )
	{
		Diag.StartupAcceptedVideoPackets = _StartupAcceptedVideoPackets;
		Diag.StartupDroppedVideoPackets = _StartupDroppedVideoPackets;
		Diag.StartupRepairedVideoTimestamps = _StartupRepairedVideoTimestamps;
		Diag.EstablishedAcceptedVideoPackets = _DiagAcceptedVideoPackets -
			_EstablishedAcceptedVideoPacketsBaseline;
		Diag.EstablishedDroppedVideoPackets = _DiagDroppedVideoPackets -
			_EstablishedDroppedVideoPacketsBaseline;
		Diag.EstablishedRepairedVideoTimestamps = _DiagRepairedVideoTimestamps -
			_EstablishedRepairedVideoTimestampsBaseline;
	}
	else
	{
		Diag.StartupAcceptedVideoPackets = _DiagAcceptedVideoPackets -
			_GenerationAcceptedVideoPacketsBaseline;
		Diag.StartupDroppedVideoPackets = _DiagDroppedVideoPackets -
			_GenerationDroppedVideoPacketsBaseline;
		Diag.StartupRepairedVideoTimestamps = _DiagRepairedVideoTimestamps -
			_GenerationRepairedVideoTimestampsBaseline;
	}
	Diag.VideoPhaseErrorMs = _DiagVideoPhaseErrorMs;
	Diag.VideoCorrectionMs = _DiagVideoCorrectionMs;
	Diag.HasAudioVideoSkew = _DiagHasAudioOutputTimestamp && _DiagHasVideoOutputTimestamp;
	if (Diag.HasAudioVideoSkew)
		Diag.AudioVideoSkewMs = (_DiagLastAudioOutputUs - _DiagLastVideoOutputUs) / 1000.0;
	Diag.TimestampCorrectionSaturatedPackets = _DiagTimestampCorrectionSaturatedPackets;
	Diag.VideoCodec = _DiagVideoCodec;
	Diag.AudioCodec = _DiagAudioCodec;
	Diag.InputFormat = _DiagInputFormat;
	Diag.VideoProfile = _DiagVideoProfile;
	Diag.VideoLevel = _DiagVideoLevel;
	Diag.VideoWidth = _DiagVideoWidth;
	Diag.VideoHeight = _DiagVideoHeight;
	Diag.VideoTimeBaseNum = _DiagVideoTimeBaseNum;
	Diag.VideoTimeBaseDen = _DiagVideoTimeBaseDen;
	Diag.VideoExtradataBytes = _DiagVideoExtradataBytes;
	Diag.VideoExtradataHash = _DiagVideoExtradataHash;
	Diag.AudioProfile = _DiagAudioProfile;
	Diag.AudioSampleRate = _DiagAudioSampleRate;
	Diag.AudioChannels = _DiagAudioChannels;
	Diag.AudioTimeBaseNum = _DiagAudioTimeBaseNum;
	Diag.AudioTimeBaseDen = _DiagAudioTimeBaseDen;
	Diag.AudioExtradataBytes = _DiagAudioExtradataBytes;
	Diag.AudioExtradataHash = _DiagAudioExtradataHash;
	Diag.InitStructureValid = _DiagInitStructureValid;
	Diag.InitStructureObserved = _DiagInitStructureObserved;
	Diag.InitBoxCount = _DiagInitBoxCount;
	Diag.InitFtypCount = _DiagInitFtypCount;
	Diag.InitMoovCount = _DiagInitMoovCount;
	Diag.InitStructureError = _DiagInitStructureError;
	Diag.InitErrorOffset = _DiagInitErrorOffset;
	Diag.BacklogSize = (int)_StreamBacklog->size();
	const int count = _DiagRingCount;
	if( count > 0 )
		Diag.TimestampNormalizationActive =
			_DiagRing[(_DiagRingPos - 1) % DIAG_RING_SIZE].TimestampNormalizationActive;
	// A repeated FFmpeg cluster keeps its original ring slot. Rank by its last
	// occurrence so a still-active cluster remains visible in the health export.
	std::vector<const MediaDiagnosticEvent*> RecentMediaEvents;
	RecentMediaEvents.reserve( _MediaEventRingCount );
	const int MediaEventStart = _MediaEventRingPos - _MediaEventRingCount;
	for( int Index = 0; Index < _MediaEventRingCount; ++Index )
		RecentMediaEvents.push_back( &_MediaEventRing[(MediaEventStart + Index) % MEDIA_EVENT_RING_SIZE] );
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
	int start = (_DiagRingPos - count);
	if (start < 0) start += DIAG_RING_SIZE;
	for (int i = 0; i < count; i++)
	{
		Diag.RecentSegments.push_back(_DiagRing[(start + i) % DIAG_RING_SIZE]);
	}
	const int FragmentStart = _FragmentDiagRingPos - _FragmentDiagRingCount;
	Diag.RecentFragments.reserve(_FragmentDiagRingCount);
	for (int Index = 0; Index < _FragmentDiagRingCount; ++Index)
	{
		Diag.RecentFragments.push_back(
			_FragmentDiagRing[(FragmentStart + Index) % FRAGMENT_DIAG_RING_SIZE]);
	}
	const int PacketStart = _PacketDiagRingPos - _PacketDiagRingCount;
	Diag.RecentPackets.reserve(_PacketDiagRingCount);
	for (int Index = 0; Index < _PacketDiagRingCount; ++Index)
	{
		Diag.RecentPackets.push_back(
			_PacketDiagRing[(PacketStart + Index) % PACKET_DIAG_RING_SIZE]);
	}
	Diag.Anomalies = _DiagAnomalies;

	return Diag;
}

void LiveOutputStream::CaptureDiagnosticAnomaly(const std::string& Reason)
{
	const int64_t CapturedAtMs = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now() - _PacketDiagEpoch).count();
	const std::lock_guard<std::mutex> guard(*_SegmentsMutex);
	if (!_DiagAnomalies.empty() && _DiagAnomalies.back().Reason == Reason &&
		CapturedAtMs - _DiagAnomalies.back().CapturedAtMs < 5000)
	{
		return;
	}

	StreamingDiagnostics::AnomalyCapture Capture;
	Capture.Sequence = ++_DiagAnomalySequence;
	Capture.CapturedAtMs = CapturedAtMs;
	Capture.Generation = _InitGeneration;
	Capture.SegmentIndex = _CurrentSegmentIndex;
	Capture.Reason = Reason.substr(0, 64);

	const int FragmentCount = (std::min)(_FragmentDiagRingCount, 60);
	const int FragmentStart = _FragmentDiagRingPos - FragmentCount;
	Capture.Fragments.reserve(FragmentCount);
	for (int Index = 0; Index < FragmentCount; ++Index)
	{
		Capture.Fragments.push_back(
			_FragmentDiagRing[(FragmentStart + Index) % FRAGMENT_DIAG_RING_SIZE]);
	}

	const int PacketCount = (std::min)(_PacketDiagRingCount, 256);
	const int PacketStart = _PacketDiagRingPos - PacketCount;
	Capture.Packets.reserve(PacketCount);
	for (int Index = 0; Index < PacketCount; ++Index)
	{
		Capture.Packets.push_back(
			_PacketDiagRing[(PacketStart + Index) % PACKET_DIAG_RING_SIZE]);
	}

	if (_DiagAnomalies.size() >= 3)
		_DiagAnomalies.erase(_DiagAnomalies.begin());
	_DiagAnomalies.push_back(std::move(Capture));
}

}}
