#include "CrowListener.h"
#include "CrowAuth.h"
#include "GlobalContext.h"
#include "Messages.h"
void CrowListener::HandlePreview( const crow::request& req, crow::response& res, int cameraId, bool largePreview )
{
	int UserUID = CrowAuth::IsCameraAuthenticated( *m_GlobalContext, req, nullptr,
		CrowAuth::Action::Read, CrowAuth::Privilege::Normal, cameraId );
	if( UserUID <= 0 )
	{
		res.code = 403;
		res.end();
		return;
	}

	auto Camera = m_GlobalContext->FindCameraById( cameraId );
	if( Camera )
	{
		if( largePreview )
		{
			Camera->LastLargePreviewTimestamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
		}
		else
		{
			Camera->LastSmallPreviewTimestamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
		}

		auto PreviewRequest = std::make_shared<CameraPreviewRequestMessage>();
		PreviewRequest->LastLargePreviewTimestamp = Camera->LastLargePreviewTimestamp;
		PreviewRequest->LastSmallPreviewTimestamp = Camera->LastSmallPreviewTimestamp;
		m_GlobalContext->MessageBus->SendToClient( Camera->Worker.get(), PreviewRequest );

		res.set_header( "Content-Type", "image/jpeg" );
		res.set_header( "Cache-Control", "no-cache, no-store, must-revalidate" );
		res.body.assign( (const char*)Camera->PreviewThumbnail.data(), Camera->PreviewThumbnail.size() );
		res.code = 200;
		res.end();
	}
	else
	{
		res.code = 404;
		res.end();
	}
}

