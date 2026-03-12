#include "CrowListener.h"
#include "CrowAuth.h"
#include "GlobalContext.h"
#include "FaceRecognitionCache.h"
#include "SQLite.h"
#include "FaceEmbeddingModel.h"

#include <Log.h>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/objdetect/face.hpp>

std::shared_ptr<Witness::Camera::FaceEmbeddingModel> CrowListener::EnsureFaceModel()
{
	auto embModel = m_GlobalContext->FaceEmbeddingModel;
	if( embModel && embModel->IsModelLoaded() )
		return embModel;

	// Try to load model on-demand (settings may have been enabled after startup)
	std::string modelPath;
	{
		SQLiteDatabaseQueryInstance sq( m_GlobalContext->Database, "GetSetting" );
		sq->Bind( "@Name", "face_recognition_model_path" );
		sq->Execute( [&]( const SQLiteDatabaseQuery& row ) {
			const char* val = row.GetColumnValueText( 0 );
			if( val ) modelPath = val;
			return true;
		});
	}
	if( modelPath.empty() )
	{
#ifdef _WIN32
		wchar_t buf[MAX_PATH] = {};
		GetModuleFileNameW( nullptr, buf, MAX_PATH );
		auto defaultPath = std::filesystem::path( buf ).parent_path() / "models" / "face_recognition.onnx";
#else
		auto defaultPath = std::filesystem::canonical( "/proc/self/exe" ).parent_path() / "models" / "face_recognition.onnx";
#endif
		if( std::filesystem::exists( defaultPath ) )
			modelPath = defaultPath.string();
	}

	if( !modelPath.empty() )
	{
		auto newModel = std::make_shared<Witness::Camera::FaceEmbeddingModel>();
		if( newModel->LoadModel( modelPath.c_str(), false, "" ) )
		{
			m_GlobalContext->FaceEmbeddingModel = newModel;
			if( !m_GlobalContext->FaceCache )
				m_GlobalContext->FaceCache = std::make_shared<FaceRecognitionCache>();
			LOG_INFO( "Face recognition model loaded on-demand: %s", modelPath.c_str() );
			return newModel;
		}
	}

	return nullptr;
}

void CrowListener::HandleKnownFaceList( const crow::request& req, crow::response& res )
{
	int UserUID = CrowAuth::IsAuthenticated( *m_GlobalContext, req, nullptr,
		CrowAuth::Action::Read, CrowAuth::Privilege::Normal );
	if( UserUID < 0 ) { res.code = 401; res.end(); return; }

	std::vector<crow::json::wvalue> faces;

	SQLiteDatabaseQueryInstance q( m_GlobalContext->Database, "SelectAllKnownFaces" );
	q->Execute( [&]( const SQLiteDatabaseQuery& row )
	{
		crow::json::wvalue face;
		face["id"] = row.GetColumnValueInt( 0 );
		face["name"] = std::string( row.GetColumnValueText( 1 ) ? row.GetColumnValueText( 1 ) : "" );
		face["notes"] = std::string( row.GetColumnValueText( 2 ) ? row.GetColumnValueText( 2 ) : "" );
		face["createdAt"] = row.GetColumnValueDouble( 3 );
		face["updatedAt"] = row.GetColumnValueDouble( 4 );
		face["verifiedCount"] = row.GetColumnValueInt( 5 );
		face["totalCount"] = row.GetColumnValueInt( 6 );
		const char* bestCrop = row.GetColumnValueText( 7 );
		face["bestCropPath"] = bestCrop ? std::string( bestCrop ) : "";
		face["bestCropUID"] = row.GetColumnValueInt( 8 );
		faces.push_back( std::move( face ) );
		return true;
	});

	crow::json::wvalue result;
	result["faces"] = std::move( faces );
	res.set_header( "Content-Type", "application/json" );
	res.body = result.dump();
	res.code = 200;
	res.end();
}

void CrowListener::HandleKnownFaceCreate( const crow::request& req, crow::response& res )
{
	auto body = crow::json::load( req.body );
	if( !body ) { res.code = 400; res.end(); return; }

	int UserUID = CrowAuth::IsAuthenticated( *m_GlobalContext, req, &body,
		CrowAuth::Action::ReadWrite, CrowAuth::Privilege::Administrator );
	if( UserUID < 0 ) { res.code = 401; res.end(); return; }

	std::string name = body.has( "name" ) ? std::string( body["name"].s() ) : "";
	std::string notes = body.has( "notes" ) ? std::string( body["notes"].s() ) : "";

	if( name.empty() ) { res.code = 400; res.body = R"({"error":"Name is required"})"; res.end(); return; }

	auto now = std::chrono::duration<double>( std::chrono::system_clock::now().time_since_epoch() ).count();

	SQLiteDatabaseQueryInstance q( m_GlobalContext->Database, "CreateKnownFace" );
	q->Bind( "@Name", name.c_str() );
	q->Bind( "@Notes", notes.c_str() );
	q->Bind( "@CreatedAt", now );
	q->Bind( "@UpdatedAt", now );
	q->Execute( nullptr );

	crow::json::wvalue result;
	result["id"] = q->GetLastInsertionId();
	result["name"] = name;
	res.set_header( "Content-Type", "application/json" );
	res.body = result.dump();
	res.code = 200;
	res.end();
}

