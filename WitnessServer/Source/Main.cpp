#include "Listener.h"
#include "Common.h"
#include "Database.h"
#include "Android/AndroidNotify.h"
#include "Commands/Authenticate.h"
#include "sodium.h"
#include "ObservingMotionFilter.h"
#include "Witness.h"

#include <windows.h>
#include <minmax.h>
#pragma comment(lib, "winmm.lib")

using namespace web::json;
using namespace web::http::client;
using namespace utility;

int wmain( int argc, wchar_t* argv[] )
{
	struct ScopedTimePeriod
	{
		ScopedTimePeriod(UINT Period)
		: Period( Period )
		{
			timeBeginPeriod( Period );
		}

		~ScopedTimePeriod()
		{
			timeEndPeriod( Period );
		}

		UINT Period;
	};

	TIMECAPS TimeCaps;
	timeGetDevCaps( &TimeCaps, sizeof(TimeCaps) );
	auto Resolution = min(max(TimeCaps.wPeriodMin, 0), TimeCaps.wPeriodMax);

	ScopedTimePeriod TimePeriod( Resolution );

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