void CrowListener::HandleCameraEnum( const crow::request& req, crow::response& res, bool asAdmin, bool longPoll )
{
	int UserUID = CrowAuth::IsAuthenticated( *m_GlobalContext, req, nullptr,
		CrowAuth::Action::Read, asAdmin ? CrowAuth::Privilege::Administrator : CrowAuth::Privilege::Normal );
	if( UserUID < 0 )
	{
		res.code = 400;
		res.end();
		return;
	}

	bool First = true;
	std::vector<int> State;
	std::vector<int> OriginalState;
	std::vector<crow::json::wvalue> Array;

	bool IsAcceptable = false;
	do {
		Array.clear();
		State.clear();

		{
			SQLiteDatabaseQueryInstance GetCamerasForUser( m_GlobalContext->Database, asAdmin ? "GetCameras" : "GetCamerasForUser" );
			GetCamerasForUser->Bind( "@User", UserUID );

			GetCamerasForUser->Execute(
				[&]( const SQLiteDatabaseQuery& query )
				{
					int ID = query.GetColumnValueInt( 0 );
					std::string Name = query.GetColumnValueText( 1 );
					std::string ConnectionString = query.GetColumnValueText( 2 );
					std::string ConnectionStringSub = query.GetColumnValueText( 3 );
					std::string Description = query.GetColumnValueText( 4 ) ? query.GetColumnValueText( 4 ) : "";
					int Enabled = query.GetColumnValueInt( 5 );

					if( Enabled || asAdmin )
					{
						crow::json::wvalue Camera;
						Camera["id"] = ID;
						Camera["name"] = Name;
						Camera["description"] = Description;
						Camera["enabled"] = Enabled;

						std::vector<crow::json::wvalue> Groups;

						SQLiteDatabaseQueryInstance SelectGroupsForCamera( m_GlobalContext->Database, "SelectGroupsForCamera" );
						SelectGroupsForCamera->Bind( "@Camera", ID );

						SelectGroupsForCamera->Execute(
							[&Groups]( const SQLiteDatabaseQuery& query )
							{
								int GroupId = query.GetColumnValueInt( 1 );
								Groups.push_back( GroupId );
								return true;
							}
						);

						if( asAdmin )
						{
							Camera["connectionString"] = ConnectionString;
							Camera["connectionStringSub"] = ConnectionStringSub;

							int SkipFrames = query.GetColumnValueInt( 6 );
							int MDFrameHeight = query.GetColumnValueInt( 7 );
							double MDThreshold = query.GetColumnValueDouble( 8 );
							const char* MotionFilter = query.GetColumnValueText( 9 );
							const char* BlackoutMaskPath = query.GetColumnValueText( 10 );
							const char* FocusMaskPath = query.GetColumnValueText( 11 );

							Camera["skipFrames"] = SkipFrames;
							Camera["mdFrameHeight"] = MDFrameHeight;
							Camera["mdThreshold"] = MDThreshold;
							Camera["motionFilter"] = MotionFilter ? MotionFilter : "";
							Camera["blackoutMaskPath"] = BlackoutMaskPath ? BlackoutMaskPath : "";
							Camera["focusMaskPath"] = FocusMaskPath ? FocusMaskPath : "";

							int ContinuousRecording = query.GetColumnValueInt( 12 );
							Camera["continuousRecording"] = ContinuousRecording;
						}

						Camera["groups"] = std::move( Groups );

						{
							auto CameraState = m_GlobalContext->FindCameraById( ID );
							if( CameraState )
							{
								if( longPoll )
								{
									State.push_back( ID );
									State.push_back( CameraState->IsRecording );
								}

								Camera["status"] = CameraState->Status;
								Camera["recording"] = CameraState->IsRecording;

								auto StreamStats = CameraState->Worker->GetStreamStats();

								auto ImgStats = m_GlobalContext->CommonImageProcessingJobQueue->GetStats( ID );
								Camera["lastTimestamp"] = ImgStats.LastTimestamp;

								if( asAdmin && ImgStats.FrameCount > 0 )
								{
									Camera["frameCount"] = ImgStats.FrameCount;

#define GET_STAT_CROW(OutputPrefix, StatName) \
	Camera[ OutputPrefix "TimeOfEachMS" ] = (double)ImgStats.Stats.FrameCount[StatName] ? ((double)ImgStats.Stats.Stats[StatName] / ((double)ImgStats.Stats.FrameCount[StatName] * 1000.0 * 1000.0)) : 0.0;\
	Camera[ OutputPrefix "ActualMS" ] = ImgStats.FrameCount ? (double)ImgStats.Stats.Stats[StatName] / ((double)ImgStats.FrameCount * 1000.0 * 1000.0) : 0

									GET_STAT_CROW("processing", FilterStat_Process_Total);
									GET_STAT_CROW("scale", FilterStat_Scale);
									GET_STAT_CROW("jpegEncoding", FilterStat_JpegEncoding);
									GET_STAT_CROW("observer", FilterStat_ObserverFilter);
									GET_STAT_CROW("firstPassFilter", FilterStat_FirstPassFilter);
									GET_STAT_CROW("secondPassFilter", FilterStat_SecondPassFilter);
									GET_STAT_CROW("thirdPassFilter", FilterStat_ThirdPassFilter);
									GET_STAT_CROW("debug", FilterStat_Debug);

									GET_STAT_CROW("mvfInternal", FilterStat_MVF_Internal);
									GET_STAT_CROW("mvfSideData", FilterStat_MVF_SideData);
									GET_STAT_CROW("mvfVectorPass", FilterStat_MVF_VectorPass);
									GET_STAT_CROW("mvfClusterPass", FilterStat_MVF_ClusterPass);
									GET_STAT_CROW("mvfObjectPass", FilterStat_MVF_ObjectPass);

#undef GET_STAT_CROW
								}

								if( asAdmin && StreamStats.FrameCount > 0 )
								{
									double Decode = (double)StreamStats.DecoderTimeTotal / ((double)StreamStats.FrameCount * 1000.0 * 1000.0);
									double Output = (double)StreamStats.OutputTimeTotal / ((double)StreamStats.FrameCount * 1000.0 * 1000.0);
									double Read = (double)StreamStats.ReadTimeTotal / ((double)StreamStats.FrameCount * 1000.0 * 1000.0);

									Camera["streamReadTimeMS"] = Read;
									Camera["streamDecodeTimeMS"] = Decode;
									Camera["streamOutputTimeMS"] = Output;
								}
							}
						}

						Array.push_back( std::move( Camera ) );
					}

					return true;
				}
			);
		}

		if( longPoll )
		{
			if( First )
			{
				OriginalState = State;
				First = false;
			}
			else
			{
				if( OriginalState.size() != State.size() )
				{
					IsAcceptable = true;
				}
				else
				{
					bool Same = true;
					for( size_t Index = 0; Index < OriginalState.size(); Index++ )
					{
						if( OriginalState[Index] != State[Index] )
						{
							Same = false;
							IsAcceptable = true;
							break;
						}
					}

					if( !IsAcceptable )
					{
						LongPollScope Scope( m_GlobalContext->LongPoll );
						Scope.Wait();
					}
				}
			}
		}
		else
		{
			IsAcceptable = true;
		}
	} while( !IsAcceptable );

	crow::json::wvalue Result = std::move( Array );
	res.set_header( "Content-Type", "application/json" );
	res.body = Result.dump();
	res.code = 200;
	res.end();
}

