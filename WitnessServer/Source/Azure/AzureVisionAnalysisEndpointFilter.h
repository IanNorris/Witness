#pragma once

#include "AzureEndpoint.h"

class AzureVisionAnalysisEndpointFilter : public AzureEndpointFilter
{
public:

	enum CommandType
	{
		Analysis
	};

	AzureVisionAnalysisEndpointFilter(const SettingsMap& Settings)
	: AzureEndpointFilter(Settings)
	{
	}

	virtual void ClassifyFrame( FilterFrame& Frame, ClassificationResult& Result ) override;

	virtual void ClearState() override {}

private:

	virtual const string_t CommandTypeToEndpoint( int CommandType );
	virtual const string_t CommandTypeToMethod( int CommandType );

};
