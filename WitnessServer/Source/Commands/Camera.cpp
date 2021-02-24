#include "Camera.h"
#include "Authenticate.h"
#include "../Messages.h"

#include "cpprest/json.h"
#include "cpprest/http_client.h"
#include "cpprest/uri.h"
#include "cpprest/asyncrt_utils.h"
#include "sodium.h"

#include <iostream>
#include <chrono>

using namespace web::json;
using namespace web::http::client;

void Command_Camera::OnMessage( GlobalContext& Context, http_request& Message, const string_t& CurrentCommand, vector<string_t>& ChildPath, bool IsPost )
{
	auto Packet = Message.extract_json().get();

	if( ChildPath.size() == 2 && IsPost )
	{
		auto Command = ChildPath.front();
		if( Command.compare( _T("record") ) == 0 )
		{
			OnRecordMessage( Context, Message, ChildPath[1], Packet );
		}
		else
		{
			Message.reply( status_codes::NotFound );
		}

		return;
	}
	if( ChildPath.size() == 1 && IsPost )
	{
		auto Command = ChildPath.front();
		if( Command.compare( _T("set_groups") ) == 0 )
		{
			OnSetGroupsMessage( Context, Message, Packet );
		}
		else if( Command.compare( _T("admin_reset_stats") ) == 0 )
		{
			OnResetStatsMessage( Context, Message, Packet );
		}
		else if (Command.compare(_T("admin_create")) == 0)
		{
			OnCreateMessage(Context, Message, Packet);
		}
		else
		{
			Message.reply( status_codes::NotFound );
		}

		return;
	}
	else if( ChildPath.size() == 2 && !IsPost )
	{
		auto Command = ChildPath.front();
		if( Command.compare( _T("previewLarge") ) == 0 )
		{
			OnPreviewMessage( Context, Message, ChildPath[1], Packet, true );
		}
		else if( Command.compare( _T("preview") ) == 0 )
		{
			OnPreviewMessage( Context, Message, ChildPath[1], Packet, false );
		}
		else
		{
			Message.reply( status_codes::NotFound );
		}

		return;
	}
	else if( ChildPath.size() == 1 && !IsPost )
	{
		auto Command = ChildPath.front();
		if( Command.compare( _T("enum_longpoll") ) == 0 )
		{
			OnEnumMessage( Context, Message, Packet, false, true );
		}
		else if (Command.compare(_T("enum")) == 0)
		{
			OnEnumMessage(Context, Message, Packet, false, false );
		}
		else if( Command.compare( _T("admin_enum") ) == 0 )
		{
			OnEnumMessage( Context, Message, Packet, true, false );
		}
		else
		{
			Message.reply( status_codes::NotFound );
		}

		return;
	}
	else
	{
		Message.reply( status_codes::NotFound );
	}
}

void Command_Camera::OnPreviewMessage( GlobalContext& Context, http_request& Message, const string_t& TargetCamera, const json::value& Packet, bool LargePreview )
{
	int TargetCameraInt = _wtoi( TargetCamera.c_str() );
	int UserUID = Command_Authenticate::IsCameraAuthenticated( Context, Message, Packet, Command_Authenticate::Action::Read, Command_Authenticate::Privilege::Normal, TargetCameraInt );
	if( UserUID < 0 )
	{
		return;
	}

	lock_guard<mutex> Lock( Context.Mutex );

	auto Iter = Context.Cameras.find( TargetCameraInt );
	if( Iter != Context.Cameras.end() )
	{
		if( LargePreview )
		{
			(*Iter).second.LastLargePreviewTimestamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
		}
		else
		{
			(*Iter).second.LastSmallPreviewTimestamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
		}

		auto PreviewRequest = make_shared<CameraPreviewRequestMessage>();
		PreviewRequest->LastLargePreviewTimestamp = (*Iter).second.LastLargePreviewTimestamp;
		PreviewRequest->LastSmallPreviewTimestamp = (*Iter).second.LastSmallPreviewTimestamp;
		Context.MessageBus->SendToClient( (*Iter).second.Worker.get(), PreviewRequest );

		http_response Response;
		Response.set_status_code( status_codes::OK );
		Response.set_body( (*Iter).second.PreviewThumbnail );
		Response.headers().set_content_type( _T("image/jpeg") );
		Response.headers().set_cache_control( _T("no-cache, no-store, must-revalidate") );

		Message.reply( Response );
	}
	else
	{
		Message.reply( status_codes::NotFound );
	}
}

