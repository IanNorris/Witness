#pragma once

#include "../Common.h"
#include "../SettingsMap.h"
#include "RecordFilter.h"

#include <cpprest/http_client.h>

using namespace Witness::Camera;

class AzureEndpointFilter : public Witness::Camera::IRecordFilter
{
public:

	typedef pair<string_t,string_t> QueryPair;
	typedef vector<QueryPair> QueryPairs;


	AzureEndpointFilter( const MotionChainNode& Chain, const SettingsMap& Settings );

	pplx::task<web::http::http_response> SendCommand( int CommandType, const json::value& RequestData, const QueryPairs& QueryValues, const vector<unsigned char>& Data );

	virtual ETaskType GetTaskType() { return ETaskType::ManualContinuation; }

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
