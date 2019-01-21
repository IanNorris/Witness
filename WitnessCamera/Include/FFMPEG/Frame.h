#pragma once

#include "Export.h"

struct AVFrame;

namespace Witness{
namespace Camera{
namespace FFMPEG{

class CAMERA_API Frame
{
public:
	
	Frame( unsigned int Width, unsigned int Height, unsigned int Format, unsigned int Alignment = 1 );
	virtual ~Frame();

	void Prepare();

	void Unref();

	inline unsigned int GetWidth() { return m_Width; }
	inline unsigned int GetHeight() { return m_Height; }
	inline unsigned int GetFormat() { return m_Format; }

	inline AVFrame*& GetFrame() { return m_Frame; }

private:

	Frame( const Frame& Other );
	Frame& operator=( const Frame& );

	AVFrame*		m_Frame;

	unsigned int	m_Width;
	unsigned int	m_Height;
	unsigned int	m_Alignment;
	unsigned int	m_Format;
	
	bool			m_Prepared;
};

}}}
