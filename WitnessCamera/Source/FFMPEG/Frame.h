#pragma once

#include "Common.h"

namespace Witness{
namespace Camera{
namespace FFMPEG{

class Frame
{
public:
	
	Frame( unsigned int Width, unsigned int Height, AVPixelFormat Format, unsigned int Alignment = 1 );
	~Frame();

	void Unref();

	inline unsigned int GetWidth() { return m_Width; }
	inline unsigned int GetHeight() { return m_Height; }
	inline unsigned int GetFormat() { return m_Format; }

	inline AVFrame*& GetFrame() { return m_Frame; }

private:

	AVFrame*		m_Frame;

	unsigned int	m_Width;
	unsigned int	m_Height;
	unsigned int	m_Alignment;
	AVPixelFormat	m_Format;
	
};

}}}