void CrowListener::HandleKnownFaceUpdate( const crow::request& req, crow::response& res )
{
	auto body = crow::json::load( req.body );
	if( !body ) { res.code = 400; res.end(); return; }

	int UserUID = CrowAuth::IsAuthenticated( *m_GlobalContext, req, &body,
		CrowAuth::Action::ReadWrite, CrowAuth::Privilege::Administrator );
	if( UserUID < 0 ) { res.code = 401; res.end(); return; }

	int id = body.has( "id" ) ? (int)body["id"].i() : 0;
	std::string name = body.has( "name" ) ? std::string( body["name"].s() ) : "";
	std::string notes = body.has( "notes" ) ? std::string( body["notes"].s() ) : "";

	if( id <= 0 || name.empty() ) { res.code = 400; res.end(); return; }

	auto now = std::chrono::duration<double>( std::chrono::system_clock::now().time_since_epoch() ).count();

	SQLiteDatabaseQueryInstance q( m_GlobalContext->Database, "UpdateKnownFace" );
	q->Bind( "@KnownFaceUID", id );
	q->Bind( "@Name", name.c_str() );
	q->Bind( "@Notes", notes.c_str() );
	q->Bind( "@UpdatedAt", now );
	q->Execute( nullptr );

	// Refresh cache so matching uses new name
	if( m_GlobalContext->FaceCache )
		m_GlobalContext->FaceCache->Refresh( m_GlobalContext->Database );

	res.code = 200;
	res.body = R"({"ok":true})";
	res.set_header( "Content-Type", "application/json" );
	res.end();
}

void CrowListener::HandleKnownFaceDelete( const crow::request& req, crow::response& res )
{
	auto body = crow::json::load( req.body );
	if( !body ) { res.code = 400; res.end(); return; }

	int UserUID = CrowAuth::IsAuthenticated( *m_GlobalContext, req, &body,
		CrowAuth::Action::ReadWrite, CrowAuth::Privilege::Administrator );
	if( UserUID < 0 ) { res.code = 401; res.end(); return; }

	int id = body.has( "id" ) ? (int)body["id"].i() : 0;
	if( id <= 0 ) { res.code = 400; res.end(); return; }

	// Embeddings will have KnownFaceUID set to NULL by ON DELETE SET NULL
	SQLiteDatabaseQueryInstance q( m_GlobalContext->Database, "DeleteKnownFace" );
	q->Bind( "@KnownFaceUID", id );
	q->Execute( nullptr );

	if( m_GlobalContext->FaceCache )
		m_GlobalContext->FaceCache->Refresh( m_GlobalContext->Database );

	res.code = 200;
	res.body = R"({"ok":true})";
	res.set_header( "Content-Type", "application/json" );
	res.end();
}

