#pragma once

#include "AzureEndpoint.h"

class AzureVisionAnalysisEndpointFilter : public AzureEndpointFilter
{
public:

	enum CommandType
	{
		Analysis
	};

	AzureVisionAnalysisEndpointFilter(const MotionChainNode& Chain, const SettingsMap& Settings)
	: AzureEndpointFilter(Chain, Settings)
	{
	}

	virtual bool ProcessFrame( SharedClassificationTask TaskData ) override;

private:

	virtual const StringT CommandTypeToEndpoint( int CommandType );
	virtual const StringT CommandTypeToMethod( int CommandType );

};
