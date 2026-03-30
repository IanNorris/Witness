#include "OutputStream.h"
#include "InputStream.h"
#include "StreamData.h"
#include "InMemoryIOContext.h"

#ifndef _WIN32
#include <cstdio>
#endif

namespace Witness{
namespace Camera{
namespace FFMPEG{

static const int ContextBufferSize = 256 * 1024;

InMemoryIOContext::InMemoryIOContext(const char* Filename)
{
	_Filename = new std::string(Filename);

	_TempBuffer = new uint8_t[ContextBufferSize];

#ifdef _WIN32
	fopen_s(&_Handle, Filename, "wb");
#else
	_Handle = fopen(Filename, "wb");
#endif

	_Context = avio_alloc_context(_TempBuffer, ContextBufferSize, AVIO_FLAG_WRITE, this, &InMemoryIOContext::Read, &InMemoryIOContext::Write, &InMemoryIOContext::Seek);
}

InMemoryIOContext::~InMemoryIOContext()
{
	Close();

	av_free(_Context);

	delete[] _TempBuffer;
	_TempBuffer = nullptr;

	delete _Filename;
	_Filename = nullptr;
}

void InMemoryIOContext::Close()
{
	if( _Handle )
	{
		fclose(_Handle);
		_Handle = nullptr;
	}

#ifdef _WIN32
	DeleteFileA(_Filename->c_str());
#else
	std::remove(_Filename->c_str());
#endif
}

int InMemoryIOContext::Read(void* Opaque, uint8_t* Buffer, int BufferSize)
{
	InMemoryIOContext* This = (InMemoryIOContext*)Opaque;

	return (int)fread(Buffer, 1, BufferSize, This->_Handle);
}

int InMemoryIOContext::Write(void* Opaque, const uint8_t* Buffer, int BufferSize)
{
	InMemoryIOContext* This = (InMemoryIOContext*)Opaque;

	return (int)fwrite(Buffer, 1, BufferSize, This->_Handle);
}

int64_t InMemoryIOContext::Seek(void* Opaque, int64_t Offset, int Origin)
{
	InMemoryIOContext* This = (InMemoryIOContext*)Opaque;

	return fseek(This->_Handle, (int)Offset, Origin);
}

}}}