void CrowListener::HandleFaceAssign( const crow::request& req, crow::response& res )
{
	auto body = crow::json::load( req.body );
	if( !body ) { res.code = 400; res.end(); return; }

	int UserUID = CrowAuth::IsAuthenticated( *m_GlobalContext, req, &body,
		CrowAuth::Action::ReadWrite, CrowAuth::Privilege::Administrator );
	if( UserUID < 0 ) { res.code = 401; res.end(); return; }

	int cropUID = body.has( "cropUID" ) ? (int)body["cropUID"].i() : 0;
	int knownFaceUID = body.has( "knownFaceUID" ) ? (int)body["knownFaceUID"].i() : 0;

	if( cropUID <= 0 || knownFaceUID <= 0 ) { res.code = 400; res.end(); return; }

	// Check if embedding already exists for this crop
	bool embeddingExists = false;
	int existingEmbUID = 0;

	{
		SQLiteDatabaseQueryInstance q( m_GlobalContext->Database, "SelectEmbeddingByFaceCrop" );
		q->Bind( "@FaceCropUID", cropUID );
		q->Execute( [&]( const SQLiteDatabaseQuery& row )
		{
			embeddingExists = true;
			existingEmbUID = row.GetColumnValueInt( 0 );
			return false;
		});
	}

	if( embeddingExists )
	{
		// Update existing embedding's identity
		SQLiteDatabaseQueryInstance q( m_GlobalContext->Database, "UpdateFaceEmbeddingIdentity" );
		q->Bind( "@EmbeddingUID", existingEmbUID );
		q->Bind( "@KnownFaceUID", knownFaceUID );
		q->Bind( "@MatchConfidence", 1.0 );
		q->Bind( "@Verified", 1 );
		q->Execute( nullptr );
	}
	else
	{
		// Need to generate embedding first — load crop, align, embed
		std::string filePath;
		float lmX[5] = {}, lmY[5] = {};

		{
			SQLiteDatabaseQueryInstance q( m_GlobalContext->Database, "SelectFaceCropByUID" );
			q->Bind( "@CropUID", cropUID );
			q->Execute( [&]( const SQLiteDatabaseQuery& row )
			{
				const char* path = row.GetColumnValueText( 5 );
				if( path ) filePath = path;
				for( int i = 0; i < 5; i++ )
				{
					lmX[i] = (float)row.GetColumnValueDouble( 7 + i * 2 );
					lmY[i] = (float)row.GetColumnValueDouble( 8 + i * 2 );
				}
				return false;
			});
		}

		if( filePath.empty() || !std::filesystem::exists( filePath ) )
		{
			res.code = 404;
			res.body = R"({"error":"Face crop file not found"})";
			res.set_header( "Content-Type", "application/json" );
			res.end();
			return;
		}

		auto embModel = m_GlobalContext->FaceEmbeddingModel;
		if( !embModel || !embModel->IsModelLoaded() )
		{
			// No model — just insert a placeholder without embedding
			res.code = 400;
			res.body = R"({"error":"Face recognition model not loaded"})";
			res.set_header( "Content-Type", "application/json" );
			res.end();
			return;
		}

		cv::Mat crop = cv::imread( filePath );
		if( crop.empty() )
		{
			res.code = 500;
			res.body = R"({"error":"Failed to read crop image"})";
			res.set_header( "Content-Type", "application/json" );
			res.end();
			return;
		}

		// Align if landmarks available
		bool hasLandmarks = false;
		for( int i = 0; i < 5; i++ )
			if( lmX[i] != 0.0f || lmY[i] != 0.0f ) { hasLandmarks = true; break; }

		cv::Mat aligned;
		if( hasLandmarks )
		{
			// Landmarks are stored as normalized (0-1) relative to full frame,
			// but the crop is already 112x112 — need to convert to crop-relative pixels
			// The crop coordinates aren't stored directly, so use landmarks as-is scaled to 112
			float cropLmX[5], cropLmY[5];
			for( int i = 0; i < 5; i++ )
			{
				cropLmX[i] = lmX[i] * 112.0f;
				cropLmY[i] = lmY[i] * 112.0f;
			}
			aligned = Witness::Camera::FaceEmbeddingModel::AlignFace( crop, cropLmX, cropLmY );
		}
		else
		{
			aligned = crop;
		}

		auto embedding = embModel->GetEmbedding( aligned );
		if( embedding.empty() )
		{
			res.code = 500;
			res.body = R"({"error":"Failed to generate embedding"})";
			res.set_header( "Content-Type", "application/json" );
			res.end();
			return;
		}

		auto now = std::chrono::duration<double>( std::chrono::system_clock::now().time_since_epoch() ).count();

		SQLiteDatabaseQueryInstance q( m_GlobalContext->Database, "InsertFaceEmbedding" );
		q->Bind( "@FaceCropUID", cropUID );
		q->Bind( "@KnownFaceUID", knownFaceUID );
		q->BindBlob( "@Embedding", embedding.data(), static_cast<int>( embedding.size() * sizeof( float ) ) );
		q->Bind( "@Dimension", static_cast<int>( embedding.size() ) );
		q->Bind( "@MatchConfidence", 1.0 );
		q->Bind( "@Verified", 1 );
		q->Bind( "@CreatedAt", now );
		q->Execute( nullptr );
	}

	// Refresh cache with new verified embedding
	if( m_GlobalContext->FaceCache )
		m_GlobalContext->FaceCache->Refresh( m_GlobalContext->Database );

	res.code = 200;
	res.body = R"({"ok":true})";
	res.set_header( "Content-Type", "application/json" );
	res.end();
}

