#include "CrowListener.h"
#include "Common.h"
#include "Database.h"
#include "SetupConfig.h"
#include "SetupServer.h"
#include "sodium.h"
#include "ObservingMotionFilter.h"
#include "Witness.h"
#include <Stream.h>
#include <ONNXDetectionFilter.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
}

#include <opencv2/core/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>

#include <windows.h>
#include <mmsystem.h>
#include <minmax.h>
#include <chrono>
#include <thread>
#include <iostream>
#include <Log.h>

#ifdef CROW_ENABLE_SSL
#include <openssl/applink.c>
#endif

#pragma comment(lib, "winmm.lib")

#define SERVICE_NAME L"WitnessCameraServer"
#define SERVICE_DISPLAY_NAME L"Witness Camera Service - CCTV Monitoring Server"
#define SERVICE_START_TYPE SERVICE_AUTO_START
#define SERVICE_ACCOUNT L"NT AUTHORITY\\NetworkService"
#define SERVICE_PASSWORD L""

bool ContinueRunning = true;

std::filesystem::path GetConfigFilePath(std::string Filename);

bool UpdateService(wchar_t* Path, bool Install)
{
	SC_HANDLE SCM = OpenSCManager(nullptr, nullptr, SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE);

	if (!SCM)
	{
		LOG_ERROR( "Unable to connect to service manager - admin access required." );
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
			LOG_ERROR( "Unable to create new service." );
			return false;
		}

		CloseServiceHandle(Service);

		LOG_INFO( "Created service." );
	}
	else
	{
		SC_HANDLE Service = OpenService(SCM, SERVICE_NAME, SERVICE_STOP | SERVICE_QUERY_STATUS | SERVICE_INTERROGATE | DELETE );
		if (!Service)
		{
			CloseServiceHandle(SCM);
			LOG_ERROR( "Service not found." );
			return false;
		}
		
		LOG_INFO( "Stopping service." );

		SERVICE_STATUS Status = {};
		if (ControlService(Service, SERVICE_CONTROL_STOP, &Status))
		{
			int Turns = 60;

			do {
				std::this_thread::sleep_for(std::chrono::milliseconds(1000));

				if (Status.dwCurrentState == SERVICE_STOPPED)
				{
					break;
				}

			} while (QueryServiceStatus(Service, &Status) && Turns--);
		}

		if (Status.dwCurrentState == SERVICE_STOPPED)
		{
			LOG_INFO( "Service stopped cleanly." );
		}
		else
		{
			LOG_WARNING( "Service failed to stop." );
		}

		if (!DeleteService(Service))
		{
			LOG_WARNING( "Could not delete service." );
		}
		else
		{
			LOG_INFO( "Deleted service." );
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
		LOG_ERROR( "Unable to initialize libsodium." );
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
	// Initialize logging early (before any LOG_* calls)
	{
		auto logDir = GetConfigFilePath( "logs" );
		std::filesystem::create_directories( logDir );
		Witness::LogInit( logDir.string(), Witness::LogLevel::Info );
	}

	if (argc >= 2)
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
			auto DatabaseFile = GetConfigFilePath("server.db");

			Database::InitializeDatabase(DatabaseFile.string());

			return 0;
		}
		else if (_wcsicmp(argv[1], L"/websetup") == 0)
		{
			// Force the web setup wizard regardless of whether an admin exists
			auto DatabaseFile = GetConfigFilePath("server.db");
			auto DB = Database::InitializeDatabase(DatabaseFile.string());

			if (sodium_init() == -1)
			{
				LOG_ERROR( "Unable to initialize libsodium." );
				return 1;
			}

			// Derive web root from exe path
			wchar_t exeBuf[MAX_PATH] = {};
			GetModuleFileNameW(nullptr, exeBuf, MAX_PATH);
			std::string staticRoot = (std::filesystem::path(exeBuf).parent_path() / "Web").string();

			SetupServer setup(DB, staticRoot);
			setup.Run();
			return 0;
		}
		else if (_wcsicmp(argv[1], L"/setup") == 0)
		{
			// Interactive CLI setup or JSON-based setup
			auto DatabaseFile = GetConfigFilePath("server.db");
			auto DB = Database::InitializeDatabase(DatabaseFile.string());

			if (argc >= 4 && _wcsicmp(argv[2], L"--json") == 0)
			{
				// Scripted setup: /setup --json <path>
				char jsonPath[MAX_PATH] = {};
				WideCharToMultiByte(CP_UTF8, 0, argv[3], -1, jsonPath, MAX_PATH, nullptr, nullptr);

				SetupConfig config;
				if (!config.LoadFromJson(jsonPath))
				{
					return 1;
				}
				if (!config.ApplyToDatabase(DB))
				{
					LOG_ERROR( "Failed to apply configuration." );
					return 1;
				}

				LOG_INFO( "Configuration applied successfully." );
				return 0;
			}
			else
			{
				// Interactive CLI setup
				if (sodium_init() == -1)
				{
					LOG_ERROR( "Unable to initialize libsodium." );
					return 1;
				}

				SetupConfig config;
				std::string input;

				LOG_INFO( "" );
				LOG_INFO( "========================================" );
				LOG_INFO( "  Witness Interactive Setup" );
				LOG_INFO( "========================================" );
				LOG_INFO( "" );

				std::cout << "Admin username: ";
				std::getline(std::cin, config.Username);

				std::cout << "Admin password: ";
				SetStdinEcho(false);
				std::getline(std::cin, config.Password);
				SetStdinEcho(true);
				std::cout << std::endl;

				std::cout << "Server hostname:port (e.g. localhost:8080): ";
				std::getline(std::cin, config.Hostname);

				std::cout << "TLS mode [NoSecurity/SelfSigned/LetsEncrypt/Manual]: ";
				std::getline(std::cin, config.TlsMode);
				if (config.TlsMode.empty()) config.TlsMode = "NoSecurity";

				std::cout << "Cache path [C:\\WitnessCache]: ";
				std::getline(std::cin, config.CachePath);
				if (config.CachePath.empty()) config.CachePath = "C:\\WitnessCache";

				if (!config.ApplyToDatabase(DB))
				{
					LOG_ERROR( "Failed to apply configuration." );
					return 1;
				}

				LOG_INFO( "Setup complete. Start WitnessServer normally to run." );
				return 0;
			}
		}
		else if (_wcsicmp(argv[1], L"/test-cuda") == 0)
		{
			// CUDA probe: test if GPU acceleration works without crashing the main server.
			// cuDNN calls __fastfail on error which kills the process -- this is expected.
			const char* cudnnPath = nullptr;
			char cudnnPathBuf[MAX_PATH] = {};
			if (argc >= 3)
			{
				WideCharToMultiByte(CP_UTF8, 0, argv[2], -1, cudnnPathBuf, MAX_PATH, nullptr, nullptr);
				cudnnPath = cudnnPathBuf;
			}

			// Find default model
			wchar_t exeBuf[MAX_PATH] = {};
			GetModuleFileNameW(nullptr, exeBuf, MAX_PATH);
			auto modelPath = std::filesystem::path(exeBuf).parent_path() / "models" / "yolo26n.onnx";
			std::string modelPathStr = modelPath.string();

			bool success = Witness::Camera::TestCudaAvailability(
				std::filesystem::exists(modelPath) ? modelPathStr.c_str() : nullptr,
				cudnnPath );
			return success ? 0 : 1;
		}
		else if (_wcsicmp(argv[1], L"/test-detection") == 0)
		{
			// Test detection on a clip file, outputting per-frame results.
			// Usage: /test-detection <clip_path> [--clahe] [--model <path>] [--confidence <float>] [--save-clahe <dir>]
			if (argc < 3)
			{
				std::cerr << "Usage: WitnessServer.exe /test-detection <clip.mp4> [--clahe] [--model <path>] [--confidence <float>] [--save-clahe <dir>]" << std::endl;
				return 1;
			}

			char clipPathBuf[MAX_PATH] = {};
			WideCharToMultiByte(CP_UTF8, 0, argv[2], -1, clipPathBuf, MAX_PATH, nullptr, nullptr);

			bool useClahe = false;
			float confidence = 0.3f;
			std::string saveClaheDir;

			wchar_t exeBuf[MAX_PATH] = {};
			GetModuleFileNameW(nullptr, exeBuf, MAX_PATH);
			auto defaultModel = std::filesystem::path(exeBuf).parent_path() / "models" / "yolo26n.onnx";
			std::string modelPath = defaultModel.string();

			for (int i = 3; i < argc; i++)
			{
				if (_wcsicmp(argv[i], L"--clahe") == 0)
					useClahe = true;
				else if (_wcsicmp(argv[i], L"--model") == 0 && i + 1 < argc)
				{
					char buf[MAX_PATH] = {};
					WideCharToMultiByte(CP_UTF8, 0, argv[++i], -1, buf, MAX_PATH, nullptr, nullptr);
					modelPath = buf;
				}
				else if (_wcsicmp(argv[i], L"--confidence") == 0 && i + 1 < argc)
				{
					char buf[64] = {};
					WideCharToMultiByte(CP_UTF8, 0, argv[++i], -1, buf, 64, nullptr, nullptr);
					confidence = (float)atof(buf);
				}
				else if (_wcsicmp(argv[i], L"--save-clahe") == 0 && i + 1 < argc)
				{
					char buf[MAX_PATH] = {};
					WideCharToMultiByte(CP_UTF8, 0, argv[++i], -1, buf, MAX_PATH, nullptr, nullptr);
					saveClaheDir = buf;
				}
			}

			if (!std::filesystem::exists(clipPathBuf))
			{
				std::cerr << "Clip not found: " << clipPathBuf << std::endl;
				return 1;
			}

			if (!std::filesystem::exists(modelPath))
			{
				std::cerr << "Model not found: " << modelPath << std::endl;
				return 1;
			}

			// Initialize FFmpeg network (required for avformat_open_input)
			avformat_network_init();

			// Create a minimal detection filter (no filter chain needed, just DetectFrame)
			Witness::Camera::MotionChainNode dummyChain;
			Witness::Camera::ONNXDetectionFilter filter( dummyChain, modelPath.c_str(), confidence, false );

			if (!filter.IsModelLoaded())
			{
				std::cerr << "Failed to load model: " << modelPath << std::endl;
				return 1;
			}

			// Open the clip with FFmpeg
			AVFormatContext* fmtCtx = nullptr;
			if (avformat_open_input(&fmtCtx, clipPathBuf, nullptr, nullptr) < 0)
			{
				std::cerr << "Failed to open clip: " << clipPathBuf << std::endl;
				return 1;
			}
			avformat_find_stream_info(fmtCtx, nullptr);

			int videoIdx = -1;
			for (unsigned i = 0; i < fmtCtx->nb_streams; i++)
			{
				if (fmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
				{
					videoIdx = i;
					break;
				}
			}
			if (videoIdx < 0)
			{
				std::cerr << "No video stream found" << std::endl;
				avformat_close_input(&fmtCtx);
				return 1;
			}

			auto codecpar = fmtCtx->streams[videoIdx]->codecpar;
			auto codec = avcodec_find_decoder(codecpar->codec_id);
			auto codecCtx = avcodec_alloc_context3(codec);
			avcodec_parameters_to_context(codecCtx, codecpar);
			avcodec_open2(codecCtx, codec, nullptr);

			SwsContext* swsCtx = sws_getContext(
				codecCtx->width, codecCtx->height, codecCtx->pix_fmt,
				codecCtx->width, codecCtx->height, AV_PIX_FMT_BGR24,
				SWS_BILINEAR, nullptr, nullptr, nullptr);

			double durationSec = 0.0;
			if (fmtCtx->duration > 0)
				durationSec = (double)fmtCtx->duration / AV_TIME_BASE;

			AVPacket* pkt = av_packet_alloc();
			AVFrame* frame = av_frame_alloc();
			AVFrame* bgrFrame = av_frame_alloc();

			int w = codecCtx->width, h = codecCtx->height;
			int bgrBufSize = av_image_get_buffer_size(AV_PIX_FMT_BGR24, w, h, 1);
			std::vector<uint8_t> bgrBuffer(bgrBufSize);
			av_image_fill_arrays(bgrFrame->data, bgrFrame->linesize, bgrBuffer.data(), AV_PIX_FMT_BGR24, w, h, 1);

			std::cout << "Clip: " << clipPathBuf << std::endl;
			std::cout << "Duration: " << durationSec << "s  Resolution: " << w << "x" << h << std::endl;
			std::cout << "Model: " << modelPath << "  Confidence: " << confidence << std::endl;
			std::cout << "CLAHE: " << (useClahe ? "ON" : "OFF") << std::endl;
			std::cout << "---" << std::endl;

			if (!saveClaheDir.empty())
				std::filesystem::create_directories(saveClaheDir);

			double sampleInterval = 2.0;
			double currentTime = 0.0;
			int frameNum = 0;

			while (currentTime <= durationSec || currentTime == 0.0)
			{
				int64_t seekTarget = (int64_t)(currentTime * AV_TIME_BASE);
				if (currentTime > 0.0)
				{
					av_seek_frame(fmtCtx, -1, seekTarget, AVSEEK_FLAG_BACKWARD);
					avcodec_flush_buffers(codecCtx);
				}

				bool gotFrame = false;
				while (av_read_frame(fmtCtx, pkt) >= 0)
				{
					if (pkt->stream_index == videoIdx)
					{
						avcodec_send_packet(codecCtx, pkt);
						if (avcodec_receive_frame(codecCtx, frame) == 0)
						{
							sws_scale(swsCtx, frame->data, frame->linesize, 0, h,
								bgrFrame->data, bgrFrame->linesize);

							cv::Mat mat(h, w, CV_8UC3, bgrFrame->data[0], bgrFrame->linesize[0]);
							auto lighting = Witness::Camera::ClassifyLighting(mat);

							const char* lightStr = "Unknown";
							if (lighting == Witness::Camera::LightingCondition::Day) lightStr = "Day";
							else if (lighting == Witness::Camera::LightingCondition::Night) lightStr = "Night";

							cv::Mat detectionMat = mat;
							if (useClahe && lighting == Witness::Camera::LightingCondition::Night)
								detectionMat = Witness::Camera::ApplyCLAHE(mat);

							// Save CLAHE comparison images if requested
							if (!saveClaheDir.empty() && lighting == Witness::Camera::LightingCondition::Night)
							{
								auto claheFrame = Witness::Camera::ApplyCLAHE(mat);
								std::string origPath = (std::filesystem::path(saveClaheDir) / ("frame_" + std::to_string(frameNum) + "_original.jpg")).string();
								std::string clahePath = (std::filesystem::path(saveClaheDir) / ("frame_" + std::to_string(frameNum) + "_clahe.jpg")).string();
								cv::imwrite(origPath, mat);
								cv::imwrite(clahePath, claheFrame);
								std::cout << "  Saved: " << origPath << " and " << clahePath << std::endl;
							}

							auto detections = filter.DetectFrame(detectionMat);

							std::cout << "Frame " << frameNum << " @ " << currentTime << "s  Lighting: " << lightStr;
							if (detections.empty())
							{
								std::cout << "  (no detections)" << std::endl;
							}
							else
							{
								std::cout << "  Detections:" << std::endl;
								for (auto& det : detections)
								{
									char buf[128];
									snprintf(buf, sizeof(buf), "    %-12s  %.1f%%", det.ClassName.c_str(), det.Confidence * 100.0f);
									std::cout << buf << std::endl;
								}
							}

							gotFrame = true;
							av_packet_unref(pkt);
							break;
						}
					}
					av_packet_unref(pkt);
				}

				if (!gotFrame)
					break;

				frameNum++;
				currentTime += sampleInterval;
			}

			av_frame_free(&bgrFrame);
			av_frame_free(&frame);
			av_packet_free(&pkt);
			sws_freeContext(swsCtx);
			avcodec_free_context(&codecCtx);
			avformat_close_input(&fmtCtx);
			return 0;
		}
		else if (_wcsicmp(argv[1], L"/apply-config") == 0 && argc >= 3)
		{
			// Elevated helper: read pending config JSON, apply privileged actions
			char configPath[MAX_PATH] = {};
			WideCharToMultiByte(CP_UTF8, 0, argv[2], -1, configPath, MAX_PATH, nullptr, nullptr);

			SetupConfig config;
			if (!config.LoadFromJson(configPath))
			{
				return 1;
			}

			auto DatabaseFile = GetConfigFilePath("server.db");
			auto DB = Database::InitializeDatabase(DatabaseFile.string());

			if (!config.ApplyToDatabase(DB))
			{
				LOG_ERROR( "Failed to apply configuration." );
				return 1;
			}

			// Apply privileged actions
			bool success = true;

			if (config.StartupMode == "Service")
			{
				success &= UpdateService(argv[0], true);
			}

			// Write status file for the web wizard to poll
			std::string statusPath = std::string(configPath) + ".status";
			std::ofstream statusFile(statusPath);
			if (statusFile)
			{
				crow::json::wvalue status;
				status["success"] = success;
				status["message"] = success ? "Configuration applied" : "Some operations failed";
				statusFile << status.dump();
			}

			// Clean up the config file
			std::filesystem::remove(configPath);

			return success ? 0 : 1;
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
