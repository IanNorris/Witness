#pragma once

#include <stdint.h>
#include <stdio.h>
#include "Export.h"

// FFmpeg 7+ changed avio write callback to const uint8_t*; older versions use uint8_t*
#include <libavformat/version.h>
#if LIBAVFORMAT_VERSION_MAJOR >= 61
#  define AV_WRITE_BUF_CONST const
#else
#  define AV_WRITE_BUF_CONST
#endif

struct AVIOContext;

namespace Witness{
namespace Camera{
namespace FFMPEG{

class CAMERA_API InMemoryIOContext
{
public:

	InMemoryIOContext(const char* Filename);
	virtual ~InMemoryIOContext();

	AVIOContext* GetContext() { return _Context;}

	void Close();

	static int Read(void* Opaque, uint8_t* Buffer, int BufferSize);
	static int Write(void* Opaque, AV_WRITE_BUF_CONST uint8_t* Buffer, int BufferSize);
	static int64_t Seek(void* Opaque, int64_t Offset, int Origin);

private:

	std::string* _Filename;
	uint8_t* _TempBuffer;
	FILE* _Handle;
	AVIOContext* _Context;
};

}}}
