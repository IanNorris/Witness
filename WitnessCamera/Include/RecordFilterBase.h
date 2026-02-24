#pragma once

#include "Export.h"
#include "Pimpl.h"
#include "RecordFilter.h"

#include <memory>

namespace Witness{
namespace Camera{

template<typename FilterDataType>
class CAMERA_API RecordFilterBase : public Pimpl<FilterDataType>, public IRecordFilter
{
public:

	RecordFilterBase( const MotionChainNode& Chain ) : Pimpl<FilterDataType>(), IRecordFilter( Chain )  {}
	virtual ~RecordFilterBase(){}
};

}}