void Command_Camera::OnEnumMessage( const GlobalContext& Context, http_request& Message, const json::value& Packet, bool AsAdmin, bool LongPoll )
{
	int UserUID = Command_Authenticate::IsAuthenticated( Context, Message, Packet, Command_Authenticate::Action::Read, AsAdmin ? Command_Authenticate::Privilege::Administrator : Command_Authenticate::Privilege::Normal );
	if( UserUID < 0 )
	{
		return;
	}

	bool First = true;
	vector<int> State;
	vector<int> OriginalState;
	vector<json::value> Array;

	bool IsAcceptable = false;
	do {
		Array.clear();
		State.clear();

		{
			SQLiteDatabaseQueryInstance GetCamerasForUser(Context.Database, AsAdmin ? _T("GetCameras") : _T("GetCamerasForUser"));
			GetCamerasForUser->Bind("@User", UserUID);

			GetCamerasForUser->Execute(
				[&Array, &Context, LongPoll, &State, AsAdmin](const SQLiteDatabaseQuery& query)
				{
					int ID = query.GetColumnValueInt(0);
					string_t Name = query.GetColumnValueText(1);
					string_t ConnectionString = query.GetColumnValueText(2);
					string_t ConnectionStringSub = query.GetColumnValueText(3);
					string_t Description = query.GetColumnValueText(4) ? query.GetColumnValueText(4) : _T("");
					int Enabled = query.GetColumnValueInt(5);

					if (Enabled || AsAdmin)
					{
						json::value Camera;
						Camera[_T("id")] = json::value(ID);
						Camera[_T("name")] = json::value(Name);
						Camera[_T("description")] = json::value(Description);
						Camera[_T("enabled")] = json::value(Enabled);

						vector<json::value> Groups;

						SQLiteDatabaseQueryInstance SelectGroupsForCamera(Context.Database, _T("SelectGroupsForCamera"));
						SelectGroupsForCamera->Bind("@Camera", ID);

						SelectGroupsForCamera->Execute(
							[&Groups](const SQLiteDatabaseQuery& query)
							{
								int GroupId = query.GetColumnValueInt(1);

								Groups.push_back(json::value(GroupId));

								return true;
							}
						);

						if (AsAdmin)
						{
							Camera[_T("connectionString")] = json::value(ConnectionString);
							Camera[_T("connectionStringSub")] = json::value(ConnectionStringSub);
						}

						Camera[_T("groups")] = json::value::array(Groups);

						{
							lock_guard<mutex> Lock(Context.Mutex);

							auto Iter = Context.Cameras.find(ID);
							if (Iter != Context.Cameras.end())
							{
								if (LongPoll)
								{
									State.push_back((*Iter).first);
									State.push_back((*Iter).second.IsRecording);
								}

								Camera[_T("status")] = json::value((*Iter).second.Status);
								Camera[_T("recording")] = json::value((*Iter).second.IsRecording);

								auto StreamStats = (*Iter).second.Worker->GetStreamStats();

								auto ImgStats = Context.CommonImageProcessingJobQueue->GetStats(ID);
								Camera[_T("lastTimestamp")] = json::value(ImgStats.LastTimestamp);

								if (AsAdmin && ImgStats.FrameCount > 0)
								{
									Camera[_T("frameCount")] = json::value(ImgStats.FrameCount);

#define GET_STAT(OutputPrefix, StatName) \
			Camera[ _T(OutputPrefix "TimeOfEachMS") ] = json::value( (double)ImgStats.Stats.FrameCount[StatName] ? ((double)ImgStats.Stats.Stats[StatName] / ((double)ImgStats.Stats.FrameCount[StatName] * 1000.0 * 1000.0)) : 0.0 );\
			Camera[ _T(OutputPrefix "ActualMS") ] = json::value( ImgStats.FrameCount ? (double)ImgStats.Stats.Stats[StatName] / ((double)ImgStats.FrameCount * 1000.0 * 1000.0) : 0 )

									GET_STAT("processing", FilterStat_Process_Total);
									GET_STAT("scale", FilterStat_Scale);
									GET_STAT("jpegEncoding", FilterStat_JpegEncoding);
									GET_STAT("observer", FilterStat_ObserverFilter);
									GET_STAT("firstPassFilter", FilterStat_FirstPassFilter);
									GET_STAT("secondPassFilter", FilterStat_SecondPassFilter);
									GET_STAT("thirdPassFilter", FilterStat_ThirdPassFilter);
									GET_STAT("debug", FilterStat_Debug);

									GET_STAT("mvfInternal", FilterStat_MVF_Internal);
									GET_STAT("mvfSideData", FilterStat_MVF_SideData);
									GET_STAT("mvfVectorPass", FilterStat_MVF_VectorPass);
									GET_STAT("mvfClusterPass", FilterStat_MVF_ClusterPass);
									GET_STAT("mvfObjectPass", FilterStat_MVF_ObjectPass);

#undef GET_STAT
								}

								if (AsAdmin && StreamStats.FrameCount > 0)
								{
									double Decode = (double)StreamStats.DecoderTimeTotal / ((double)StreamStats.FrameCount * 1000.0 * 1000.0);
									double Output = (double)StreamStats.OutputTimeTotal / ((double)StreamStats.FrameCount * 1000.0 * 1000.0);
									double Read = (double)StreamStats.ReadTimeTotal / ((double)StreamStats.FrameCount * 1000.0 * 1000.0);

									Camera[_T("streamReadTimeMS")] = json::value(Read);
									Camera[_T("streamDecodeTimeMS")] = json::value(Decode);
									Camera[_T("streamOutputTimeMS")] = json::value(Output);
								}
							}
						}

						Array.push_back(Camera);
					}

					return true;
				}
			);
		}

		if (LongPoll)
		{
			if (First)
			{
				OriginalState = State;
				First = false;
			}
			else
			{
				if (OriginalState.size() != State.size())
				{
					IsAcceptable = true;
				}
				else
				{
					int Same = true;
					for (int Index = 0; Index < OriginalState.size(); Index++)
					{
						if (OriginalState[Index] != State[Index])
						{
							Same = false;
							IsAcceptable = true;
							break;
						}
					}

					if (!IsAcceptable)
					{
						LongPollScope Scope(Context.LongPoll);
						Scope.Wait();
					}
				}
			}

			
		}
		else
		{
			IsAcceptable = true;
		}
	} while (!IsAcceptable);

	Message.reply( status_codes::OK, json::value::array(Array) );
}

