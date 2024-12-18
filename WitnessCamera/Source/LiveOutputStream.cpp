#include "LiveOutputStream.h"
#include "OutputStream.h"
#include "InputStream.h"
#include "StreamData.h"
#include "InMemoryIOContext.h"

#include <atomic>
#include <sstream>
#include <chrono>
#include <format>

namespace Witness{
namespace Camera{

LiveOutputStream::LiveOutputStream(const std::string& LiveCachePath, InputStream* InputStream, int KeyframesPerSegment)
	: Stream()
	, _LiveCachePath( new std::string(LiveCachePath))
	, _StreamBacklog( new std::vector<LiveStreamSegment>() )
	, _CurrentStream( nullptr )
	, _CurrentPartStream(nullptr)
	, _InputStream(InputStream)
	, _KeyframesPerSegment(KeyframesPerSegment)
	, _KeyframesPerSegmentLeft(KeyframesPerSegment)
	, _SkipInitialKeyframes(1)
	, _CurrentSegmentIndex(0)
	, _CurrentPartIndex(0)
	, _SegmentsMutex( new std::mutex )
{
	m_LastPartTime = std::chrono::high_resolution_clock::now();
}

LiveOutputStream::~LiveOutputStream()
{
	delete _StreamBacklog;
	_StreamBacklog = nullptr;

	delete _LiveCachePath;
	_LiveCachePath = nullptr;

	delete _SegmentsMutex;
	_SegmentsMutex = nullptr;

	delete _CurrentPartStream;
	_CurrentPartStream = nullptr;

	delete _CurrentStream;
	_CurrentStream = nullptr;
}

CameraStreamError LiveOutputStream::Initialize()
{
	return CameraStreamError::Success;
}

CameraStreamError LiveOutputStream::ProcessFrame(const std::shared_ptr<IRecordFilter>& Filter, Stream* TargetStream, Stream* LiveStream)
{
	return CameraStreamError::Success;
}

void LiveOutputStream::Shutdown()
{

}

CameraStreamError LiveOutputStream::WriteInterleavedPacket(const AVPacket* Packet)
{
	auto CurrentTime = std::chrono::high_resolution_clock::now();
	auto TimeSinceLastSegment = std::chrono::duration_cast<std::chrono::milliseconds>(CurrentTime - m_LastPartTime).count();
	if (Packet->flags & AV_PKT_FLAG_KEY)
	{
		if (_SkipInitialKeyframes > 0)
		{
			_SkipInitialKeyframes--;
			return CameraStreamError::Success;
		}

		if (_CurrentPartStream)
		{
			FinishPartStream();
		}

		if (_CurrentStream)
		{
			FinishStream();
		}

		CameraStreamError Result = StartNewStream(Packet);
		if (Result != CameraStreamError::Success)
		{
			return Result;
		}

		m_LastPartTime = CurrentTime;
		Result = StartNewPartStream(Packet);
		if (Result != CameraStreamError::Success)
		{
			return Result;
		}
	}
	else if (TimeSinceLastSegment >= 125 && _CurrentStream)
	{
		if (_CurrentPartStream)
		{
			FinishPartStream();
		}

		m_LastPartTime = CurrentTime;
		CameraStreamError Result = StartNewPartStream(Packet);
		if (Result != CameraStreamError::Success)
		{
			return Result;
		}
	}
	
	if (_CurrentStream)
	{
		CameraStreamError Result = _CurrentStream->WriteInterleavedPacket(Packet);
		if (Result != CameraStreamError::Success)
		{
			memcpy(m_ErrorMessage, _CurrentStream->GetFFMPEGErrorMessage(), 256);
			return Result;
		}
	}

	if (_CurrentPartStream)
	{
		CameraStreamError Result = _CurrentPartStream->WriteInterleavedPacket(Packet);
		if (Result != CameraStreamError::Success)
		{
			memcpy(m_ErrorMessage, _CurrentPartStream->GetFFMPEGErrorMessage(), 256);
			return Result;
		}
	}

	return CameraStreamError::Success;
}

void LiveOutputStream::FinishStream()
{
	_CurrentStream->Shutdown();

	const std::lock_guard<std::mutex> guard(*_SegmentsMutex);

	if (_StreamBacklog->size() > 2)
	{
		delete _StreamBacklog->front().Stream;

		for (auto PartStream : _StreamBacklog->front().PartialStreams)
		{
			delete PartStream;
		}

		_StreamBacklog->erase(_StreamBacklog->begin());
	}

	if (_StreamBacklog->size() > 0)
	{
		_StreamBacklog->back().Ready = true;
	}
}

void LiveOutputStream::FinishPartStream()
{
	_CurrentPartStream->Shutdown();

	const std::lock_guard<std::mutex> guard(*_SegmentsMutex);

	if (_StreamBacklog->size() > 0)
	{
		_StreamBacklog->back().PartialStreams.push_back(_CurrentPartStream);
	}
}

CameraStreamError LiveOutputStream::StartNewStream(const AVPacket* Packet)
{
	_CurrentPartIndex = 0;
	int NewSegmentIndex = _CurrentSegmentIndex;

	CreateDirectoryA(_LiveCachePath->c_str(), nullptr);
	std::stringstream TargetFilename;
	TargetFilename << *_LiveCachePath << "\\Live_" << _InputStream->GetSourceId() << "_" << NewSegmentIndex << ".mp4";

	std::string FinishedPath = TargetFilename.str();

	_CurrentStream = new OutputStream(FinishedPath, _InputStream, false, true);
	_CurrentStream->SetSegmentIndex(NewSegmentIndex);

	_CurrentSegmentIndex++;

	CameraStreamError Result  = _CurrentStream->Initialize();
	if (Result != CameraStreamError::Success)
	{
		memcpy(m_ErrorMessage, _CurrentStream->GetFFMPEGErrorMessage(), 256);
		return Result;
	}

	Result = _CurrentStream->WriteInterleavedPacket(Packet);
	if(Result != CameraStreamError::Success)
	{
		memcpy(m_ErrorMessage, _CurrentStream->GetFFMPEGErrorMessage(), 256);
		return Result;
	}

	if (Packet->flags & AV_PKT_FLAG_KEY)
	{
		_CurrentStream->SetIsolated(true);
	}

	//Create a placeholder, we'll insert the real stream here on close.
	LiveStreamSegment NewSegment;
	
	NewSegment.Ready = false;
	NewSegment.Stream = _CurrentStream;

	OutputStream* PrevStream = nullptr;
	if (_StreamBacklog->size() && _StreamBacklog->back().Stream)
	{
		PrevStream = _StreamBacklog->back().Stream;
		double ClipLength = PrevStream->GetClipLength();
		int WholeSeconds = (int)ClipLength;
		int Milliseconds = (int)((ClipLength - WholeSeconds) * 1000);
		NewSegment.SegmentTime = _StreamBacklog->back().SegmentTime + std::chrono::seconds(WholeSeconds) + std::chrono::milliseconds(Milliseconds);
	}
	else
	{
		NewSegment.SegmentTime = std::chrono::system_clock::now();
	}

	_StreamBacklog->push_back(NewSegment);

	return CameraStreamError::Success;
}

CameraStreamError LiveOutputStream::StartNewPartStream(const AVPacket* Packet)
{
	CreateDirectoryA(_LiveCachePath->c_str(), nullptr);
	std::stringstream TargetFilename;
	TargetFilename << *_LiveCachePath << "\\Live_" << _InputStream->GetSourceId() << "_" << (_CurrentSegmentIndex-1) << "." << _CurrentPartIndex << ".mp4";
	
	std::string FinishedPath = TargetFilename.str();

	_CurrentPartStream = new OutputStream(FinishedPath, _InputStream, false, true);
	CameraStreamError Result = _CurrentPartStream->Initialize();
	if (Result != CameraStreamError::Success)
	{
		memcpy(m_ErrorMessage, _CurrentPartStream->GetFFMPEGErrorMessage(), 256);
		return Result;
	}

	Result = _CurrentPartStream->WriteInterleavedPacket(Packet);
	if (Result != CameraStreamError::Success)
	{
		memcpy(m_ErrorMessage, _CurrentPartStream->GetFFMPEGErrorMessage(), 256);
		return Result;
	}

	if (Packet->flags & AV_PKT_FLAG_KEY)
	{
		_CurrentPartStream->SetIsolated(true);
	}

	_CurrentPartStream->SetSegmentIndex(_CurrentSegmentIndex);
	_CurrentPartStream->SetPartIndex(_CurrentPartIndex);
	_CurrentPartIndex++;

	return CameraStreamError::Success;
}

}}