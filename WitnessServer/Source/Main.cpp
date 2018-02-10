#include "Listener.h"
#include "Common.h"
#include "Database.h"
#include "Android/AndroidNotify.h"
#include "Commands/Authenticate.h"
#include "sodium.h"
#include "ObservingMotionFilter.h"
#include "Witness.h"

using namespace web::json;
using namespace web::http::client;
using namespace utility;

int wmain( int argc, wchar_t* argv[] )
{
	WitnessServer Server;

	if( sodium_init() == -1 )
	{
		std::tcerr << U("Unable to initialize libsodium.") << std::endl;
        return 1;
    }

	if (!Server.Initialize())
	{
		return 1;
	}

	Server.MessageLoop();
	Server.Shutdown();

	return 0;
}
