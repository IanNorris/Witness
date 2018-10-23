#pragma once

#include "../Common.h"
#include "../SettingsMap.h"
#include "RecordFilter.h"

#include <cpprest/http_client.h>

class AzureEndpointFilter : public Witness::Camera::IRecordFilter
{
public:

	typedef pair<string_t,string_t> QueryPair;
	typedef vector<QueryPair> QueryPairs;


	AzureEndpointFilter( const SettingsMap& Settings );

	pplx::task<web::http::http_response> SendCommand( int CommandType, const json::value& RequestData, const QueryPairs& QueryValues, const vector<unsigned char>& Data );

	virtual void FilterFrame( const AVFrame* Frame, Witness::Camera::ClassificationResult& Result, cv::Mat& InputFrame, cv::Mat& GrayscaleInputFrame ) = 0;

	void PrepareImage( const cv::Mat& InputFrame, vector<unsigned char>& Data );

	bool IsAllowedToProcessFrame();

protected:

	virtual const string_t CommandTypeToEndpoint( int CommandType ) = 0;
	virtual const string_t CommandTypeToMethod( int CommandType ) = 0;
	
	string_t Hostname;
	string_t EndpointBase;
	string_t ApiKey;
	double MinimumFrameDistance;
	uint64_t LastFrameTime;
};