void CrowListener::HandleFaceUnassign( const crow::request& req, crow::response& res )
{
	auto body = crow::json::load( req.body );
	if( !body ) { res.code = 400; res.end(); return; }

	int UserUID = CrowAuth::IsAuthenticated( *m_GlobalContext, req, &body,
		CrowAuth::Action::ReadWrite, CrowAuth::Privilege::Administrator );
	if( UserUID < 0 ) { res.code = 401; res.end(); return; }

	int embeddingUID = body.has( "embeddingUID" ) ? (int)body["embeddingUID"].i() : 0;
	if( embeddingUID <= 0 ) { res.code = 400; res.end(); return; }

	// Set KnownFaceUID to NULL and unverify
	SQLiteDatabaseQueryInstance q( m_GlobalContext->Database, "UpdateFaceEmbeddingIdentity" );
	q->Bind( "@EmbeddingUID", embeddingUID );
	q->BindNull( "@KnownFaceUID" );
	q->Bind( "@MatchConfidence", 0.0 );
	q->Bind( "@Verified", 0 );
	q->Execute( nullptr );

	if( m_GlobalContext->FaceCache )
		m_GlobalContext->FaceCache->Refresh( m_GlobalContext->Database );

	res.code = 200;
	res.body = R"({"ok":true})";
	res.set_header( "Content-Type", "application/json" );
	res.end();
}

void CrowListener::HandleFaceMerge( const crow::request& req, crow::response& res )
{
	auto body = crow::json::load( req.body );
	if( !body ) { res.code = 400; res.end(); return; }

	int UserUID = CrowAuth::IsAuthenticated( *m_GlobalContext, req, &body,
		CrowAuth::Action::ReadWrite, CrowAuth::Privilege::Administrator );
	if( UserUID < 0 ) { res.code = 401; res.end(); return; }

	int sourceUID = body.has( "sourceId" ) ? (int)body["sourceId"].i() : 0;
	int targetUID = body.has( "targetId" ) ? (int)body["targetId"].i() : 0;

	if( sourceUID <= 0 || targetUID <= 0 || sourceUID == targetUID )
	{
		res.code = 400;
		res.end();
		return;
	}

	// Move all embeddings from source to target
	{
		SQLiteDatabaseQueryInstance q( m_GlobalContext->Database, "MergeKnownFace" );
		q->Bind( "@SourceUID", sourceUID );
		q->Bind( "@TargetUID", targetUID );
		q->Execute( nullptr );
	}

	// Delete the source known face
	{
		SQLiteDatabaseQueryInstance q( m_GlobalContext->Database, "DeleteKnownFace" );
		q->Bind( "@KnownFaceUID", sourceUID );
		q->Execute( nullptr );
	}

	if( m_GlobalContext->FaceCache )
		m_GlobalContext->FaceCache->Refresh( m_GlobalContext->Database );

	res.code = 200;
	res.body = R"({"ok":true})";
	res.set_header( "Content-Type", "application/json" );
	res.end();
}

void CrowListener::HandleUnidentifiedFaces( const crow::request& req, crow::response& res )
{
	int UserUID = CrowAuth::IsAuthenticated( *m_GlobalContext, req, nullptr,
		CrowAuth::Action::Read, CrowAuth::Privilege::Normal );
	if( UserUID < 0 ) { res.code = 401; res.end(); return; }

	int limit = 50;
	int offset = 0;
	auto limitParam = req.url_params.get( "limit" );
	auto offsetParam = req.url_params.get( "offset" );
	if( limitParam ) limit = std::min( std::max( std::stoi( limitParam ), 1 ), 200 );
	if( offsetParam ) offset = std::max( std::stoi( offsetParam ), 0 );

	std::vector<crow::json::wvalue> faces;

	SQLiteDatabaseQueryInstance q( m_GlobalContext->Database, "SelectUnidentifiedFaces" );
	q->Bind( "@Limit", limit );
	q->Bind( "@Offset", offset );

	q->Execute( [&]( const SQLiteDatabaseQuery& row )
	{
		crow::json::wvalue face;
		face["cropUID"] = row.GetColumnValueInt( 0 );
		face["cameraId"] = row.GetColumnValueInt( 1 );
		face["timestamp"] = row.GetColumnValueDouble( 2 );

		const char* path = row.GetColumnValueText( 3 );
		face["filePath"] = path ? std::string( path ) : "";
		face["detectionConfidence"] = row.GetColumnValueDouble( 4 );

		int embUID = row.GetColumnValueInt( 5 );
		face["embeddingUID"] = embUID;
		face["matchConfidence"] = row.GetColumnValueDouble( 6 );

		// Landmarks (normalized 0-1)
		std::vector<crow::json::wvalue> landmarks( 10 );
		for( int i = 0; i < 5; i++ )
		{
			landmarks[i * 2] = row.GetColumnValueDouble( 7 + i * 2 );
			landmarks[i * 2 + 1] = row.GetColumnValueDouble( 8 + i * 2 );
		}
		face["landmarks"] = std::move( landmarks );

		faces.push_back( std::move( face ) );
		return true;
	});

	crow::json::wvalue result;
	result["faces"] = std::move( faces );
	res.set_header( "Content-Type", "application/json" );
	res.body = result.dump();
	res.code = 200;
	res.end();
}

