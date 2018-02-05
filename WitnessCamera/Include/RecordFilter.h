#pragma once

#include "Export.h"
#include "Pimpl.h"

#include <memory>

namespace Witness{
namespace Camera{

struct FilterData;
class StreamManager;

enum class ClassificationImportance
{
	None,

	Motion = 10,

	Person_Familial = 100,
	Person_Friends = 150,
	Person_Recognized = 200,

	Person_Unknown = 200,
	Person_HighRisk = 500,
};

struct ClassificationResult
{
	ClassificationImportance operator= ( int Input )
	{
		return (ClassificationImportance)Input;
	}

	ClassificationResult( ClassificationImportance Importance = ClassificationImportance::None, const char* ResultString = nullptr, double MotionAmount = 0.0 )
	: ResultString( ResultString )
	, Importance( (int)Importance )
	, MotionPercentage( MotionAmount )
	{}

	const char* ResultString;
	int Importance;

	double MotionPercentage;
};

class CAMERA_API IRecordFilter
{
public:
	
	virtual ClassificationResult FilterFrame( unsigned int Width, unsigned int Height, void* Data, StreamManager* StreamManager ) = 0;

	virtual void AddChildFilter( std::shared_ptr<IRecordFilter>& ChildFilter ) = 0;
	virtual ClassificationResult PostSuccessChildVisitor( unsigned int Width, unsigned int Height, void* Data, StreamManager* StreamManager ) = 0;
};

}}
