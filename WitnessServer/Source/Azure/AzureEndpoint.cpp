#include "AzureEndpoint.h"
#include <cpprest/uri.h> 
#include <cpprest/http_client.h>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgcodecs/imgcodecs_c.h>
#include <opencv2/core/core_c.h>
#include <opencv2/imgproc.hpp>
#include <opencv2/imgproc/imgproc_c.h>

AzureEndpointFilter::AzureEndpointFilter( const SettingsMap& Settings )
{
	string_t EndpointName = _T("endpoint");
	string_t KeyName = _T("key");
	string_t MinFrameDistanceName = _T("min_frame_distance");

	LastFrameTime = 0;

	auto EndpointUriIter = Settings.Settings.find(EndpointName);
	auto ApiKeyIter = Settings.Settings.find(KeyName);
	auto MinFrameDistanceIter = Settings.Settings.find(MinFrameDistanceName);

	string_t EndpointUri;

	if (EndpointUriIter != Settings.Settings.end())
	{
		EndpointUri = (*EndpointUriIter).second;
	}
	else
	{
		wprintf(_T("No endpoint specified for '%s'.\n"), Settings.Name.c_str() );
		return;
	}

	if (ApiKeyIter != Settings.Settings.end())
	{
		ApiKey = (*ApiKeyIter).second;
	}
	else
	{
		wprintf(_T("No key specified for '%s'.\n"), ApiKey.c_str() );
		return;
	}

	if (MinFrameDistanceIter != Settings.Settings.end())
	{
		MinimumFrameDistance = _wtof( (*MinFrameDistanceIter).second.c_str() );
	}
	else
	{
		wprintf(_T("No min_frame_distance specified for '%s'.\n"), Settings.Name.c_str() );
		return;
	}

	web::uri_builder DeconstructEndpoint(EndpointUri);
	Hostname = _T("https://") + DeconstructEndpoint.host();
	EndpointBase = DeconstructEndpoint.path();
}

pplx::task<web::http::http_response> AzureEndpointFilter::SendCommand(int CommandType, const json::value& RequestData, const QueryPairs& QueryValues, const vector<unsigned char>& Data)
{
	const string_t FinalEndpointUri = CommandTypeToEndpoint(CommandType);
	const string_t Method = CommandTypeToMethod(CommandType);
	
	web::uri_builder Uri( FinalEndpointUri );

	for( auto& QueryValue : QueryValues )
	{
		Uri.append_query( QueryValue.first, QueryValue.second, true );
	}
	
	web::http::client::http_client Client( Hostname );

	web::http::http_request Request( Method );
	Request.set_request_uri( Uri.to_uri() );
	Request.headers().add( _T("Ocp-Apim-Subscription-Key"), ApiKey );
	
	if( Data.size() )
	{
		Request.set_body( Data );
		Request.headers().content_type() = _T("application/octet-stream");
	}
	else
	{
		const string_t RestData = RequestData.serialize();
		Request.set_body( RestData );
		Request.headers().content_type() = _T("application/json");
	}

	return Client.request( Request );
}

bool AzureEndpointFilter::IsAllowedToProcessFrame()
{
	const double NanoSecondsToSeconds = 1000.0 * 1000.0 * 1000.0;

	uint64_t Now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
	bool Allowed = ((double)(Now - LastFrameTime) / NanoSecondsToSeconds) > MinimumFrameDistance;

	if (Allowed)
	{
		LastFrameTime = Now;
		return true;
	}

	return false;
}

void AzureEndpointFilter::PrepareImage(const cv::Mat& InputFrame, vector<unsigned char>& Data)
{
	float Aspect = (float)InputFrame.cols / (float)InputFrame.rows;

	const int TargetSize = 1280;
	const int Quality = 70;

	cv::Mat ResizedImage;
	cv::resize( InputFrame, ResizedImage, cv::Size(TargetSize,(int)((float)TargetSize/Aspect)), 0, 0 );
	
	cv::imencode( ".jpg", InputFrame, Data, std::vector<int>{ CV_IMWRITE_JPEG_QUALITY, Quality } );
}