void CrowListener::HandleFaceSightings( const crow::request& req, crow::response& res, int knownFaceId )
{
	int UserUID = CrowAuth::IsAuthenticated( *m_GlobalContext, req, nullptr,
		CrowAuth::Action::Read, CrowAuth::Privilege::Normal );
	if( UserUID < 0 ) { res.code = 401; res.end(); return; }

	int limit = 50;
	int offset = 0;
	auto limitParam = req.url_params.get( "limit" );
	auto offsetParam = req.url_params.get( "offset" );
	if( limitParam ) limit = std::min( std::max( std::stoi( limitParam ), 1 ), 200 );
	if( offsetParam ) offset = std::max( std::stoi( offsetParam ), 0 );

	std::vector<crow::json::wvalue> sightings;

	SQLiteDatabaseQueryInstance q( m_GlobalContext->Database, "SelectFaceSightings" );
	q->Bind( "@KnownFaceUID", knownFaceId );
	q->Bind( "@Limit", limit );
	q->Bind( "@Offset", offset );

	q->Execute( [&]( const SQLiteDatabaseQuery& row )
	{
		crow::json::wvalue s;
		s["cropUID"] = row.GetColumnValueInt( 0 );
		s["cameraId"] = row.GetColumnValueInt( 1 );
		s["timestamp"] = row.GetColumnValueDouble( 2 );
		const char* path = row.GetColumnValueText( 3 );
		s["filePath"] = path ? std::string( path ) : "";
		s["detectionConfidence"] = row.GetColumnValueDouble( 4 );
		s["matchConfidence"] = row.GetColumnValueDouble( 5 );
		s["verified"] = row.GetColumnValueInt( 6 );
		s["embeddingUID"] = row.GetColumnValueInt( 7 );
		sightings.push_back( std::move( s ) );
		return true;
	});

	crow::json::wvalue result;
	result["sightings"] = std::move( sightings );
	result["knownFaceId"] = knownFaceId;
	res.set_header( "Content-Type", "application/json" );
	res.body = result.dump();
	res.code = 200;
	res.end();
}

