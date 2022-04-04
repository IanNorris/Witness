#include "OutputStream.h"
#include "InputStream.h"
#include "StreamData.h"
#include "InMemoryIOContext.h"

namespace Witness{
namespace Camera{
namespace FFMPEG{

static const int ContextBufferSize = 256 * 1024;

InMemoryIOContext::InMemoryIOContext(const char* Filename)
{
	_Filename = new std::string(Filename);

	_TempBuffer = new unsigned char[ContextBufferSize];

	fopen_s(&_Handle, Filename, "wb");

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

	DeleteFileA(_Filename->c_str());
}

int InMemoryIOContext::Read(void* Opaque, unsigned char* Buffer, int BufferSize)
{
	InMemoryIOContext* This = (InMemoryIOContext*)Opaque;

	return (int)fread(Buffer, 1, BufferSize, This->_Handle);
}

int InMemoryIOContext::Write(void* Opaque, unsigned char* Buffer, int BufferSize)
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