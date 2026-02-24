#pragma once

#include <stdint.h>
#include <stdio.h>
#include "Export.h"

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
	static int Write(void* Opaque, const uint8_t* Buffer, int BufferSize);
	static int64_t Seek(void* Opaque, int64_t Offset, int Origin);

private:

	std::string* _Filename;
	uint8_t* _TempBuffer;
	FILE* _Handle;
	AVIOContext* _Context;
};

}}}
