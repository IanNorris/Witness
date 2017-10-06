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
	size_t PicSizeOut = av_image_get_buffer_size( Format, Width, Height, Alignment );
	uint8_t* PicBufOut = (uint8_t*)av_malloc( PicSizeOut );

	av_image_fill_arrays( m_Frame->data, m_Frame->linesize, PicBufOut, Format, Width, Height, Alignment );
}

Frame::~Frame()
{
	av_frame_free( &m_Frame );
}

void Frame::Unref()
{
	av_frame_unref( m_Frame );
}

}}}
