#pragma once

#include "RecordFilterBase.h"

namespace Witness{
namespace Camera{

struct PersonRecognitionFilterData;

class CAMERA_API PersonRecognitionFilter : public RecordFilterBase<PersonRecognitionFilterData>
{
public:

	PersonRecognitionFilter( const char* FaceCascadeDataFilename, const char* FullBodyCascadeDataFilename, const char* FilterName );
	virtual ~PersonRecognitionFilter();

	virtual ClassificationResult FilterFrame( unsigned int Width, unsigned int Height, void* Data, StreamManager* StreamManager );
};

}}
