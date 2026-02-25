#pragma once

#include "../Common.h"
#include "../SettingsMap.h"
#include "RecordFilter.h"

#include <cpprest/http_client.h>

using namespace Witness::Camera;

class AzureEndpointFilter : public Witness::Camera::IRecordFilter
{
public:

	typedef std::pair<StringT,StringT> QueryPair;
	typedef std::vector<QueryPair> QueryPairs;


	AzureEndpointFilter( const MotionChainNode& Chain, const SettingsMap& Settings );

	pplx::task<web::http::http_response> SendCommand( int CommandType, const json::value& RequestData, const QueryPairs& QueryValues, const std::vector<unsigned char>& Data );

	virtual ETaskType GetTaskType() { return ETaskType::ManualContinuation; }

	bool IsAllowedToProcessFrame();

protected:

	virtual const StringT CommandTypeToEndpoint( int CommandType ) = 0;
	virtual const StringT CommandTypeToMethod( int CommandType ) = 0;
	
	StringT Hostname;
	StringT EndpointBase;
	StringT ApiKey;
	double MinimumFrameDistance;
	uint64_t LastFrameTime;
};
