#include "CameraWorker.h"
#include "GlobalContext.h"
#include "MotionVectorFilter.h"
#include "ObservingMotionFilter.h"
#include "PersonRecognitionFilter.h"
#include "ONNXDetectionFilter.h"
#include "FaceDetectionFilter.h"
#include "FaceEmbeddingModel.h"
#include "FaceRecognitionCache.h"
#include "ObjectTracker.h"
#include "SQLite.h"
#include "EventBroadcaster.h"
#include "StreamBroadcaster.h"
#include "crow/json.h"

#include <Log.h>
#include <chrono>
#include <thread>
#include <filesystem>
#include <sstream>
#include <iomanip>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <set>
#include <unordered_map>
#include "SoundManager.h"

std::string CameraWorker::GetVideoCodecName() const
{
	std::shared_ptr<InputStream> Stream = CameraStream;
	if( Stream )
	{
		return Stream->GetCodecName();
	}
	return "";
}

void CameraWorker::CreateInputStream()
{
	InputStreamSetup Setup;
	Setup.GetTimestamp = GetUnixTimestamp;
	Setup.MotionFilterFrameSkip = Camera.SkipFrames;
	Setup.MotionDetectFrameHeight = Camera.MDFrameHeight;
	Setup.MotionDetectThreshold = Camera.MDThreshold;
	Setup.HistoricalPacketBufferSeconds = Video.ClipHistoryPeriod;
	Setup.ExportMotionVectors = Video.ExportMotionVectors != 0;

	std::string CamPath = Camera.Path;

	std::string CachePath = std::string(Context->CachePath.begin(), Context->CachePath.end());

	CameraStream = std::make_shared<InputStream>( Setup, Camera.ID, Camera.JobQueue, CamPath );

	if (LiveStream)
	{
		LiveStream->ResetForReconnect(CameraStream.get());
	}
	else
	{
		LiveStream = std::make_shared<LiveOutputStream>(CachePath, CameraStream.get(), 1);
		LiveStream->SetPartialTargetDuration(Video.MsePartialDuration);

		// Wire up MSE WebSocket notifications
		int cameraId = Camera.ID;
		auto streams = Context->Streams;
		LiveStream->SetEventCallback([cameraId, streams](const Witness::Camera::LiveStreamEvent& ev)
		{
			if (!streams->HasViewers(cameraId))
				return;

			crow::json::wvalue ctrl;
			switch (ev.EventType)
			{
			case Witness::Camera::LiveStreamEvent::InitSegmentReady:
				ctrl["type"] = "initSegment";
				ctrl["generation"] = ev.Generation;
				streams->SendControl(cameraId, ctrl.dump());
				streams->SendBinary(cameraId, ev.Data);
				break;

			case Witness::Camera::LiveStreamEvent::PartialReady:
				ctrl["type"] = "partial";
				ctrl["segmentIndex"] = ev.SegmentIndex;
				ctrl["partIndex"] = ev.PartIndex;
				ctrl["duration"] = ev.Duration;
				ctrl["independent"] = ev.Independent;
				streams->SendControl(cameraId, ctrl.dump());
				streams->SendBinary(cameraId, ev.Data);
				break;

			case Witness::Camera::LiveStreamEvent::SegmentReady:
				ctrl["type"] = "segment";
				ctrl["segmentIndex"] = ev.SegmentIndex;
				ctrl["duration"] = ev.Duration;
				streams->SendControl(cameraId, ctrl.dump());
				break;

			case Witness::Camera::LiveStreamEvent::Discontinuity:
				ctrl["type"] = "discontinuity";
				ctrl["generation"] = ev.Generation;
				streams->SendControl(cameraId, ctrl.dump());
				break;
			}
		});
	}

	// Continuous recording
	if (Camera.ContinuousRecording)
	{
		std::string continuousPath = (std::filesystem::path(CachePath) / "continuous" / std::to_string(Camera.ID)).string();

		if (ContinuousStream)
		{
			ContinuousStream->ResetForReconnect(CameraStream.get());
		}
		else
		{
			ContinuousStream = std::make_shared<ContinuousOutputStream>(continuousPath, Camera.ID, CameraStream.get());
			ContinuousStream->SetSegmentCompleteCallback(
				[this](int cameraUID, int64_t startTimestamp, int64_t endTimestamp, int duration, const std::string& filePath)
				{
					// Get file size
					int64_t fileSize = 0;
					std::error_code ec;
					auto fsize = std::filesystem::file_size( filePath, ec );
					if( !ec ) fileSize = static_cast<int64_t>(fsize);

					// Register completed segment in database
					SQLiteDatabaseQueryInstance query(Context->Database, "CreateContinuousSegment");
					query->Bind("@CameraUID", cameraUID);
					query->Bind("@StartTimestamp", startTimestamp);
					query->Bind("@EndTimestamp", endTimestamp);
					query->Bind("@Duration", duration);
					query->Bind("@FilePath", filePath.c_str());
					query->Bind("@FileSize", fileSize);
					query->Execute([](const SQLiteDatabaseQuery&) { return true; });

					LOG_INFO("Continuous segment registered: camera %d, %ds, %s", cameraUID, duration, filePath.c_str());

					// Broadcast to WebSocket clients
					crow::json::wvalue ev;
					ev["cameraId"] = cameraUID;
					ev["from"] = startTimestamp;
					ev["to"] = endTimestamp;
					ev["duration"] = duration;
					Context->Events->Broadcast("dvr:segment", std::move(ev));
				}
			);
		}

		// Wire packet callback so ContinuousStream receives every video packet
		auto contStream = ContinuousStream;
		CameraStream->SetPacketCallback([contStream](const AVPacket* pkt)
		{
			contStream->WritePacket(pkt);
		});
	}

	if (_strnicmp(CamPath.c_str(), "rtsp://", 7) == 0)
	{
		IsRTSP = true;
	}
}