void Command_Camera::OnCreateMessage(const GlobalContext& Context, http_request& Message, const json::value& Packet)
{
	int UserUID = Command_Authenticate::IsAuthenticated(Context, Message, Packet, Command_Authenticate::Action::ReadWrite, Command_Authenticate::Privilege::Administrator);
	if (UserUID < 0)
	{
		return;
	}

	bool Success = true;
	string_t Errors;

	string_t DisplayName;
	string_t Description;
	string_t ConnectionString;
	string_t ConnectionStringSub;

	Success &= GetJsonField(Packet, _T("displayName"), DisplayName, Errors);
	Success &= GetJsonField(Packet, _T("description"), Description, Errors);
	Success &= GetJsonField(Packet, _T("connectionString"), ConnectionString, Errors);
	Success &= GetJsonField(Packet, _T("connectionStringSub"), ConnectionStringSub, Errors);

	SQLiteDatabaseQueryInstance CreateCamera(Context.Database, _T("CreateCamera"));
	CreateCamera->Bind("@CameraName", DisplayName.c_str() );
	CreateCamera->Bind("@Description", Description.c_str() );
	CreateCamera->Bind("@CameraString", ConnectionString.c_str() );
	CreateCamera->Bind("@CameraStringSub", ConnectionStringSub.c_str());

	if (CreateCamera->Execute(
		[&](const SQLiteDatabaseQuery& query)
		{
			return true;
		}
	) < 0)
	{
		json::value Data;

		Data[_T("errorMessage")] = json::value(CreateCamera->GetLastError());

		Message.reply(status_codes::BadRequest, Data);

		return;
	}

	int64_t RowResult = CreateCamera->GetLastInsertionId();

	auto AddMessage = make_shared<CameraAddedMessage>((int)RowResult);

	Context.MessageBus->SendToClient(nullptr, AddMessage);

	Message.reply(status_codes::OK, json::value(_T("OK")));
}

void Command_Camera::OnRecordMessage( const GlobalContext& Context, http_request& Message, const string_t& TargetCamera, const json::value& Packet )
{
	int TargetCameraInt = _wtoi( TargetCamera.c_str() );
	int UserUID = Command_Authenticate::IsCameraAuthenticated( Context, Message, Packet, Command_Authenticate::Action::Read, Command_Authenticate::Privilege::Normal, TargetCameraInt );
	if( UserUID < 0 )
	{
		return;
	}

	bool Success = true;
	bool Record = false;
	string_t Errors;

	Success &= GetJsonField( Packet, _T("record"), Record, Errors );
	
	if (!Success)
	{
		Message.reply( status_codes::BadRequest, Errors );
		return;
	}

	bool ValidCamera = false;

	{
		lock_guard<mutex> Lock( Context.Mutex );

		auto Iter = Context.Cameras.find( TargetCameraInt );
		if( Iter != Context.Cameras.end() )
		{
			//Don't set recording value here, need to ensure it gets toggled correctly
			ValidCamera = true;
		}
	}

	auto ToggleRecord = make_shared<CameraStateToggleRecordMessage>( TargetCameraInt, Record );

	Context.MessageBus->SendToClient( nullptr, ToggleRecord );

	Message.reply( status_codes::OK, json::value(_T("OK")) );
}