void CrowListener::HandleFaceReprocess( const crow::request& req, crow::response& res )
{
	auto body = crow::json::load( req.body );
	int UserUID = CrowAuth::IsAuthenticated( *m_GlobalContext, req, body ? &body : nullptr,
		CrowAuth::Action::ReadWrite, CrowAuth::Privilege::Administrator );
	if( UserUID < 0 ) { res.code = 401; res.end(); return; }

	auto embModel = m_GlobalContext->FaceEmbeddingModel;
	if( !embModel || !embModel->IsModelLoaded() )
	{
		// Try to load model on-demand (settings may have been enabled after startup)
		std::string modelPath;
		{
			SQLiteDatabaseQueryInstance sq( m_GlobalContext->Database, "GetSetting" );
			sq->Bind( "@Name", "face_recognition_model_path" );
			sq->Execute( [&]( const SQLiteDatabaseQuery& row ) {
				const char* val = row.GetColumnValueText( 0 );
				if( val ) modelPath = val;
				return true;
			});
		}
		if( modelPath.empty() )
		{
#ifdef _WIN32
			wchar_t buf[MAX_PATH] = {};
			GetModuleFileNameW( nullptr, buf, MAX_PATH );
			auto defaultPath = std::filesystem::path( buf ).parent_path() / "models" / "face_recognition.onnx";
#else
			auto defaultPath = std::filesystem::canonical( "/proc/self/exe" ).parent_path() / "models" / "face_recognition.onnx";
#endif
			if( std::filesystem::exists( defaultPath ) )
				modelPath = defaultPath.string();
		}

		if( !modelPath.empty() )
		{
			auto newModel = std::make_shared<Witness::Camera::FaceEmbeddingModel>();
			if( newModel->LoadModel( modelPath.c_str(), false, "" ) )
			{
				m_GlobalContext->FaceEmbeddingModel = newModel;
				if( !m_GlobalContext->FaceCache )
					m_GlobalContext->FaceCache = std::make_shared<FaceRecognitionCache>();
				embModel = newModel;
				LOG_INFO( "Face recognition model loaded on-demand: %s", modelPath.c_str() );
			}
		}

		if( !embModel || !embModel->IsModelLoaded() )
		{
			res.code = 400;
			res.body = R"({"error":"Face recognition model not found. Place face_recognition.onnx in models/ folder."})";
			res.set_header( "Content-Type", "application/json" );
			res.end();
			return;
		}
	}

	// Process crops without embeddings in batches
	int processed = 0;
	int batchLimit = 100;

	SQLiteDatabaseQueryInstance q( m_GlobalContext->Database, "SelectFaceCropsWithoutEmbedding" );
	q->Bind( "@Limit", batchLimit );

	struct CropInfo
	{
		int CropUID;
		std::string FilePath;
		float LmX[5], LmY[5];
		int CameraID;
		double Timestamp;
	};

	std::vector<CropInfo> crops;
	q->Execute( [&]( const SQLiteDatabaseQuery& row )
	{
		CropInfo c;
		c.CropUID = row.GetColumnValueInt( 0 );
		const char* path = row.GetColumnValueText( 1 );
		c.FilePath = path ? path : "";
		for( int i = 0; i < 5; i++ )
		{
			c.LmX[i] = (float)row.GetColumnValueDouble( 3 + i * 2 );
			c.LmY[i] = (float)row.GetColumnValueDouble( 4 + i * 2 );
		}
		c.CameraID = row.GetColumnValueInt( 13 );
		c.Timestamp = row.GetColumnValueDouble( 14 );
		crops.push_back( std::move( c ) );
		return true;
	});

	for( auto& c : crops )
	{
		if( c.FilePath.empty() || !std::filesystem::exists( c.FilePath ) )
			continue;

		cv::Mat crop = cv::imread( c.FilePath );
		if( crop.empty() ) continue;

		bool hasLandmarks = false;
		for( int i = 0; i < 5; i++ )
			if( c.LmX[i] != 0.0f || c.LmY[i] != 0.0f ) { hasLandmarks = true; break; }

		cv::Mat aligned;
		if( hasLandmarks )
		{
			float cropLmX[5], cropLmY[5];
			for( int i = 0; i < 5; i++ )
			{
				cropLmX[i] = c.LmX[i] * 112.0f;
				cropLmY[i] = c.LmY[i] * 112.0f;
			}
			aligned = Witness::Camera::FaceEmbeddingModel::AlignFace( crop, cropLmX, cropLmY );
		}
		else
		{
			aligned = crop;
		}

		auto embedding = embModel->GetEmbedding( aligned );
		if( embedding.empty() ) continue;

		SQLiteDatabaseQueryInstance embQ( m_GlobalContext->Database, "InsertFaceEmbedding" );
		embQ->Bind( "@FaceCropUID", c.CropUID );
		embQ->BindNull( "@KnownFaceUID" );
		embQ->BindBlob( "@Embedding", embedding.data(), static_cast<int>( embedding.size() * sizeof( float ) ) );
		embQ->Bind( "@Dimension", static_cast<int>( embedding.size() ) );
		embQ->Bind( "@MatchConfidence", 0.0 );
		embQ->Bind( "@Verified", 0 );
		embQ->Bind( "@CreatedAt", c.Timestamp );
		embQ->Execute( nullptr );

		processed++;
	}

	LOG_INFO( "FaceReprocess: Generated embeddings for %d crops.", processed );

	crow::json::wvalue result;
	result["processed"] = processed;
	result["remaining"] = (int)crops.size() == batchLimit;// true if there may be more
	res.set_header( "Content-Type", "application/json" );
	res.body = result.dump();
	res.code = 200;
	res.end();
}