void CameraWorker::WorkerInit()
{
	UpdateLastTimedAction("Creating filters...");

	MotionChainNode NoContinuation;
	
	Observer = std::make_shared<ObservingMotionFilter>( NoContinuation, Camera.ID, MessageBusObject );

	// Set up detection overlay callback -- stores detection boxes and broadcasts via WebSocket
	if( Context && Context->Database && Context->Events )
	{
		auto db = Context->Database;
		auto events = Context->Events;
		auto tracker = std::make_shared<Witness::ObjectTracker>();
		auto cachePath = std::string( Context->CachePath.begin(), Context->CachePath.end() );
		auto soundManager = Context->Sound;
		auto faceEmbModel = Context->FaceEmbeddingModel;
		auto faceCache = Context->FaceCache;
		double faceRecThreshold = Video.FaceRecognitionConfidence;
		bool faceAutoAssign = Video.FaceRecognitionAutoAssign;
		double faceMinSharpness = 30.0;  // Minimum Laplacian variance to accept a face crop

		// Track best face sharpness per tracking ID to avoid saving blurry duplicates
		struct FaceSharpnessEntry { double sharpness; double timestamp; };
		auto faceSharpnessMap = std::make_shared<std::unordered_map<uint64_t, FaceSharpnessEntry>>();

		Observer->SetDetectionCallback( [db, events, tracker, cachePath, soundManager, faceEmbModel, faceCache, faceRecThreshold, faceAutoAssign, faceMinSharpness, faceSharpnessMap]( const DetectionFrameData& frame )
		{
			// Run IoU tracker for stable TrackingIDs
			std::vector<Witness::TrackedBox> trackInputs;
			for( auto& box : frame.Boxes )
			{
				Witness::TrackedBox tb;
				tb.X = box.X; tb.Y = box.Y; tb.W = box.W; tb.H = box.H;
				tb.ClassID = box.ClassID;
				tb.Confidence = box.Confidence;
				tb.ClassName = box.ClassName;
				trackInputs.push_back( std::move( tb ) );
			}

			auto tracked = tracker->Update( trackInputs );

			// Store in database
			int64_t frameUID = 0;
			{
				SQLiteDatabaseQueryInstance query( db, "InsertDetectionFrame" );
				query->Bind( "@CameraID", frame.CameraID );
				query->Bind( "@Timestamp", frame.Timestamp );
				query->Bind( "@FrameWidth", frame.FrameWidth );
				query->Bind( "@FrameHeight", frame.FrameHeight );
				query->BindNull( "@FramePath" );
				query->Execute( nullptr );
				frameUID = query->GetLastInsertionId();
			}

			// Prepare crop directory
			bool canCrop = !frame.DecodedFrame.empty();
			auto cropsDir = std::filesystem::path( cachePath ) / "crops" / std::to_string( frame.CameraID );
			auto facesDir = std::filesystem::path( cachePath ) / "faces" / std::to_string( frame.CameraID );
			bool cropsDirCreated = false;
			bool facesDirCreated = false;
			std::unordered_map<uint32_t, std::string> faceRecognitionNames; // trackID -> recognized name

			// Build WebSocket event and store boxes
			crow::json::wvalue ev;
			ev["cameraId"] = frame.CameraID;
			ev["timestamp"] = frame.Timestamp;
			std::vector<crow::json::wvalue> boxArray;

			// Map from original box index to tracked result for landmark lookup
			size_t trackedIdx = 0;
			for( auto& [box, trackID] : tracked )
			{
				std::string cropPath;

				// Crop detection region and save to disk
				if( canCrop && !box.ClassName.empty() )
				{
					int cropX = static_cast<int>( box.X * frame.FrameWidth );
					int cropY = static_cast<int>( box.Y * frame.FrameHeight );
					int cropW = static_cast<int>( box.W * frame.FrameWidth );
					int cropH = static_cast<int>( box.H * frame.FrameHeight );

					// Clamp to frame bounds
					cropX = std::max( 0, cropX );
					cropY = std::max( 0, cropY );
					cropW = std::min( cropW, frame.DecodedFrame.cols - cropX );
					cropH = std::min( cropH, frame.DecodedFrame.rows - cropY );

					if( cropW > 10 && cropH > 10 )
					{
						if( !cropsDirCreated )
						{
							std::filesystem::create_directories( cropsDir );
							cropsDirCreated = true;
						}

						cv::Rect cropRect( cropX, cropY, cropW, cropH );
						cv::Mat cropped = frame.DecodedFrame( cropRect ).clone();

						// Resize to a standard thumbnail (max 224px on longest side)
						int maxDim = std::max( cropped.cols, cropped.rows );
						if( maxDim > 224 )
						{
							double scale = 224.0 / maxDim;
							cv::resize( cropped, cropped, cv::Size(), scale, scale );
						}

						std::ostringstream filename;
						filename << std::fixed << std::setprecision( 3 ) << frame.Timestamp
							<< "_" << trackID << ".jpg";
						auto filePath = cropsDir / filename.str();

						std::vector<int> params = { cv::IMWRITE_JPEG_QUALITY, 80 };
						cv::imwrite( filePath.string(), cropped, params );
						cropPath = filePath.string();
					}
				}

				{
					SQLiteDatabaseQueryInstance query( db, "InsertDetectionBox" );
					query->Bind( "@FrameUID", frameUID );
					query->Bind( "@TrackingID", static_cast<int>( trackID ) );
					query->Bind( "@ClassID", box.ClassID );
					query->Bind( "@ClassName", box.ClassName.c_str() );
					query->Bind( "@Confidence", static_cast<double>( box.Confidence ) );
					query->Bind( "@X", static_cast<double>( box.X ) );
					query->Bind( "@Y", static_cast<double>( box.Y ) );
					query->Bind( "@W", static_cast<double>( box.W ) );
					query->Bind( "@H", static_cast<double>( box.H ) );
					query->Bind( "@IsBaseline", 0 );
					if( !cropPath.empty() )
						query->Bind( "@CropPath", cropPath.c_str() );
					query->Execute( nullptr );
				}

				// For face detections, also save 112x112 ArcFace-ready crop + landmarks
				if( box.ClassName == "face" && canCrop )
				{
					int fX = static_cast<int>( box.X * frame.FrameWidth );
					int fY = static_cast<int>( box.Y * frame.FrameHeight );
					int fW = static_cast<int>( box.W * frame.FrameWidth );
					int fH = static_cast<int>( box.H * frame.FrameHeight );
					fX = std::max( 0, fX );
					fY = std::max( 0, fY );
					fW = std::min( fW, frame.DecodedFrame.cols - fX );
					fH = std::min( fH, frame.DecodedFrame.rows - fY );

					if( fW > 10 && fH > 10 )
					{
						if( !facesDirCreated )
						{
							std::filesystem::create_directories( facesDir );
							facesDirCreated = true;
						}

						cv::Rect faceRect( fX, fY, fW, fH );
						cv::Mat faceCrop;
						cv::resize( frame.DecodedFrame( faceRect ), faceCrop, cv::Size( 112, 112 ) );

						// Compute sharpness via Laplacian variance -- reject blurry crops
						cv::Mat gray, laplacian;
						cv::cvtColor( faceCrop, gray, cv::COLOR_BGR2GRAY );
						cv::Laplacian( gray, laplacian, CV_64F );
						cv::Scalar mean, stddev;
						cv::meanStdDev( laplacian, mean, stddev );
						double sharpness = stddev.val[0] * stddev.val[0];  // variance

						if( sharpness < faceMinSharpness )
						{
							LOG_DEBUG( "FaceDetection: Skipping blurry face crop (sharpness=%.1f, min=%.1f)", sharpness, faceMinSharpness );
							continue;
						}

						// Only save if this is sharper than the best recent crop for this trackID
						{
							auto it = faceSharpnessMap->find( trackID );
							if( it != faceSharpnessMap->end() )
							{
								// If within 2 seconds and not sharper, skip
								if( frame.Timestamp - it->second.timestamp < 2.0 && sharpness <= it->second.sharpness )
								{
									LOG_DEBUG( "FaceDetection: Skipping face crop (sharpness=%.1f <= best=%.1f for track %llu)",
										sharpness, it->second.sharpness, trackID );
									continue;
								}
							}
							(*faceSharpnessMap)[trackID] = { sharpness, frame.Timestamp };

							// Prune old entries (> 10 seconds)
							for( auto jt = faceSharpnessMap->begin(); jt != faceSharpnessMap->end(); )
							{
								if( frame.Timestamp - jt->second.timestamp > 10.0 )
									jt = faceSharpnessMap->erase( jt );
								else
									++jt;
							}
						}

						std::ostringstream faceFilename;
						faceFilename << std::fixed << std::setprecision( 3 ) << frame.Timestamp
							<< "_" << trackID << ".jpg";
						auto facePath = facesDir / faceFilename.str();

						std::vector<int> faceParams = { cv::IMWRITE_JPEG_QUALITY, 85 };
						cv::imwrite( facePath.string(), faceCrop, faceParams );

						// Find landmark data from original boxes
						float lmX[5] = {}, lmY[5] = {};
						for( auto& origBox : frame.Boxes )
						{
							if( origBox.ClassName == "face" && origBox.HasLandmarks
								&& std::abs( origBox.X - box.X ) < 0.001f
								&& std::abs( origBox.Y - box.Y ) < 0.001f )
							{
								memcpy( lmX, origBox.LandmarkX, sizeof( lmX ) );
								memcpy( lmY, origBox.LandmarkY, sizeof( lmY ) );
								break;
							}
						}

						// Normalize landmarks relative to crop bounding box (0-1)
						// lmX/Y are already normalized to full frame (0-1) via ObservingMotionFilter
						float normLmX[5], normLmY[5];
						for( int li = 0; li < 5; li++ )
						{
							normLmX[li] = ( lmX[li] - box.X ) / box.W;
							normLmY[li] = ( lmY[li] - box.Y ) / box.H;
						}

						SQLiteDatabaseQueryInstance query( db, "InsertFaceCrop" );
						query->Bind( "@CameraID", frame.CameraID );
						query->Bind( "@Timestamp", frame.Timestamp );
						query->Bind( "@FrameUID", frameUID );
						query->Bind( "@TrackingID", static_cast<int>( trackID ) );
						query->Bind( "@FilePath", facePath.string().c_str() );
						query->Bind( "@Confidence", static_cast<double>( box.Confidence ) );
						query->Bind( "@Landmark0X", static_cast<double>( normLmX[0] ) );
						query->Bind( "@Landmark0Y", static_cast<double>( normLmY[0] ) );
						query->Bind( "@Landmark1X", static_cast<double>( normLmX[1] ) );
						query->Bind( "@Landmark1Y", static_cast<double>( normLmY[1] ) );
						query->Bind( "@Landmark2X", static_cast<double>( normLmX[2] ) );
						query->Bind( "@Landmark2Y", static_cast<double>( normLmY[2] ) );
						query->Bind( "@Landmark3X", static_cast<double>( normLmX[3] ) );
						query->Bind( "@Landmark3Y", static_cast<double>( normLmY[3] ) );
						query->Bind( "@Landmark4X", static_cast<double>( normLmX[4] ) );
						query->Bind( "@Landmark4Y", static_cast<double>( normLmY[4] ) );
						query->Execute( nullptr );
						int64_t cropUID = query->GetLastInsertionId();

						// Generate face embedding and match against known faces
						// Skip low-confidence detections -- YuNet false positives produce garbage embeddings
						if( faceEmbModel && faceEmbModel->IsModelLoaded() && box.Confidence >= 0.8f )
						{
							// Validate landmark geometry -- real faces have eyes above nose above mouth
							// YuNet landmarks: 0=right eye, 1=left eye, 2=nose, 3=right mouth, 4=left mouth
							bool hasLandmarks = false;
							for( int li = 0; li < 5; li++ )
							{
								if( lmX[li] != 0.0f || lmY[li] != 0.0f ) { hasLandmarks = true; break; }
							}

							bool validGeometry = false;
							if( hasLandmarks )
							{
								// Convert to crop-relative pixels for geometry check
								float cropLmX[5], cropLmY[5];
								for( int li = 0; li < 5; li++ )
								{
									cropLmX[li] = ( lmX[li] - box.X ) / box.W * 112.0f;
									cropLmY[li] = ( lmY[li] - box.Y ) / box.H * 112.0f;
								}

								// Vertical ordering: eyes above nose above mouth
								float eyeAvgY = ( cropLmY[0] + cropLmY[1] ) * 0.5f;
								float mouthAvgY = ( cropLmY[3] + cropLmY[4] ) * 0.5f;
								float interEyeDist = std::abs( cropLmX[1] - cropLmX[0] );

								// Orientation checks -- reject extreme profile/upside-down faces
								// Relaxed for surveillance cameras which capture natural angles
								float eyeMidX = ( cropLmX[0] + cropLmX[1] ) * 0.5f;
								float noseOffsetX = std::abs( cropLmX[2] - eyeMidX );
								float eyeHeightDiff = std::abs( cropLmY[0] - cropLmY[1] );

								if( eyeAvgY < cropLmY[2] && cropLmY[2] < mouthAvgY
									&& interEyeDist > 12.0f
									&& noseOffsetX < 30.0f
									&& eyeHeightDiff < 25.0f )
								{
									validGeometry = true;
								}

								if( validGeometry )
								{
									cv::Mat alignedFace = Witness::Camera::FaceEmbeddingModel::AlignFace( faceCrop, cropLmX, cropLmY );

									auto embedding = faceEmbModel->GetEmbedding( alignedFace );
									if( !embedding.empty() )
									{
										// Match against known faces (for display only -- not persisted)
										std::string matchedName;
										double matchConf = 0.0;
										int matchedKnownFaceUID = 0;

										if( faceCache )
										{
											auto match = faceCache->Match( embedding, (float)faceRecThreshold );
											if( match.Matched )
											{
												matchConf = match.Similarity;
												matchedName = match.Name;
												matchedKnownFaceUID = match.KnownFaceUID;
												faceRecognitionNames[trackID] = matchedName;

												crow::json::wvalue recEv;
												recEv["cameraId"] = frame.CameraID;
												recEv["cropUID"] = static_cast<int>( cropUID );
												recEv["knownFaceUID"] = match.KnownFaceUID;
												recEv["name"] = matchedName;
												recEv["confidence"] = matchConf;
												recEv["timestamp"] = frame.Timestamp;
												events->Broadcast( "face:recognized", std::move( recEv ) );
											}
										}

										// Store embedding -- auto-assign if enabled and matched
										SQLiteDatabaseQueryInstance embQ( db, "InsertFaceEmbedding" );
										embQ->Bind( "@FaceCropUID", static_cast<int>( cropUID ) );
										if( faceAutoAssign && matchedKnownFaceUID > 0 )
										{
											embQ->Bind( "@KnownFaceUID", matchedKnownFaceUID );
										}
										else
										{
											embQ->BindNull( "@KnownFaceUID" );
										}
										embQ->BindBlob( "@Embedding", embedding.data(), static_cast<int>( embedding.size() * sizeof( float ) ) );
										embQ->Bind( "@Dimension", static_cast<int>( embedding.size() ) );
										embQ->Bind( "@MatchConfidence", matchConf );
										embQ->Bind( "@Verified", 0 );
										embQ->Bind( "@CreatedAt", frame.Timestamp );
										embQ->Execute( nullptr );
									}
								}
							}
						}
					}
				}

				crow::json::wvalue boxJson;
				boxJson["id"] = trackID;
				boxJson["cls"] = box.ClassName;
				boxJson["conf"] = static_cast<double>( box.Confidence );
				boxJson["x"] = static_cast<double>( box.X );
				boxJson["y"] = static_cast<double>( box.Y );
				boxJson["w"] = static_cast<double>( box.W );
				boxJson["h"] = static_cast<double>( box.H );
				if( box.ClassName == "face" )
				{
					auto nameIt = faceRecognitionNames.find( trackID );
					if( nameIt != faceRecognitionNames.end() )
						boxJson["name"] = nameIt->second;
				}
				boxArray.push_back( std::move( boxJson ) );
			}

			ev["boxes"] = std::move( boxArray );
			events->Broadcast( "detection:frame", std::move( ev ) );

			// Trigger detection-based actions only during active motion (not baseline/idle)
			if( frame.IsMotion && soundManager )
			{
			std::set<std::string> detectedClasses;
			// Collect max confidence per class for threshold filtering
			std::unordered_map<std::string, float> classConfidence;
			for( auto& [box, trackID] : tracked )
			{
				if( !box.ClassName.empty() )
				{
					detectedClasses.insert( box.ClassName );
					auto& best = classConfidence[box.ClassName];
					if( box.Confidence > best ) best = box.Confidence;
				}
			}

			// Add face recognition synthetic classes for actions
			if( !faceRecognitionNames.empty() )
			{
				detectedClasses.insert( "known_face" );
				classConfidence["known_face"] = 1.0f;
				for( auto& [tid, name] : faceRecognitionNames )
				{
					auto cls = "face:" + name;
					detectedClasses.insert( cls );
					classConfidence[cls] = 1.0f;
				}
			}
			// If faces detected but none recognized → unknown_face
			if( detectedClasses.count( "face" ) && faceRecognitionNames.empty() && faceEmbModel && faceEmbModel->IsModelLoaded() )
			{
				detectedClasses.insert( "unknown_face" );
				classConfidence["unknown_face"] = classConfidence["face"];
			}

			for( auto& cls : detectedClasses )
			{
				// Look up actions for this camera + detection class
				SQLiteDatabaseQueryInstance findQ( db, "FindDetectionActions" );
				findQ->Bind( "@CameraUID", frame.CameraID );
				findQ->Bind( "@DetectionClass", cls.c_str() );

				struct ActionMatch { int uid; double threshold; };
				std::vector<ActionMatch> matches;
				findQ->Execute( [&]( const SQLiteDatabaseQuery& q ) {
					matches.push_back({ q.GetColumnValueInt( 0 ), q.GetColumnValueDouble( 1 ) });
					return true;
				});

				float confidence = classConfidence[cls];

				for( auto& [uid, threshold] : matches )
				{
					// MDThreshold acts as minimum confidence for detection-class actions
					if( confidence < threshold )
						continue;

					SQLiteDatabaseQueryInstance getQ( db, "GetAction" );
					getQ->Bind( "@ActionUID", uid );
					getQ->Execute( [&]( const SQLiteDatabaseQuery& q ) {
						std::string command = q.GetColumnValueText( 2 );
						std::string param1  = q.GetColumnValueText( 3 );
						int priority        = q.GetColumnValueInt( 6 );
						int cooldown        = q.GetColumnValueInt( 7 );

						if( command == "PlaySound" )
						{
							auto soundFile = SoundManager::ResolveSoundPath( param1 );
							if( soundManager->TryPlaySound( uid, priority, cooldown, soundFile ) )
							{
								LOG_INFO( "Detection action: %s on camera %d -> PlaySound(%s) pri=%d cd=%ds",
									cls.c_str(), frame.CameraID, soundFile.c_str(), priority, cooldown );
							}
						}
						return true;
					});
				}
			}
			} // if( frame.IsMotion )
		});
	}

	MotionChainNode Observing;
	Observing.OnSuccess = Observer;
	Observing.OnFailure = Observer;

	// If ONNX detection is enabled, insert it between motion detection and observer.
	// ONNX receives ALL frames: motion frames for detection, non-motion frames for baseline capture.
	std::shared_ptr<IRecordFilter> PostMotionTarget = Observer;
	std::shared_ptr<IRecordFilter> NoMotionTarget = Observer;

	if( Video.DetectionEnabled && !Video.DetectionModelPath.empty() )
	{
		// Determine the final target after detection (face detection if enabled, otherwise observer)
		std::shared_ptr<IRecordFilter> DetectionTarget = Observer;

		// Insert face detection filter between ONNX detection and observer
		if( Video.FaceDetectionEnabled && !Video.FaceDetectionModelPath.empty() )
		{
			MotionChainNode FaceChain;
			FaceChain.OnSuccess = Observer;
			FaceChain.OnFailure = Observer;

			auto FaceFilter = std::make_shared<FaceDetectionFilter>(
				FaceChain,
				Video.FaceDetectionModelPath.c_str(),
				(float)Video.FaceDetectionConfidence,
				(float)Video.FaceBurstDuration
			);

			if( FaceFilter->IsModelLoaded() )
			{
				DetectionTarget = FaceFilter;
				LOG_INFO( "Camera %d: Face detection enabled (confidence: %.2f)", Camera.ID, Video.FaceDetectionConfidence );
			}
			else
			{
				LOG_WARNING( "Camera %d: Face detection failed to load, skipping.", Camera.ID );
			}
		}

		MotionChainNode DetectionChain;
		DetectionChain.OnSuccess = DetectionTarget;
		DetectionChain.OnFailure = DetectionTarget;

		auto DetectionFilter = std::make_shared<ONNXDetectionFilter>(
			DetectionChain,
			Video.DetectionModelPath.c_str(),
			(float)Video.DetectionConfidence,
			Video.DetectionUseGPU,
			(float)Video.DetectionMaxFPS,
			Video.DetectionCudnnPath.empty() ? nullptr : Video.DetectionCudnnPath.c_str()
		);

		if( DetectionFilter->IsModelLoaded() )
		{
			PostMotionTarget = DetectionFilter;
			NoMotionTarget = DetectionFilter;  // Also receives non-motion frames for baseline
			LOG_INFO( "Camera %d: ONNX detection enabled (model: %s, confidence: %.2f, max %.1f fps)",
				Camera.ID, Video.DetectionModelPath.c_str(), Video.DetectionConfidence, Video.DetectionMaxFPS );
		}
		else
		{
			LOG_WARNING( "Camera %d: ONNX detection failed to load, falling back to motion-only.", Camera.ID );
		}
	}

	MotionChainNode MVF;
	MVF.OnSuccess = PostMotionTarget;
	MVF.OnFailure = NoMotionTarget;
	MVF.MinimumThreshold = (float)Camera.MDThreshold;
	MVF.InclusiveFilter = ClassificationResult::Motion_Motion;
	MVF.ExclusiveFilter = 0;

	std::shared_ptr<MotionVectorFilter> RootFilter = std::make_shared<MotionVectorFilter>( MVF, Camera.BlackoutMaskPath.c_str(), Camera.FocusMaskPath.c_str() );

	Filter = RootFilter;

	MessageBusObject->SendToClient( nullptr, std::make_shared<CameraStartupMessage>( Camera.ID ) );

	UpdateLastTimedAction("Starting camera connection...");

	CreateInputStream();
}

