#include "Frame.h"

namespace Witness{
namespace Camera{
namespace FFMPEG{

Frame::Frame( unsigned int Width, unsigned int Height, AVPixelFormat Format, unsigned int Alignment )
: m_Frame( nullptr )
, m_Width( Width )
, m_Height( Height )
, m_Format( Format )
, m_Alignment( Alignment )
{
	m_Frame = av_frame_alloc();
	m_Frame->width = Width;
	m_Frame->height = Height;
	m_Frame->format = Format;
	
	//av_frame_get_buffer( m_Frame, Alignment );
	av_image_alloc( m_Frame->data, m_Frame->linesize, Width, Height, Format, Alignment );
	av_image_fill_linesizes( m_Frame->linesize, Format, Width );

	m_Frame->sample_aspect_ratio.num = 1;
	m_Frame->sample_aspect_ratio.den = 1;
	m_Frame->pts = AV_NOPTS_VALUE;
	
	Prepare();
}

Frame::~Frame()
{
	av_frame_free( &m_Frame );
}

void Frame::Prepare()
{
	av_frame_make_writable( m_Frame );
}

void Frame::Unref()
{
	av_frame_unref( m_Frame );
}

}}}