void CrowListener::HandleCameraRecord( const crow::request& req, crow::response& res, int cameraId )
{
	auto body = crow::json::load( req.body );
	if( !body )
	{
		res.code = 400;
		res.end();
		return;
	}

	int UserUID = CrowAuth::IsCameraAuthenticated( *m_GlobalContext, req, &body,
		CrowAuth::Action::Read, CrowAuth::Privilege::Normal, cameraId );
	if( UserUID <= 0 )
	{
		res.code = 403;
		res.end();
		return;
	}

	if( !body.has( "record" ) )
	{
		res.code = 400;
		res.body = "Missing 'record' field";
		res.end();
		return;
	}

	bool Record = body["record"].b();

	auto CameraState = m_GlobalContext->FindCameraById( cameraId );
	if( CameraState )
	{
		auto ToggleRecord = std::make_shared<CameraStateToggleRecordMessage>( cameraId, Record );
		m_GlobalContext->MessageBus->SendToClient( nullptr, ToggleRecord );
	}

	res.set_header( "Content-Type", "application/json" );
	res.body = "\"OK\"";
	res.code = 200;
	res.end();
}

void CrowListener::HandleCameraCreate( const crow::request& req, crow::response& res )
{
	auto body = crow::json::load( req.body );
	if( !body )
	{
		res.code = 400;
		res.end();
		return;
	}

	int UserUID = CrowAuth::IsAuthenticated( *m_GlobalContext, req, &body,
		CrowAuth::Action::ReadWrite, CrowAuth::Privilege::Administrator );
	if( UserUID < 0 )
	{
		res.code = 400;
		res.end();
		return;
	}

	std::string DisplayName = body.has("displayName") ? std::string(body["displayName"].s()) : "";
	std::string Description = body.has("description") ? std::string(body["description"].s()) : "";
	std::string ConnectionString = body.has("connectionString") ? std::string(body["connectionString"].s()) : "";
	std::string ConnectionStringSub = body.has("connectionStringSub") ? std::string(body["connectionStringSub"].s()) : "";

	std::string DisplayNameW( DisplayName.begin(), DisplayName.end() );
	std::string DescriptionW( Description.begin(), Description.end() );
	std::string ConnectionStringW( ConnectionString.begin(), ConnectionString.end() );
	std::string ConnectionStringSubW( ConnectionStringSub.begin(), ConnectionStringSub.end() );

	SQLiteDatabaseQueryInstance CreateCamera( m_GlobalContext->Database, "CreateCamera" );
	CreateCamera->Bind( "@CameraName", DisplayNameW.c_str() );
	CreateCamera->Bind( "@Description", DescriptionW.c_str() );
	CreateCamera->Bind( "@CameraString", ConnectionStringW.c_str() );
	CreateCamera->Bind( "@CameraStringSub", ConnectionStringSubW.c_str() );

	if( CreateCamera->Execute( [&]( const SQLiteDatabaseQuery& query ) { return true; } ) < 0 )
	{
		crow::json::wvalue Data;
		Data["errorMessage"] = CreateCamera->GetLastError();
		res.set_header( "Content-Type", "application/json" );
		res.body = Data.dump();
		res.code = 400;
		res.end();
		return;
	}

	int64_t RowResult = CreateCamera->GetLastInsertionId();

	auto AddMessage = std::make_shared<CameraAddedMessage>( (int)RowResult );
	m_GlobalContext->MessageBus->SendToClient( nullptr, AddMessage );

	res.set_header( "Content-Type", "application/json" );
	res.body = "\"OK\"";
	res.code = 200;
	res.end();
}

