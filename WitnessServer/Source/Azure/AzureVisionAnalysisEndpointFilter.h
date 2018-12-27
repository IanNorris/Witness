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

	virtual void FilterFrame( const AVFrame* Frame, Witness::Camera::ClassificationResult& Result, cv::Mat& InputFrame, cv::Mat& GrayscaleInputFrame ) override;

	virtual void ClearState() override {}

private:

	virtual const string_t CommandTypeToEndpoint( int CommandType );
	virtual const string_t CommandTypeToMethod( int CommandType );

};
