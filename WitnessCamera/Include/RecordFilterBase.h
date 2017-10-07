#pragma once

#include "Export.h"
#include "Pimpl.h"
#include "RecordFilter.h"

#include <memory>

#define FILTER_BASE_CONSTRUCT(DataType)\
void RecordFilterBase<DataType>::AddChildFilter( std::shared_ptr<IRecordFilter>& ChildFilter )\
{GetData().ChildFilters.push_back( ChildFilter );}

namespace Witness{
namespace Camera{

template<typename FilterDataType>
class CAMERA_API RecordFilterBase : public Pimpl<FilterDataType>, public IRecordFilter
{
public:

	RecordFilterBase() : Pimpl() {}

	virtual void AddChildFilter( std::shared_ptr<IRecordFilter>& ChildFilter );
};

}}