void CrowListener::HandleCameraDelete( const crow::request& req, crow::response& res )
{
	auto body = crow::json::load( req.body );
	if( !body )
	{
		res.code = 400;
		res.end();
		return;
	}

	int UserUID = CrowAuth::IsAuthenticated( *m_GlobalContext, req, &body,
		CrowAuth::Action::ReadWrite, CrowAuth::Privilege::Administrator );
	if( UserUID < 0 )
	{
		res.code = 400;
		res.end();
		return;
	}

	if( !body.has("id") )
	{
		res.code = 400;
		res.body = "Missing 'id' field";
		res.end();
		return;
	}

	int CameraUID = (int)body["id"].i();

	{
		SQLiteDatabaseQueryInstance DeleteCamera( m_GlobalContext->Database, "DeleteCamera" );
		DeleteCamera->Bind( "@CameraId", CameraUID );
		DeleteCamera->Execute( nullptr );
	}

	auto DeleteMsg = std::make_shared<CameraRemovedMessage>( CameraUID );
	m_GlobalContext->MessageBus->SendToClient( nullptr, DeleteMsg );

	res.set_header( "Content-Type", "application/json" );
	res.body = "{}";
	res.code = 200;
	res.end();
}

void CrowListener::HandleCameraSetGroups( const crow::request& req, crow::response& res )
{
	auto body = crow::json::load( req.body );
	if( !body )
	{
		res.code = 400;
		res.end();
		return;
	}

	int UserUID = CrowAuth::IsAuthenticated( *m_GlobalContext, req, &body,
		CrowAuth::Action::ReadWrite, CrowAuth::Privilege::Administrator );
	if( UserUID < 0 )
	{
		res.code = 400;
		res.end();
		return;
	}

	if( !body.has("camera") || !body.has("value") )
	{
		res.code = 400;
		res.body = "Missing 'camera' or 'value' field";
		res.end();
		return;
	}

	int CameraID = (int)body["camera"].i();

	std::vector<int> CameraGroupsRequested;
	for( auto& Element : body["value"] )
	{
		CameraGroupsRequested.push_back( (int)Element.i() );
	}

	std::vector<int> CameraGroupsCurrent;

	SQLiteDatabaseQueryInstance SelectGroupsForCamera( m_GlobalContext->Database, "SelectGroupsForCamera" );
	SelectGroupsForCamera->Bind( "@Camera", CameraID );

	SelectGroupsForCamera->Execute(
		[&]( const SQLiteDatabaseQuery& query )
		{
			int Group = query.GetColumnValueInt( 1 );
			CameraGroupsCurrent.push_back( Group );
			return true;
		}
	);

	// Add new groups
	for( int Value : CameraGroupsRequested )
	{
		if( find( CameraGroupsCurrent.begin(), CameraGroupsCurrent.end(), Value ) == CameraGroupsCurrent.end() )
		{
			SQLiteDatabaseQueryInstance CreateMapping( m_GlobalContext->Database, "CreateCameraGroupMapping" );
			CreateMapping->Bind( "@Camera", CameraID );
			CreateMapping->Bind( "@Group", Value );
			CreateMapping->Execute( [&]( const SQLiteDatabaseQuery& query ) { return true; } );
		}
	}

	// Remove old groups
	for( int Value : CameraGroupsCurrent )
	{
		if( find( CameraGroupsRequested.begin(), CameraGroupsRequested.end(), Value ) == CameraGroupsRequested.end() )
		{
			SQLiteDatabaseQueryInstance DeleteMapping( m_GlobalContext->Database, "DeleteCameraGroupMapping" );
			DeleteMapping->Bind( "@Camera", CameraID );
			DeleteMapping->Bind( "@Group", Value );
			DeleteMapping->Execute( [&]( const SQLiteDatabaseQuery& query ) { return true; } );
		}
	}

	m_GlobalContext->LongPoll->NotifyAll();

	res.set_header( "Content-Type", "application/json" );
	res.body = "{}";
	res.code = 200;
	res.end();
}