// --------------------------------------------------------------------------
// Upload a face photo, detect the face, crop, generate embedding
// --------------------------------------------------------------------------
void CrowListener::HandleFaceUpload( const crow::request& req, crow::response& res )
{
	auto body = crow::json::load( req.body );
	if( !body ) { res.code = 400; res.end(); return; }

	int UserUID = CrowAuth::IsAuthenticated( *m_GlobalContext, req, &body,
		CrowAuth::Action::ReadWrite, CrowAuth::Privilege::Administrator );
	if( UserUID < 0 ) { res.code = 401; res.end(); return; }

	// Parse request: base64-encoded image data + optional knownFaceUID
	if( !body.has( "image" ) )
	{
		res.code = 400;
		res.body = R"({"error":"Missing 'image' field [base64-encoded]"})";		res.set_header( "Content-Type", "application/json" );
		res.end();
		return;
	}

	std::string imageB64 = std::string( body["image"].s() );
	int knownFaceUID = body.has( "knownFaceUID" ) ? (int)body["knownFaceUID"].i() : 0;

	// Strip data URL prefix if present (e.g. "data:image/jpeg;base64,...")
	auto commaPos = imageB64.find( ',' );
	if( commaPos != std::string::npos )
		imageB64 = imageB64.substr( commaPos + 1 );

	// Decode base64
	auto decoded = crow::utility::base64decode( imageB64 );
	if( decoded.empty() )
	{
		res.code = 400;
		res.body = R"({"error":"Invalid base64 image data"})";
		res.set_header( "Content-Type", "application/json" );
		res.end();
		return;
	}

	// Decode image
	std::vector<uchar> imgBuf( decoded.begin(), decoded.end() );
	cv::Mat image = cv::imdecode( imgBuf, cv::IMREAD_COLOR );
	if( image.empty() )
	{
		res.code = 400;
		res.body = R"({"error":"Failed to decode image"})";
		res.set_header( "Content-Type", "application/json" );
		res.end();
		return;
	}

	// Ensure face recognition model is loaded
	auto embModel = EnsureFaceModel();
	if( !embModel )
	{
		res.code = 400;
		res.body = R"({"error":"Face recognition model not loaded"})";
		res.set_header( "Content-Type", "application/json" );
		res.end();
		return;
	}

	// Resolve face detection model path
	std::string faceDetectModelPath;
	{
		SQLiteDatabaseQueryInstance sq( m_GlobalContext->Database, "GetSetting" );
		sq->Bind( "@Name", "face_detection_model_path" );
		sq->Execute( [&]( const SQLiteDatabaseQuery& row ) {
			const char* val = row.GetColumnValueText( 0 );
			if( val ) faceDetectModelPath = val;
			return true;
		});
	}
	if( faceDetectModelPath.empty() )
	{
#ifdef _WIN32
		wchar_t buf[MAX_PATH] = {};
		GetModuleFileNameW( nullptr, buf, MAX_PATH );
		auto defaultPath = std::filesystem::path( buf ).parent_path() / "models" / "face_detection_yunet_2023mar.onnx";
#else
		auto defaultPath = std::filesystem::canonical( "/proc/self/exe" ).parent_path() / "models" / "face_detection_yunet_2023mar.onnx";
#endif
		if( std::filesystem::exists( defaultPath ) )
			faceDetectModelPath = defaultPath.string();
	}

	if( faceDetectModelPath.empty() || !std::filesystem::exists( faceDetectModelPath ) )
	{
		res.code = 400;
		res.body = R"({"error":"Face detection model not found"})";
		res.set_header( "Content-Type", "application/json" );
		res.end();
		return;
	}

	// Create face detector and detect faces
	auto detector = cv::FaceDetectorYN::create( faceDetectModelPath, "", image.size(), 0.7f, 0.3f, 5000 );
	cv::Mat faces;
	detector->detect( image, faces );

	if( faces.empty() || faces.rows == 0 )
	{
		res.code = 400;
		res.body = R"({"error":"No face detected in image. Please upload a clear photo with a visible face."})";
		res.set_header( "Content-Type", "application/json" );
		res.end();
		return;
	}

	auto facesDir = std::filesystem::path( m_GlobalContext->CachePath ) / "faces" / "uploads";
	std::filesystem::create_directories( facesDir );

	auto now = std::chrono::duration<double>( std::chrono::system_clock::now().time_since_epoch() ).count();
	std::vector<int> jpegParams = { cv::IMWRITE_JPEG_QUALITY, 95 };

	// Process ALL detected faces
	std::vector<crow::json::wvalue> results;
	int accepted = 0, rejected = 0;

	for( int fi = 0; fi < faces.rows; fi++ )
	{
		float score = faces.at<float>( fi, 14 );

		float bx = faces.at<float>( fi, 0 );
		float by = faces.at<float>( fi, 1 );
		float bw = faces.at<float>( fi, 2 );
		float bh = faces.at<float>( fi, 3 );

		float lmX[5], lmY[5];
		for( int i = 0; i < 5; i++ )
		{
			lmX[i] = faces.at<float>( fi, 4 + i * 2 );
			lmY[i] = faces.at<float>( fi, 5 + i * 2 );
		}

		int ix = std::max( 0, (int)bx );
		int iy = std::max( 0, (int)by );
		int iw = std::min( (int)bw, image.cols - ix );
		int ih = std::min( (int)bh, image.rows - iy );
		if( iw <= 0 || ih <= 0 ) { rejected++; continue; }

		cv::Rect faceRect( ix, iy, iw, ih );
		cv::Mat faceCrop;
		cv::resize( image( faceRect ), faceCrop, cv::Size( 112, 112 ) );

		// Normalize landmarks relative to the crop
		float normLmX[5], normLmY[5];
		for( int i = 0; i < 5; i++ )
		{
			normLmX[i] = ( lmX[i] - (float)ix ) / (float)iw;
			normLmY[i] = ( lmY[i] - (float)iy ) / (float)ih;
		}

		// Orientation validation
		{
			float cropPx[5], cropPy[5];
			for( int i = 0; i < 5; i++ )
			{
				cropPx[i] = normLmX[i] * 112.0f;
				cropPy[i] = normLmY[i] * 112.0f;
			}
			float eyeAvgY = ( cropPy[0] + cropPy[1] ) * 0.5f;
			float mouthAvgY = ( cropPy[3] + cropPy[4] ) * 0.5f;
			float interEyeDist = std::abs( cropPx[1] - cropPx[0] );
			float eyeMidX = ( cropPx[0] + cropPx[1] ) * 0.5f;
			float noseOffsetX = std::abs( cropPx[2] - eyeMidX );
			float eyeHeightDiff = std::abs( cropPy[0] - cropPy[1] );

			bool verticalOk = eyeAvgY < cropPy[2] && cropPy[2] < mouthAvgY;
			if( !verticalOk || interEyeDist < 15.0f || noseOffsetX > 25.0f
				|| eyeHeightDiff > 20.0f )
			{
				rejected++;
				continue;
			}
		}

		// Save crop to disk
		std::ostringstream filenameStr;
		filenameStr << std::fixed << std::setprecision( 3 ) << now << "_upload_" << fi << ".jpg";
		auto facePath = facesDir / filenameStr.str();
		cv::imwrite( facePath.string(), faceCrop, jpegParams );

		// Insert FaceCrop into database
		int64_t cropUID = 0;
		{
			SQLiteDatabaseQueryInstance q( m_GlobalContext->Database, "InsertFaceCrop" );
			q->Bind( "@CameraID", 0 );
			q->Bind( "@Timestamp", now );
			q->Bind( "@FrameUID", 0 );
			q->Bind( "@TrackingID", 0 );
			q->Bind( "@FilePath", facePath.string().c_str() );
			q->Bind( "@Confidence", (double)score );
			for( int i = 0; i < 5; i++ )
			{
				char nameX[32], nameY[32];
				snprintf( nameX, sizeof( nameX ), "@Landmark%dX", i );
				snprintf( nameY, sizeof( nameY ), "@Landmark%dY", i );
				q->Bind( nameX, (double)normLmX[i] );
				q->Bind( nameY, (double)normLmY[i] );
			}
			q->Execute( nullptr );
			cropUID = q->GetLastInsertionId();
		}

		// Align face and generate embedding
		float cropLmX[5], cropLmY[5];
		for( int i = 0; i < 5; i++ )
		{
			cropLmX[i] = normLmX[i] * 112.0f;
			cropLmY[i] = normLmY[i] * 112.0f;
		}
		cv::Mat aligned = Witness::Camera::FaceEmbeddingModel::AlignFace( faceCrop, cropLmX, cropLmY );

		auto embedding = embModel->GetEmbedding( aligned );
		if( embedding.empty() ) { rejected++; continue; }

		// Store embedding
		{
			SQLiteDatabaseQueryInstance q( m_GlobalContext->Database, "InsertFaceEmbedding" );
			q->Bind( "@FaceCropUID", (int)cropUID );
			if( knownFaceUID > 0 )
				q->Bind( "@KnownFaceUID", knownFaceUID );
			else
				q->BindNull( "@KnownFaceUID" );
			q->BindBlob( "@Embedding", embedding.data(), static_cast<int>( embedding.size() * sizeof( float ) ) );
			q->Bind( "@Dimension", static_cast<int>( embedding.size() ) );
			q->Bind( "@MatchConfidence", knownFaceUID > 0 ? 1.0 : 0.0 );
			q->Bind( "@Verified", knownFaceUID > 0 ? 1 : 0 );
			q->Bind( "@CreatedAt", now );
			q->Execute( nullptr );
		}

		crow::json::wvalue faceResult;
		faceResult["cropUID"] = (int)cropUID;
		faceResult["confidence"] = score;
		results.push_back( std::move( faceResult ) );
		accepted++;
	}

	// Refresh cache if assigned to a known face
	if( knownFaceUID > 0 && m_GlobalContext->FaceCache && accepted > 0 )
		m_GlobalContext->FaceCache->Refresh( m_GlobalContext->Database );

	if( accepted == 0 )
	{
		std::string msg = faces.rows == 1
			? "Face orientation not suitable for recognition — use a front-facing photo"
			: "No faces passed orientation check (" + std::to_string( faces.rows ) + " detected, all rejected)";
		crow::json::wvalue err;
		err["error"] = msg;
		res.code = 400;
		res.body = err.dump();
		res.set_header( "Content-Type", "application/json" );
		res.end();
		return;
	}

	LOG_INFO( "FaceUpload: %d faces accepted, %d rejected from image with %d detections%s",
		accepted, rejected, faces.rows,
		knownFaceUID > 0 ? (" assigned to known face " + std::to_string( knownFaceUID )).c_str() : "" );

	crow::json::wvalue result;
	result["ok"] = true;
	result["accepted"] = accepted;
	result["rejected"] = rejected;
	result["facesDetected"] = faces.rows;
	result["faces"] = std::move( results );
	res.code = 200;
	res.body = result.dump();
	res.set_header( "Content-Type", "application/json" );
	res.end();
}
