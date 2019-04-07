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

	virtual const string_t CommandTypeToEndpoint( int CommandType );
	virtual const string_t CommandTypeToMethod( int CommandType );

};