void CrowListener::HandleCameraResetStats( const crow::request& req, crow::response& res )
{
	auto body = crow::json::load( req.body );
	if( !body )
	{
		res.code = 400;
		res.end();
		return;
	}

	int UserUID = CrowAuth::IsAuthenticated( *m_GlobalContext, req, &body,
		CrowAuth::Action::ReadWrite, CrowAuth::Privilege::Administrator );
	if( UserUID < 0 )
	{
		res.code = 400;
		res.end();
		return;
	}

	{
		std::lock_guard<std::mutex> Lock( m_GlobalContext->Mutex );

		for( auto Camera : m_GlobalContext->GetCameraMap() )
		{
			m_GlobalContext->CommonImageProcessingJobQueue->ResetStats( Camera.first );
		}
	}

	res.set_header( "Content-Type", "application/json" );
	res.body = "\"OK\"";
	res.code = 200;
	res.end();
}

void CrowListener::HandleCameraUpdate( const crow::request& req, crow::response& res )
{
	auto body = crow::json::load( req.body );
	if( !body )
	{
		res.code = 400;
		res.end();
		return;
	}

	int UserUID = CrowAuth::IsAuthenticated( *m_GlobalContext, req, &body,
		CrowAuth::Action::ReadWrite, CrowAuth::Privilege::Administrator );
	if( UserUID < 0 )
	{
		res.code = 400;
		res.end();
		return;
	}

	if( !body.has("id") )
	{
		res.code = 400;
		res.body = "Missing 'id' field";
		res.end();
		return;
	}

	int CameraID = (int)body["id"].i();

	std::string DisplayName = body.has("displayName") ? std::string(body["displayName"].s()) : "";
	std::string ConnectionString = body.has("connectionString") ? std::string(body["connectionString"].s()) : "";
	std::string ConnectionStringSub = body.has("connectionStringSub") ? std::string(body["connectionStringSub"].s()) : "";
	std::string Description = body.has("description") ? std::string(body["description"].s()) : "";
	int Enabled = body.has("enabled") ? (int)body["enabled"].i() : 1;
	int SkipFrames = body.has("skipFrames") ? (int)body["skipFrames"].i() : 1;
	int MDFrameHeight = body.has("mdFrameHeight") ? (int)body["mdFrameHeight"].i() : 400;
	double MDThreshold = body.has("mdThreshold") ? body["mdThreshold"].d() : 0.0001;
	std::string MotionFilter = body.has("motionFilter") ? std::string(body["motionFilter"].s()) : "";
	std::string BlackoutMaskPath = body.has("blackoutMaskPath") ? std::string(body["blackoutMaskPath"].s()) : "";
	std::string FocusMaskPath = body.has("focusMaskPath") ? std::string(body["focusMaskPath"].s()) : "";
	int ContinuousRecording = body.has("continuousRecording") ? (int)body["continuousRecording"].i() : 0;

	SQLiteDatabaseQueryInstance UpdateCamera( m_GlobalContext->Database, "UpdateCamera" );
	UpdateCamera->Bind( "@CameraId", CameraID );
	UpdateCamera->Bind( "@CameraName", DisplayName.c_str() );
	UpdateCamera->Bind( "@CameraString", ConnectionString.c_str() );
	UpdateCamera->Bind( "@CameraStringSub", ConnectionStringSub.c_str() );
	UpdateCamera->Bind( "@Description", Description.c_str() );
	UpdateCamera->Bind( "@Enabled", Enabled );
	UpdateCamera->Bind( "@SkipFrames", SkipFrames );
	UpdateCamera->Bind( "@MDFrameHeight", MDFrameHeight );
	UpdateCamera->Bind( "@MDThreshold", MDThreshold );
	UpdateCamera->Bind( "@MotionFilter", MotionFilter.empty() ? nullptr : MotionFilter.c_str() );
	UpdateCamera->Bind( "@BlackoutMaskPath", BlackoutMaskPath.empty() ? nullptr : BlackoutMaskPath.c_str() );
	UpdateCamera->Bind( "@FocusMaskPath", FocusMaskPath.empty() ? nullptr : FocusMaskPath.c_str() );
	UpdateCamera->Bind( "@ContinuousRecording", ContinuousRecording );

	if( UpdateCamera->Execute( nullptr ) < 0 )
	{
		crow::json::wvalue Data;
		Data["errorMessage"] = UpdateCamera->GetLastError();
		res.set_header( "Content-Type", "application/json" );
		res.body = Data.dump();
		res.code = 400;
		res.end();
		return;
	}

	m_GlobalContext->LongPoll->NotifyAll();

	res.set_header( "Content-Type", "application/json" );
	res.body = "{}";
	res.code = 200;
	res.end();
}