void CameraWorker::WorkerShutdown()
{
	//Ensure destruction is done on the worker thread
	Filter = nullptr;
	ContinuousStream = nullptr;
	LiveStream = nullptr;
	CameraStream = nullptr;

	MessageBusObject->SendToClient( nullptr, std::make_shared<ThreadShutdownMessage>() );
}

void CameraWorker::WorkerMain()
{
	UpdateLastTimedAction("Work...");

	std::shared_ptr<Message> Msg;
	while( MessageBusQueue->TryPop( Msg ) )
	{
		Msg->Handle<ThreadShutdownMessage>([&](const ThreadShutdownMessage& Data)
		{
			RequestShutdown();
		});

		Msg->Handle<CameraPreviewRequestMessage>([&](const CameraPreviewRequestMessage& Data)
		{
			Observer->SetPreviewTimestamps( Data.LastLargePreviewTimestamp, Data.LastSmallPreviewTimestamp );
		});

		Msg->Handle<CameraStartRecordMessage>([&](const CameraStartRecordMessage& Data)
		{
			OnClipFinished(false);
				
			Observer->SetManualClipStart( Data.Timestamp );
			RecordStream = std::make_shared<OutputStream>( std::string( Data.Path.begin(), Data.Path.end() ), CameraStream.get(), false, false, false, false );
			CameraStreamError InitResult = RecordStream->Initialize();
			if (InitResult != CameraStreamError::Success)
			{
				LOG_ERROR("Recording init failed for camera %d: %s", Camera.ID, RecordStream->GetFFMPEGErrorMessage());
				RecordStream.reset();
			}

			Context->LongPoll->NotifyAll();
		});

		Msg->Handle<CameraStopRecordMessage>([&](const CameraStopRecordMessage& Data)
		{
			OnClipFinished(Data.ManualStop);

			if( Data.ManualStop )
			{
				Filter->ClearState();
			}

			Context->LongPoll->NotifyAll();
		});
	}

	if (IsConnected)
	{
		UpdateLastTimedAction("Processing...");
	}
	else
	{
		UpdateLastTimedAction("Connecting...");
	}

	double FrameRate = CameraStream->GetFramerateDouble();
	double FrameTime = 1.0f / FrameRate;

	const double DeletetionCheckPeriod = 120.0;
	const double BufferPeriodInMilliseconds = 0.0;
	const double NanoSecondsToSeconds = 1000.0 * 1000.0 * 1000.0;
	uint64_t Start = std::chrono::high_resolution_clock::now().time_since_epoch().count();
	
	CameraStreamError Error = CameraStream->ProcessFrame( std::static_pointer_cast<IRecordFilter>(Filter), RecordStream.get(), LiveStream.get() );

	if( Error == CameraStreamError::Success )
	{
		if( !IsConnected )
		{
			IsConnected = true;

			LOG_INFO("[HLS] Camera %d connected, live stream generation %d",
				Camera.ID, LiveStream ? LiveStream->GetInitGeneration() : 0);

			MessageBusObject->SendToClient( nullptr, std::make_shared<CameraConnectedMessage>( Camera.ID ) );
		}
	}
	else
	{
		IsConnected = false;

		OnClipFinished(false);

		std::string ErrorStrA = GetCameraStreamErrorMessage(Error);
		if( CameraStream->GetFFMPEGErrorMessage()[0] != '\0')
		{
			ErrorStrA += ": ";
			ErrorStrA += CameraStream->GetFFMPEGErrorMessage();
		}
		std::string ErrorStr(ErrorStrA.begin(), ErrorStrA.end());

		LOG_WARNING("[HLS] Camera %d disconnected: %s", Camera.ID, ErrorStrA.c_str());

		MessageBusObject->SendToClient( nullptr, std::make_shared<CameraReconnectMessage>( Camera.ID, ErrorStr ) );

		CreateInputStream();
		LastFrameTime = 0;

		Camera.JobQueue->RemoveAllForSource( Camera.ID );

		if( Error != CameraStreamError::EndOfFile )
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(3000));
		}
	}

	uint64_t End = std::chrono::high_resolution_clock::now().time_since_epoch().count();

	if (!IsRTSP && IsConnected)
	{
		double Duration = (double)(End - Start) / NanoSecondsToSeconds;
		if (Duration < FrameTime)
		{
			double MillisecondsToWait = ((FrameTime - Duration) * 1000.0);

			MillisecondsToWait = std::max( MillisecondsToWait - BufferPeriodInMilliseconds, 0.0 );

			if( MillisecondsToWait > 0.0 )
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int>(MillisecondsToWait)));
			}
		}
	}

	LastFrameTime = Start;
}

void CameraWorker::OnClipFinished(bool ManualStop)
{
	if (RecordStream)
	{
		Observer->SetManualClipEnd( GetUnixTimestamp() );

		auto FinishedMessage = std::make_shared<CameraClipFinishedMessage>( Camera.ID, ManualStop );
		FinishedMessage->Result = Observer->GetCurrentResult();
		FinishedMessage->ClipStats = Observer->GetClipStatistics();
		MessageBusObject->SendToClient( nullptr, FinishedMessage );
		Filter->ClearState();



		RecordStream->CloseFile();
		RecordStream.reset();
	}
}
