#include "LiveOutputStream.h"
#include "OutputStream.h"
#include "InputStream.h"
#include "StreamData.h"
#include "InMemoryIOContext.h"

#include <atomic>
#include <sstream>

namespace Witness{
namespace Camera{

LiveOutputStream::LiveOutputStream(const std::string& LiveCachePath, InputStream* InputStream, int KeyframesPerSegment)
	: Stream()
	, _LiveCachePath( new std::string(LiveCachePath))
	, _StreamBacklog( new std::vector<OutputStream*>() )
	, _CurrentStream( nullptr )
	, _InputStream(InputStream)
	, _KeyframesPerSegment(KeyframesPerSegment)
	, _KeyframesPerSegmentLeft(KeyframesPerSegment)
	, _SkipInitialKeyframes(1)
	, _CurrentSegmentIndex(0)
	, _SegmentsMutex( new std::mutex )
{
}

LiveOutputStream::~LiveOutputStream()
{
	delete _StreamBacklog;
	_StreamBacklog = nullptr;

	delete _LiveCachePath;
	_LiveCachePath = nullptr;

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

void LiveOutputStream::Shutdown()
{

}

CameraStreamError LiveOutputStream::WriteInterleavedPacket(const AVPacket* Packet)
{
	if (Packet->flags & AV_PKT_FLAG_KEY)
	{
		if (_SkipInitialKeyframes > 0)
		{
			_SkipInitialKeyframes--;
			return CameraStreamError::Success;
		}

		if (_CurrentStream)
		{
			//Finished the segment
			if (_KeyframesPerSegmentLeft == 0)
			{
				_KeyframesPerSegmentLeft = _KeyframesPerSegment;

				FinishStream();
				
				CameraStreamError Result = StartNewStream(Packet);
				if (Result != CameraStreamError::Success)
				{
					return Result;
				}
			}
			else
			{
				_KeyframesPerSegmentLeft--;

				CameraStreamError Result = _CurrentStream->WriteInterleavedPacket(Packet);
				if (Result != CameraStreamError::Success)
				{
					memcpy(m_ErrorMessage, _CurrentStream->GetFFMPEGErrorMessage(), 256);
					return Result;
				}
			}
		}
		else
		{
			CameraStreamError Result = StartNewStream(Packet);
			if (Result != CameraStreamError::Success)
			{
				return Result;
			}
		}
	}
	else
	{
		if (_CurrentStream)
		{
			CameraStreamError Result = _CurrentStream->WriteInterleavedPacket(Packet);
			if (Result != CameraStreamError::Success)
			{
				memcpy(m_ErrorMessage, _CurrentStream->GetFFMPEGErrorMessage(), 256);
				return Result;
			}
		}
	}

	return CameraStreamError::Success;
}

void LiveOutputStream::FinishStream()
{
	_CurrentStream->Shutdown();

	const std::lock_guard<std::mutex> guard(*_SegmentsMutex);

	/*if (_StreamBacklog->size() > 10)
	{
		delete _StreamBacklog->front();

		_StreamBacklog->erase(_StreamBacklog->begin());
	}*/

	_CurrentStream->SetSegmentIndex(_CurrentSegmentIndex);
	_StreamBacklog->push_back(_CurrentStream);
}

CameraStreamError LiveOutputStream::StartNewStream(const AVPacket* Packet)
{
	int NewSegmentIndex = ++_CurrentSegmentIndex;

	CreateDirectoryA(_LiveCachePath->c_str(), nullptr);
	std::stringstream TargetFilename;
	TargetFilename << *_LiveCachePath << "\\Live_" << _InputStream->GetSourceId() << "_" << NewSegmentIndex << ".mp4";

	std::string FinishedPath = TargetFilename.str();

	_CurrentStream = new OutputStream(FinishedPath, _InputStream, false);
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

	return CameraStreamError::Success;
}

}}