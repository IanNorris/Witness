#include "FFMPEG/Frame.h"
#include "Common.h"

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
	m_Frame->width = Width;
	m_Frame->height = Height;
	m_Frame->format = Format;
	
	//Prepare();
}

Frame::~Frame()
{
	if (m_Prepared)
	{
		av_freep( &m_Frame->data[0] );

	}
	
	av_frame_free( &m_Frame );
}

void Frame::Prepare()
{
	av_frame_get_buffer( m_Frame, m_Alignment );
	av_image_alloc( m_Frame->data, m_Frame->linesize, m_Width, m_Height, (AVPixelFormat)m_Format, m_Alignment );
	av_image_fill_linesizes( m_Frame->linesize, (AVPixelFormat)m_Format, m_Width );

	m_Frame->sample_aspect_ratio.num = 1;
	m_Frame->sample_aspect_ratio.den = 1;
	m_Frame->pts = AV_NOPTS_VALUE;

	av_frame_make_writable( m_Frame );

	m_Prepared = true;
}

void Frame::Unref()
{
	av_frame_unref( m_Frame );
}

}}}
