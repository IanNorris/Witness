#include "AzureVisionAnalysisEndpointFilter.h"

void AzureVisionAnalysisEndpointFilter::FilterFrame(const AVFrame* Frame, Witness::Camera::ClassificationResult& Result, cv::Mat& InputFrame, cv::Mat& GrayscaleInputFrame)
{
	if (!IsAllowedToProcessFrame())
	{
		return;
	}

	json::value Request;
	QueryPairs Query;
	std::vector<unsigned char> Data;

	PrepareImage( InputFrame, Data );

	Query.push_back( QueryPair( _T("visualFeatures"), _T("Faces,Tags") ) );

	SendCommand( Analysis, Request, Query, Data ).then(
		[](web::http::http_response Response)
		{
			wprintf(_T("%s\n"), Response.extract_string(true).get().c_str() );
		}
	);
}

const string_t AzureVisionAnalysisEndpointFilter::CommandTypeToEndpoint(int CommandType)
{
	switch (CommandType)
	{
	case Analysis:
		return EndpointBase + _T("/analyze");

	default:
		assert(0);
		return _T("");
	}
}

const string_t AzureVisionAnalysisEndpointFilter::CommandTypeToMethod(int CommandType)
{
	switch (CommandType)
	{
	case Analysis:
		return _T("POST");

	default:
		assert(0);
		return _T("");
	}
}
