#pragma once

#include "Stream.h"

#include <ctime>

struct AVPacket;
struct AVRational;

namespace Witness{
namespace Camera{

namespace FFMPEG{
class Frame;
class InMemoryIOContext;
}

class InputStream;

class CAMERA_API OutputStream : public Stream
{
public:
	OutputStream( const std::string& Path, InputStream * InputStream, bool InMemory, bool LiveStream, bool Part, bool InitSegment);
	OutputStream( const std::string& Path, unsigned int Width, unsigned int Height, int Framerate, bool IsBGR );
	virtual ~OutputStream();

	virtual CameraStreamError Initialize() override;
	virtual CameraStreamError ProcessFrame( const std::shared_ptr<IRecordFilter>& Filter, Stream* TargetStream, Stream* LiveStream ) override;
	virtual void Shutdown() override;

	CameraStreamError WriteInterleavedPacket( const AVPacket* Packet );
	CameraStreamError WriteFrame( FFMPEG::Frame* Frame );

	CameraStreamError CloseFile(bool Flush = true, bool WriteTrailer = true);

	int GetStreamIndex();

	FFMPEG::InMemoryIOContext* GetOutput() { return m_IOContext; }

	double GetClipLength() { return m_ClipLength;  }

	void SetSegmentIndex(int index) { m_SegmentIndex = index; }
	int GetSegmentIndex() { return m_SegmentIndex; }

	void SetPartIndex(int index) { m_PartIndex = index; }
	int GetPartIndex() { return m_PartIndex; }

	CameraStreamError GenerateInitSegment(const std::string& InitSegmentPath);

	bool IsIsolated()
	{
		return m_Isolated;
	}

	void SetIsolated(bool Isolated)
	{
		m_Isolated = Isolated;
	}

private:

	static int GlobalOutputStreamIndex;

	CameraStreamError SendAll( void );

	InputStream * m_InputStream;
	FFMPEG::InMemoryIOContext* m_IOContext;
	int FrameIndex;

	int StreamIndex;

	bool m_FileOpened;
	bool m_InMemory;
	bool m_Live;
	bool m_Isolated;
	bool m_Part;
	bool m_InitSegment;

	double m_ClipLength;
	int m_SegmentIndex;
	int m_PartIndex;
	int64_t m_LastWrittenDTS;
	bool m_HasAudioStream;
	int m_AudioInputStreamIndex;
	int64_t m_InitialAudioDTS;
};

}}
