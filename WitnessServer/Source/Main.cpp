#include "CrowListener.h"
#include "Common.h"
#include "Database.h"
#include "sodium.h"
#include "ObservingMotionFilter.h"
#include "Witness.h"
#include <Stream.h>

#include <windows.h>
#include <mmsystem.h>
#include <minmax.h>
#include <chrono>
#include <thread>
#pragma comment(lib, "winmm.lib")

#define SERVICE_NAME L"WitnessCameraServer"
#define SERVICE_DISPLAY_NAME L"Witness Camera Service - CCTV Monitoring Server"
#define SERVICE_START_TYPE SERVICE_AUTO_START
#define SERVICE_ACCOUNT L"NT AUTHORITY\\NetworkService"
#define SERVICE_PASSWORD L""

bool ContinueRunning = true;

std::filesystem::path GetConfigFilePath(StringT Filename);

bool UpdateService(wchar_t* Path, bool Install)
{
	SC_HANDLE SCM = OpenSCManager(nullptr, nullptr, SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE);

	if (!SCM)
	{
		std::cerr << U("Unable to connect to service manager - admin access required.") << std::endl;
		return false;
	}

	if (Install)
	{
		SC_HANDLE Service = OpenService(SCM, SERVICE_NAME, SERVICE_STOP | SERVICE_QUERY_STATUS | SERVICE_INTERROGATE  | DELETE);
		if (Service)
		{
			CloseServiceHandle(Service);
			UpdateService(Path, false);
		}

		Service = CreateService(SCM, SERVICE_NAME, SERVICE_DISPLAY_NAME, SERVICE_QUERY_STATUS, SERVICE_WIN32_OWN_PROCESS, SERVICE_START_TYPE, SERVICE_ERROR_NORMAL, Path, NULL, NULL, NULL, SERVICE_ACCOUNT, SERVICE_PASSWORD);
		if (!Service)
		{
			CloseServiceHandle(SCM);
			std::cerr << U("Unable to create new service.") << std::endl;
			return false;
		}

		CloseServiceHandle(Service);

		std::cout << U("Created service.") << std::endl;
	}
	else
	{
		SC_HANDLE Service = OpenService(SCM, SERVICE_NAME, SERVICE_STOP | SERVICE_QUERY_STATUS | SERVICE_INTERROGATE | DELETE );
		if (!Service)
		{
			CloseServiceHandle(SCM);
			std::cerr << U("Service not found.") << std::endl;
			return false;
		}
		
		std::cout << U("Stopping service.");

		SERVICE_STATUS Status = {};
		if (ControlService(Service, SERVICE_CONTROL_STOP, &Status))
		{
			int Turns = 60;

			do {
				std::this_thread::sleep_for(std::chrono::milliseconds(1000));
				std::cout << U(".");

				if (Status.dwCurrentState == SERVICE_STOPPED)
				{
					break;
				}

			} while (QueryServiceStatus(Service, &Status) && Turns--);
		}

		if (Status.dwCurrentState == SERVICE_STOPPED)
		{
			std::cout << U("\nService stopped cleanly.") << std::endl;
		}
		else
		{
			std::cout << U("\nService failed to stop.") << std::endl;
		}

		if (!DeleteService(Service))
		{
			std::cout << U("Could not delete service.") << std::endl;
		}
		else
		{
			std::cout << U("Deleted service.") << std::endl;
		}

		CloseServiceHandle(Service);
	}

	CloseServiceHandle(SCM);

	return true;
}

WitnessServer* GlobalServer = nullptr;
bool IsService = true;
SERVICE_STATUS ServiceStatus = {};
SERVICE_STATUS_HANDLE ServiceHandle = nullptr;
int ReturnValue = 0;

void UpdateStatus(DWORD NewStatus, DWORD WaitHintInSeconds = 0, DWORD ErrorCode = 0)
{
	if (IsService)
	{
		ServiceStatus.dwCurrentState = NewStatus;
		ServiceStatus.dwWin32ExitCode = ErrorCode;
		ServiceStatus.dwWaitHint = WaitHintInSeconds * 1000;
		++ServiceStatus.dwCheckPoint;

		SetServiceStatus(ServiceHandle, &ServiceStatus);
	}
}