void Command_Camera::OnResetStatsMessage( const GlobalContext& Context, http_request& Message, const json::value& Packet )
{
	int UserUID = Command_Authenticate::IsAuthenticated( Context, Message, Packet, Command_Authenticate::Action::ReadWrite, Command_Authenticate::Privilege::Administrator );
	if( UserUID < 0 )
	{
		return;
	}

	{
		lock_guard<mutex> Lock( Context.Mutex );

		for( auto Camera : Context.Cameras )
		{
			Context.CommonImageProcessingJobQueue->ResetStats( Camera.first );
		}
	}

	Message.reply( status_codes::OK, json::value(_T("OK")) );
}

void Command_Camera::OnSetGroupsMessage( const GlobalContext& Context, http_request& Message, const json::value& Packet )
{
	int UserUID = Command_Authenticate::IsAuthenticated( Context, Message, Packet, Command_Authenticate::Action::ReadWrite, Command_Authenticate::Privilege::Administrator );
	if( UserUID < 0 )
	{
		return;
	}

	string_t Errors;
	int CameraID;
	
	vector<int> CameraGroupsRequested;
	vector<int> CameraGroupsCurrent;

	vector<int> CameraGroupsToAdd;
	vector<int> CameraGroupsToRemove;

	bool Success = GetJsonField( Packet, _T("camera"), CameraID, Errors );

	if( !Packet.has_array_field(_T("value")) || !Success )
	{
		Errors += _T("value is not an array");
		Message.reply( status_codes::BadRequest, Errors );
		return;
	}

	auto& Array = Packet.at(_T("value")).as_array();
	for( auto& Element : Array )
	{
		if( !Element.is_integer() )
		{
			Errors += _T("Element in array is not an integer");
			Message.reply( status_codes::BadRequest, Errors );
			return;
		}

		CameraGroupsRequested.push_back( Element.as_integer() );
	}

	SQLiteDatabaseQueryInstance SelectGroupsForCamera( Context.Database, _T("SelectGroupsForCamera") );
	SelectGroupsForCamera->Bind( "@Camera", CameraID );
	
	int UserResult = SelectGroupsForCamera->Execute( 
		[&]( const SQLiteDatabaseQuery& query )
		{
			int Group = query.GetColumnValueInt( 1 );
			
			CameraGroupsCurrent.push_back(Group);

			return true;
		} 
	);

	if( UserResult < 0 )
	{
		json::value Data;
		Message.reply( status_codes::InternalError, SelectGroupsForCamera->GetLastError() );
		return;
	}

	for( int Value : CameraGroupsRequested )
	{
		if( find( CameraGroupsCurrent.begin(), CameraGroupsCurrent.end(), Value ) == CameraGroupsCurrent.end() )
		{
			CameraGroupsToAdd.push_back(Value);
		}
	}

	for( int Value : CameraGroupsCurrent )
	{
		if( find( CameraGroupsRequested.begin(), CameraGroupsRequested.end(), Value ) == CameraGroupsRequested.end() )
		{
			CameraGroupsToRemove.push_back(Value);
		}
	}

	for( int Value : CameraGroupsToAdd )
	{
		SQLiteDatabaseQueryInstance CreateCameraGroupMapping( Context.Database, _T("CreateCameraGroupMapping") );
		CreateCameraGroupMapping->Bind( "@Camera", CameraID );
		CreateCameraGroupMapping->Bind( "@Group", Value );

		int Result = CreateCameraGroupMapping->Execute( 
			[&]( const SQLiteDatabaseQuery& query )
			{
				return true;
			}
		);

		if( Result < 0 )
		{
			json::value Data;
			Message.reply( status_codes::InternalError, CreateCameraGroupMapping->GetLastError() );
			return;
		}
	}

	for( int Value : CameraGroupsToRemove )
	{
		SQLiteDatabaseQueryInstance DeleteCameraGroupMapping( Context.Database, _T("DeleteCameraGroupMapping") );
		DeleteCameraGroupMapping->Bind( "@Camera", CameraID );
		DeleteCameraGroupMapping->Bind( "@Group", Value );

		int Result = DeleteCameraGroupMapping->Execute( 
			[&]( const SQLiteDatabaseQuery& query )
			{
				return true;
			}
		);

		if( Result < 0 )
		{
			json::value Data;
			Message.reply( status_codes::InternalError, DeleteCameraGroupMapping->GetLastError() );
			return;
		}
	}

	json::value Data;
	Message.reply( status_codes::OK, Data );
}
