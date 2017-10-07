#pragma once

#include "Export.h"
#include "Pimpl.h"

#include <memory>

namespace Witness{
namespace Camera{

struct FilterData;

class CAMERA_API IRecordFilter
{
public:
	
	virtual const char* CalculateRecordingClassification( unsigned int Width, unsigned int Height, void* Data ) = 0;

	virtual void AddChildFilter( std::shared_ptr<IRecordFilter>& ChildFilter ) = 0;
};

}}