void WINAPI ServiceController(DWORD Action)
{
	switch (Action)
	{
	case SERVICE_CONTROL_STOP:
	case SERVICE_CONTROL_SHUTDOWN:
		UpdateStatus(SERVICE_STOP_PENDING, 20);
		ContinueRunning = false;
		if (GlobalServer)
		{
			GlobalServer->RequestShutdown();
		}
		break;
	case SERVICE_CONTROL_INTERROGATE:
		break;
	}
}

BOOL WINAPI ConsoleHandlerRoutine(DWORD ControlType)
{
	ContinueRunning = false;
	if (GlobalServer)
	{
		GlobalServer->RequestShutdown();
		GlobalServer = nullptr;
	}
	return true;
}

void WINAPI ServiceMain(DWORD dwArgc, PWSTR* pszArgv)
{
	if (IsService)
	{
		ServiceHandle = RegisterServiceCtrlHandler(SERVICE_NAME, ServiceController);

		ServiceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
		ServiceStatus.dwCurrentState = SERVICE_START_PENDING;
		ServiceStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
		ServiceStatus.dwWin32ExitCode = NO_ERROR;
		ServiceStatus.dwServiceSpecificExitCode = 0;
		ServiceStatus.dwCheckPoint = 0;
		ServiceStatus.dwWaitHint = 0;

		UpdateStatus(SERVICE_START_PENDING, 30);

		std::this_thread::sleep_for(std::chrono::milliseconds(10000));
	}
	else
	{
		SetConsoleCtrlHandler(ConsoleHandlerRoutine, true);
	}

	struct ScopedTimePeriod
	{
		ScopedTimePeriod(UINT Period)
			: Period(Period)
		{
			timeBeginPeriod(Period);
		}

		~ScopedTimePeriod()
		{
			timeEndPeriod(Period);
		}

		UINT Period;
	};

	TIMECAPS TimeCaps;
	timeGetDevCaps(&TimeCaps, sizeof(TimeCaps));
	auto Resolution = min(max(TimeCaps.wPeriodMin, 0), TimeCaps.wPeriodMax);

	ScopedTimePeriod TimePeriod(Resolution);

	DebugConsole DebugConsoleInstance;
	Witness::Camera::TargetDebugConsole = &DebugConsoleInstance;

	WitnessServer Server;
	GlobalServer = &Server;

	if (sodium_init() == -1)
	{
		std::cerr << U("Unable to initialize libsodium.") << std::endl;
		ReturnValue = 1;
		UpdateStatus(SERVICE_STOPPED, 0, ReturnValue);
		return;
	}

	if (!Server.Initialize(&DebugConsoleInstance))
	{
		ReturnValue = 1;
		UpdateStatus(SERVICE_STOPPED, 0, ReturnValue);
		return;
	}

	UpdateStatus( SERVICE_RUNNING );

	Server.MessageLoop(ContinueRunning);

	GlobalServer = nullptr;
	Server.Shutdown();

	Witness::Camera::TargetDebugConsole = nullptr;

	UpdateStatus( SERVICE_STOPPED );

	ReturnValue = 0;
}

int wmain( int argc, wchar_t* argv[] )
{
	if (argc == 2)
	{
		if (_wcsicmp(argv[1], L"/installservice") == 0)
		{
			return UpdateService(argv[0], true);
		}
		else if (_wcsicmp(argv[1], L"/uninstallservice") == 0)
		{
			return UpdateService(argv[0], false);
		}
		else if (_wcsicmp(argv[1], L"/createdb") == 0)
		{
			auto DatabaseFile = GetConfigFilePath(U("server.db"));

			Database::InitializeDatabase(DatabaseFile.string());

			return 0;
		}
	}


	SERVICE_TABLE_ENTRY Services[] =
	{
		{ const_cast<LPWSTR>(SERVICE_NAME), ServiceMain },
		{ NULL, NULL }
	};

	if (!StartServiceCtrlDispatcher(Services))
	{
		DWORD Error = GetLastError();
		//We're not a service
		if (Error == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT)
		{
			IsService = false;
			ServiceMain(NULL, 0);
			return ReturnValue;
		}
		else
		{
			//Something went wrong
			return Error;
		}
	}

	return 0;
}
