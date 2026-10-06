#include "FFMPEG/Frame.h"
#include "Common.h"
#include <new>
#include <stdexcept>

namespace Witness{
namespace Camera{
namespace FFMPEG{

Frame::Frame( unsigned int Width, unsigned int Height, unsigned int Format, unsigned int Alignment )
: m_Frame( nullptr )
, m_Width( Width )
, m_Height( Height )
, m_Format( Format )
, m_Alignment( Alignment )
, m_Prepared( false )
{
	m_Frame = av_frame_alloc();
	if (!m_Frame)
		throw std::bad_alloc();
	m_Frame->width = Width;
	m_Frame->height = Height;
	m_Frame->format = Format;
	
}

Frame::~Frame()
{
	// data[] is owned by the AVBufferRefs, including decoder-produced frames.
	// Releasing a plane separately breaks retained references and can double-free.
	av_frame_free( &m_Frame );
}

void Frame::Prepare()
{
	int Result;
	if (!m_Prepared)
	{
		av_frame_unref(m_Frame);
		m_Frame->width = m_Width;
		m_Frame->height = m_Height;
		m_Frame->format = m_Format;
		// One allocation owns both data[] and buf[]. Preserve its padded strides.
		Result = av_frame_get_buffer(m_Frame, m_Alignment);
	}
	else
	{
		// Encoders can retain a previous output frame. Reuse only if writable;
		// otherwise let FFmpeg perform copy-on-write instead of overwriting it.
		Result = av_frame_make_writable(m_Frame);
	}
	if (Result < 0)
		throw std::runtime_error("Could not prepare FFmpeg frame buffer");

	m_Frame->sample_aspect_ratio.num = 1;
	m_Frame->sample_aspect_ratio.den = 1;
	m_Frame->pts = AV_NOPTS_VALUE;

	m_Prepared = true;
}

void Frame::Unref()
{
	av_frame_unref( m_Frame );
	m_Prepared = false;
}

}}}
