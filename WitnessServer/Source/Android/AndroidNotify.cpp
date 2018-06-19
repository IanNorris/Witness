#include "AndroidNotify.h"

#include "cpprest/http_client.h"
#include "cpprest/uri.h"
#include "cpprest/asyncrt_utils.h"

using namespace utility;
using namespace web::json;
using namespace web::http::client;

void SendAndroidNotification( utility::string_t ServerKey, utility::string_t TargetUser, utility::string_t MessageText, utility::string_t CameraName, utility::string_t ImageUri, std::function<void(http_response)> OnComplete )
{
	value PostParameters = value::object();
	PostParameters[U("to")] = value::string(TargetUser);

	int TimeToLive = 60*60;

	auto Data = value::object();
	Data[U("alert")] = value::string( MessageText );
	Data[U("cameraSource")] = value::string( CameraName );
	Data[U("image")] = value::string( ImageUri );

	PostParameters[U("data")] = Data;
	PostParameters[U("priority")] = value::string( U("high") );
	PostParameters[U("time_to_live")] = value::number(TimeToLive);
	
	http_client client( U("https://fcm.googleapis.com") );
	
	http_request Request( methods::POST );
	Request.headers().set_content_type( U("application/json") );
	Request.headers().add( U("Authorization"), U("key=") + ServerKey );
	Request.set_request_uri( U("/fcm/send") );
	
	string_t PostData = PostParameters.serialize();

	Request.set_body( PostData );

	auto Response = client.request( Request );

	if( OnComplete )
	{
		Response.then( [=](pplx::task<http_response> ResponseAsync)
		{
			OnComplete( Response.get() );
		} );
	}
}
