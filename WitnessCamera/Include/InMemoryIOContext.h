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

	static int Read(void* Opaque, unsigned char* Buffer, int BufferSize);
	static int Write(void* Opaque, unsigned char* Buffer, int BufferSize);
	static int64_t Seek(void* Opaque, int64_t Offset, int Origin);

private:

	std::string* _Filename;
	unsigned char* _TempBuffer;
	FILE* _Handle;
	AVIOContext* _Context;
};

}}}
