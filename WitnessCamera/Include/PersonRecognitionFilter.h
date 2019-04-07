#pragma once

#include "RecordFilterBase.h"

namespace Witness{
namespace Camera{

struct PersonRecognitionFilterData;

class CAMERA_API PersonRecognitionFilter : public RecordFilterBase<PersonRecognitionFilterData>
{
public:

	PersonRecognitionFilter( const MotionChainNode& Chain, const char* FaceCascadeDataFilename, const char* FullBodyCascadeDataFilename );
	virtual ~PersonRecognitionFilter();

	virtual bool ProcessFrame( SharedClassificationTask TaskData );
};

}}
