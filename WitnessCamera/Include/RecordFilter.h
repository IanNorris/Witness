#pragma once

#include "Export.h"
#include "Pimpl.h"

#include <memory>

namespace Witness{
namespace Camera{

struct FilterData;
class StreamManager;

struct ClassificationResult
{
	const char* Result;
	int Importance;
};

class CAMERA_API IRecordFilter
{
public:
	
	virtual const char* FilterFrame( unsigned int Width, unsigned int Height, void* Data, StreamManager* StreamManager ) = 0;

	virtual void AddChildFilter( std::shared_ptr<IRecordFilter>& ChildFilter ) = 0;
};

}}
