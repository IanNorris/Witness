#include "ClipReprocessWorker.h"
#include "TagHelpers.h"
#include "ObjectTracker.h"
#include "FaceRecognitionCache.h"

#include <Log.h>
#include <FaceEmbeddingModel.h>
#include <filesystem>
#include <set>
#include <map>
#include <array>
#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <sstream>
#include <iomanip>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
}

#include <opencv2/core/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>

namespace fs = std::filesystem;

ClipReprocessWorker::ClipReprocessWorker(
	const std::shared_ptr<MessageBus>& MessageBus,
	std::shared_ptr<SQLiteDatabase> Database,
	std::shared_ptr<Witness::Camera::ONNXDetectionFilter> DetectionFilter,
	std::shared_ptr<Witness::Camera::FaceDetectionFilter> FaceFilter,
	std::shared_ptr<Witness::Camera::FaceEmbeddingModel> FaceEmbModel,
	std::shared_ptr<FaceRecognitionCache> FaceCache,
	std::shared_ptr<EventBroadcaster> Events,
	double FaceRecThreshold,
	double DetectionMaxFPS,
	std::string CachePath,
	std::function<bool()> IsIdle
)
: WorkerBase( MessageBus )
, Database( std::move( Database ) )
, DetectionFilter( std::move( DetectionFilter ) )
, FaceFilter( std::move( FaceFilter ) )
, FaceEmbModel( std::move( FaceEmbModel ) )
, FaceCache( std::move( FaceCache ) )
, Events( std::move( Events ) )
, FaceRecThreshold( FaceRecThreshold )
, DetectionMaxFPS( DetectionMaxFPS )
, CachePath( std::move( CachePath ) )
, IsIdle( std::move( IsIdle ) )
{
}

struct ClipToReprocess
{
	int64_t ClipUID;
	int64_t Timestamp;
	int Camera;
	int RecordMode;
	std::string ExistingTags;
};

void ClipReprocessWorker::WorkerMain()
{
	UpdateLastTimedAction( "Idle" );

	// Count total clips needing reprocessing
	int totalQueue = 0;
	{
		SQLiteDatabaseQueryInstance CountQuery( Database, "CountClipsToReprocess" );
		CountQuery->Bind( "@DetectionVersion", CURRENT_DETECTION_VERSION );
		CountQuery->Execute( [&]( const SQLiteDatabaseQuery& query ) -> bool
		{
			totalQueue = query.GetColumnValueInt( 0 );
			return true;
		});
	}

	if( totalQueue == 0 )
	{
		BroadcastProgress( 0, "idle", 0, 0, 0, 0 );

		if( !LightingBackfillComplete )
			BackfillLighting();

		std::this_thread::sleep_for( std::chrono::seconds( 30 ) );
		return;
	}

	UpdateLastTimedAction( "Reprocessing clips" );

	// Fetch and process ONE clip at a time (re-fetches after each to respect priority)
	SQLiteDatabaseQueryInstance SelectClipForReprocess( Database, "SelectClipForReprocess" );
	SelectClipForReprocess->Bind( "@DetectionVersion", CURRENT_DETECTION_VERSION );

	ClipToReprocess clip;
	bool found = false;
	SelectClipForReprocess->Execute( [&]( const SQLiteDatabaseQuery& query ) -> bool
	{
		clip.ClipUID = query.GetColumnValueInt64( 0 );
		clip.Timestamp = query.GetColumnValueInt64( 1 );
		clip.Camera = query.GetColumnValueInt( 2 );
		clip.RecordMode = query.GetColumnValueInt( 6 );
		const char* tags = query.GetColumnValueText( 10 );
		if( tags )
			clip.ExistingTags = tags;
		found = true;
		return false; // Only take the first result
	});

	if( !found )
		return;

	ProcessClip( clip.ClipUID, clip.Timestamp, clip.Camera, clip.RecordMode, clip.ExistingTags,
		0, totalQueue );

	// Throttle harder when cameras are actively recording
	if( !IsIdle() )
		std::this_thread::sleep_for( std::chrono::milliseconds( 500 ) );
	else
		std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );
}

void ClipReprocessWorker::MarkClipProcessed( int64_t clipUID )
{
	SQLiteDatabaseQueryInstance q( Database, "UpdateClipDetection" );
	q->Bind( "@ClipUID", clipUID );
	q->Bind( "@Tags", "" );
	q->Bind( "@DetectionVersion", CURRENT_DETECTION_VERSION );
	q->Bind( "@Lighting", 0 );
	q->Execute( nullptr );

	SQLiteDatabaseQueryInstance q2( Database, "DeleteClipTags" );
	q2->Bind( "@ClipUID", clipUID );
	q2->Execute( nullptr );
}

void ClipReprocessWorker::ProcessClip( int64_t clipUID, int64_t timestamp, int camera, int recordMode, const std::string& existingTags,
	int queuePosition, int queueTotal )
{
	// Build clip filename: {Camera}_{Auto|Manual}_{Timestamp}.mp4
	std::stringstream nameStream;
	nameStream << camera << "_" << ( recordMode == 1 ? "Auto" : "Manual" ) << "_" << timestamp << ".mp4";
	std::string clipPath = ( fs::path( CachePath ) / nameStream.str() ).string();

	if( !fs::exists( clipPath ) )
	{
		LOG_DEBUG( "ClipReprocess: Clip %lld file not found, skipping: %s", (long long)clipUID, clipPath.c_str() );
		MarkClipProcessed( clipUID );
		return;
	}

	// Skip empty or zero-byte files (vestigial clips that never completed recording)
	auto fileSize = fs::file_size( clipPath );
	if( fileSize == 0 )
	{
		LOG_DEBUG( "ClipReprocess: Clip %lld is empty (0 bytes), skipping: %s", (long long)clipUID, clipPath.c_str() );
		MarkClipProcessed( clipUID );
		return;
	}

	// Open the clip with FFmpeg
	AVFormatContext* fmtCtx = nullptr;
	if( avformat_open_input( &fmtCtx, clipPath.c_str(), nullptr, nullptr ) < 0 )
	{
		LOG_ERROR( "ClipReprocess: Failed to open clip %lld, marking as processed: %s", (long long)clipUID, clipPath.c_str() );
		MarkClipProcessed( clipUID );
		return;
	}

	if( avformat_find_stream_info( fmtCtx, nullptr ) < 0 )
	{
		LOG_ERROR( "ClipReprocess: Failed to find stream info for clip %lld, marking as processed", (long long)clipUID );
		avformat_close_input( &fmtCtx );
		MarkClipProcessed( clipUID );
		return;
	}

	// Find video stream
	int videoIdx = -1;
	for( unsigned i = 0; i < fmtCtx->nb_streams; i++ )
	{
		if( fmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO )
		{
			videoIdx = i;
			break;
		}
	}

	if( videoIdx < 0 )
	{
		LOG_ERROR( "ClipReprocess: No video stream in clip %lld, marking as processed", (long long)clipUID );
		avformat_close_input( &fmtCtx );
		MarkClipProcessed( clipUID );
		return;
	}

	// Open decoder
	auto codecpar = fmtCtx->streams[videoIdx]->codecpar;
	auto codec = avcodec_find_decoder( codecpar->codec_id );
	if( !codec )
	{
		LOG_ERROR( "ClipReprocess: No decoder for clip %lld, marking as processed", (long long)clipUID );
		avformat_close_input( &fmtCtx );
		MarkClipProcessed( clipUID );
		return;
	}

	auto codecCtx = avcodec_alloc_context3( codec );
	avcodec_parameters_to_context( codecCtx, codecpar );
	if( avcodec_open2( codecCtx, codec, nullptr ) < 0 )
	{
		LOG_ERROR( "ClipReprocess: Failed to open decoder for clip %lld, marking as processed", (long long)clipUID );
		avcodec_free_context( &codecCtx );
		avformat_close_input( &fmtCtx );
		MarkClipProcessed( clipUID );
		return;
	}

	// Set up SwsContext for pixel format conversion to BGR24
	SwsContext* swsCtx = sws_getContext(
		codecCtx->width, codecCtx->height, codecCtx->pix_fmt,
		codecCtx->width, codecCtx->height, AV_PIX_FMT_BGR24,
		SWS_BILINEAR, nullptr, nullptr, nullptr
	);

	if( !swsCtx )
	{
		LOG_ERROR( "ClipReprocess: Failed to create SwsContext for clip %lld, marking as processed", (long long)clipUID );
		avcodec_free_context( &codecCtx );
		avformat_close_input( &fmtCtx );
		MarkClipProcessed( clipUID );
		return;
	}

	// Determine clip duration for sampling every 2 seconds
	AVStream* videoStream = fmtCtx->streams[videoIdx];
	double durationSec = 0.0;
	if( fmtCtx->duration > 0 )
		durationSec = (double)fmtCtx->duration / AV_TIME_BASE;
	else if( videoStream->duration > 0 && videoStream->time_base.den > 0 )
		durationSec = (double)videoStream->duration * av_q2d( videoStream->time_base );

	std::set<std::string> detectedTags;
	Witness::Camera::LightingCondition lighting = Witness::Camera::LightingCondition::Unknown;
	AVPacket* pkt = av_packet_alloc();
	AVFrame* frame = av_frame_alloc();
	AVFrame* bgrFrame = av_frame_alloc();

	int frameWidth = codecCtx->width;
	int frameHeight = codecCtx->height;

	// Clear old detection overlay data for this clip's time range before re-generating
	{
		double fromTs = static_cast<double>( timestamp );
		double toTs = static_cast<double>( timestamp ) + durationSec + 1.0;
		SQLiteDatabaseQueryInstance delDet( Database, "DeleteDetectionFramesInRange" );
		delDet->Bind( "@CameraID", camera );
		delDet->Bind( "@TimestampFrom", fromTs );
		delDet->Bind( "@TimestampTo", toTs );
		delDet->Execute( nullptr );
	}

	// Allocate BGR frame buffer
	int bgrBufSize = av_image_get_buffer_size( AV_PIX_FMT_BGR24, frameWidth, frameHeight, 1 );
	std::vector<uint8_t> bgrBuffer( bgrBufSize );
	av_image_fill_arrays( bgrFrame->data, bgrFrame->linesize, bgrBuffer.data(),
		AV_PIX_FMT_BGR24, frameWidth, frameHeight, 1 );

	// Sample frames at DetectionMaxFPS rate (matching live pipeline)
	double sampleInterval = ( DetectionMaxFPS > 0.0 ) ? ( 1.0 / DetectionMaxFPS ) : 0.1;
	double nextSampleTime = 0.0;
	std::vector<Witness::Camera::DetectionResult> baselineDetections;
	bool baselineCaptured = false;
	Witness::ObjectTracker clipTracker;
	int totalFrames = std::max( 1, (int)( durationSec / sampleInterval ) + 1 );
	int frameIndex = 0;

	// Track best face sharpness per tracking ID (same as live pipeline)
	struct FaceSharpnessEntry { double sharpness; double timestamp; };
	std::unordered_map<uint64_t, FaceSharpnessEntry> faceSharpnessMap;

	BroadcastProgress( clipUID, "processing", 0, totalFrames, queuePosition, queueTotal );

	// Sequential decode - read all packets, only process frames at sample intervals
	AVStream* vs = fmtCtx->streams[videoIdx];
	double timeBase = av_q2d( vs->time_base );

	while( av_read_frame( fmtCtx, pkt ) >= 0 && !IsShutdownRequested() )
	{
		if( pkt->stream_index != videoIdx )
		{
			av_packet_unref( pkt );
			continue;
		}

		avcodec_send_packet( codecCtx, pkt );
		av_packet_unref( pkt );

		while( avcodec_receive_frame( codecCtx, frame ) == 0 )
		{
			// Calculate frame time from PTS
			double frameTime = 0.0;
			if( frame->pts != AV_NOPTS_VALUE )
				frameTime = frame->pts * timeBase;
			else if( frame->pkt_dts != AV_NOPTS_VALUE )
				frameTime = frame->pkt_dts * timeBase;

			// Skip frames that aren't at our sample interval
			if( frameTime < nextSampleTime && baselineCaptured )
			{
				av_frame_unref( frame );
				continue;
			}
			nextSampleTime = frameTime + sampleInterval;

			// Convert to BGR24 cv::Mat
			sws_scale( swsCtx, frame->data, frame->linesize, 0, frameHeight,
				bgrFrame->data, bgrFrame->linesize );

			cv::Mat mat( frameHeight, frameWidth, CV_8UC3, bgrFrame->data[0], bgrFrame->linesize[0] );

					// Classify lighting on first frame
					if( !baselineCaptured )
						lighting = Witness::Camera::ClassifyLighting( mat );

					// Apply CLAHE for night/IR frames to improve detection
					cv::Mat detectionMat = ( lighting == Witness::Camera::LightingCondition::Night )
						? Witness::Camera::ApplyCLAHE( mat )
						: mat;

					// Run ONNX detection
					auto detections = DetectionFilter->DetectFrame( detectionMat );

					if( !baselineCaptured )
					{
						// First frame: capture all detections as baseline (with bounding boxes)
						baselineDetections = detections;
						baselineCaptured = true;
					}

					// Classify each detection as baseline or new
					struct ClassifiedDetection
					{
						Witness::Camera::DetectionResult det;
						bool isBaseline;
					};
					std::vector<ClassifiedDetection> classified;

					for( auto& det : detections )
					{
						bool matchesBaseline = false;
						for( auto& base : baselineDetections )
						{
							if( det.ClassId != base.ClassId )
								continue;
							float ix1 = std::max( det.X1, base.X1 );
							float iy1 = std::max( det.Y1, base.Y1 );
							float ix2 = std::min( det.X2, base.X2 );
							float iy2 = std::min( det.Y2, base.Y2 );
							float iw = std::max( 0.0f, ix2 - ix1 );
							float ih = std::max( 0.0f, iy2 - iy1 );
							float intersection = iw * ih;
							float areaA = ( det.X2 - det.X1 ) * ( det.Y2 - det.Y1 );
							float areaB = ( base.X2 - base.X1 ) * ( base.Y2 - base.Y1 );
							float unionArea = areaA + areaB - intersection;
							float iou = unionArea > 0.0f ? intersection / unionArea : 0.0f;
							if( iou > 0.5f )
							{
								matchesBaseline = true;
								break;
							}
						}
						if( !matchesBaseline )
							detectedTags.insert( det.ClassName );
						classified.push_back( { det, matchesBaseline } );
					}

					// Store all detection boxes (baseline + new) for overlay playback
					if( !classified.empty() )
					{
						// Run face detection on person crops if face filter available
						std::vector<Witness::Camera::FaceDetectionResult> faceResults;
						if( FaceFilter && FaceFilter->IsModelLoaded() )
						{
							// Build ROIs from person detections for face detection
							std::vector<Witness::Camera::ClassificationResult::RegionOfInterest> personROIs;
							for( auto& c : classified )
							{
								if( c.det.ClassName == "person" )
								{
									Witness::Camera::ClassificationResult::RegionOfInterest roi;
									roi.Left = static_cast<unsigned int>( c.det.X1 * frameWidth );
									roi.Top = static_cast<unsigned int>( c.det.Y1 * frameHeight );
									roi.Width = static_cast<unsigned int>( ( c.det.X2 - c.det.X1 ) * frameWidth );
									roi.Height = static_cast<unsigned int>( ( c.det.Y2 - c.det.Y1 ) * frameHeight );
									roi.Classification = Witness::Camera::ClassificationResult::Motion_Person;
									personROIs.push_back( roi );
								}
							}
							if( !personROIs.empty() )
							{
								faceResults = FaceFilter->DetectFacesInPersonCrops( mat, personROIs );
								LOG_DEBUG( "ClipReprocess: Face detection on %zu person crops -> %zu faces (clip %lld)",
									personROIs.size(), faceResults.size(), (long long)clipUID );
							}
						}

						// Run tracker for stable IDs
						std::vector<Witness::TrackedBox> trackInputs;
						for( auto& c : classified )
						{
							Witness::TrackedBox tb;
							tb.X = c.det.X1; tb.Y = c.det.Y1;
							tb.W = c.det.X2 - c.det.X1; tb.H = c.det.Y2 - c.det.Y1;
							tb.ClassID = c.det.ClassId;
							tb.Confidence = c.det.Confidence;
							tb.ClassName = c.det.ClassName;
							trackInputs.push_back( std::move( tb ) );
						}
						// Add face detections to tracker
						for( auto& face : faceResults )
						{
							Witness::TrackedBox tb;
							tb.X = face.X1 / frameWidth; tb.Y = face.Y1 / frameHeight;
							tb.W = ( face.X2 - face.X1 ) / frameWidth;
							tb.H = ( face.Y2 - face.Y1 ) / frameHeight;
							tb.ClassID = 100;  // FACE_CLASS_ID
							tb.Confidence = face.Confidence;
							tb.ClassName = "face";
							trackInputs.push_back( std::move( tb ) );
						}
						auto tracked = clipTracker.Update( trackInputs );

						double frameTimestamp = static_cast<double>( timestamp ) + frameTime;

						// Save detection frame image for trails/DVR preview
						auto framesDir = fs::path( CachePath ) / "frames" / std::to_string( camera );
						if( !fs::exists( framesDir ) )
							fs::create_directories( framesDir );

						std::stringstream framePath;
						framePath << std::fixed << std::setprecision(3) << frameTimestamp << ".jpg";
						auto fullFramePath = ( framesDir / framePath.str() ).string();
						cv::imwrite( fullFramePath, mat, { cv::IMWRITE_JPEG_QUALITY, 75 } );

						int64_t frameUID = 0;
						{
							SQLiteDatabaseQueryInstance q( Database, "InsertDetectionFrame" );
							q->Bind( "@CameraID", camera );
							q->Bind( "@Timestamp", frameTimestamp );
							q->Bind( "@FrameWidth", frameWidth );
							q->Bind( "@FrameHeight", frameHeight );
							q->Bind( "@FramePath", fullFramePath.c_str() );
							q->Execute( nullptr );
							frameUID = q->GetLastInsertionId();
						}

						// Prepare crop directories
						auto cropsDir = fs::path( CachePath ) / "crops" / std::to_string( camera );
						auto facesDir = fs::path( CachePath ) / "faces" / std::to_string( camera );
						bool cropsDirCreated = false;
						bool facesDirCreated = false;

						for( size_t i = 0; i < tracked.size(); i++ )
						{
							auto& [box, trackID] = tracked[i];
							bool isBase = ( i < classified.size() ) ? classified[i].isBaseline : false;

							// Crop detection region
							std::string cropPath;
							int cropX = static_cast<int>( box.X * frameWidth );
							int cropY = static_cast<int>( box.Y * frameHeight );
							int cropW = static_cast<int>( box.W * frameWidth );
							int cropH = static_cast<int>( box.H * frameHeight );
							cropX = std::max( 0, cropX );
							cropY = std::max( 0, cropY );
							cropW = std::min( cropW, mat.cols - cropX );
							cropH = std::min( cropH, mat.rows - cropY );

							if( cropW > 10 && cropH > 10 && !box.ClassName.empty() )
							{
								if( !cropsDirCreated )
								{
									fs::create_directories( cropsDir );
									cropsDirCreated = true;
								}

								cv::Rect cropRect( cropX, cropY, cropW, cropH );
								cv::Mat cropped = mat( cropRect ).clone();
								int maxDim = std::max( cropped.cols, cropped.rows );
								if( maxDim > 224 )
								{
									double scale = 224.0 / maxDim;
									cv::resize( cropped, cropped, cv::Size(), scale, scale );
								}

								std::ostringstream filename;
								filename << std::fixed << std::setprecision( 3 ) << frameTimestamp
									<< "_" << trackID << ".jpg";
								auto filePath = cropsDir / filename.str();

								std::vector<int> params = { cv::IMWRITE_JPEG_QUALITY, 80 };
								cv::imwrite( filePath.string(), cropped, params );
								cropPath = filePath.string();
							}

							SQLiteDatabaseQueryInstance q( Database, "InsertDetectionBox" );
							q->Bind( "@FrameUID", frameUID );
							q->Bind( "@TrackingID", static_cast<int>( trackID ) );
							q->Bind( "@ClassID", box.ClassID );
							q->Bind( "@ClassName", box.ClassName.c_str() );
							q->Bind( "@Confidence", static_cast<double>( box.Confidence ) );
							q->Bind( "@X", static_cast<double>( box.X ) );
							q->Bind( "@Y", static_cast<double>( box.Y ) );
							q->Bind( "@W", static_cast<double>( box.W ) );
							q->Bind( "@H", static_cast<double>( box.H ) );
							q->Bind( "@IsBaseline", isBase ? 1 : 0 );
							if( !cropPath.empty() )
								q->Bind( "@CropPath", cropPath.c_str() );
							q->Execute( nullptr );

							// Save 112x112 face crop + landmarks
							if( box.ClassName == "face" && cropW > 10 && cropH > 10 )
							{
								if( !facesDirCreated )
								{
									fs::create_directories( facesDir );
									facesDirCreated = true;
								}

								cv::Rect faceRect( cropX, cropY, cropW, cropH );
								cv::Mat faceCrop;
								cv::resize( mat( faceRect ), faceCrop, cv::Size( 112, 112 ) );

								// Compute sharpness via Laplacian variance — reject blurry crops
								cv::Mat faceGray, faceLaplacian;
								cv::cvtColor( faceCrop, faceGray, cv::COLOR_BGR2GRAY );
								cv::Laplacian( faceGray, faceLaplacian, CV_64F );
								cv::Scalar faceMean, faceStddev;
								cv::meanStdDev( faceLaplacian, faceMean, faceStddev );
								double faceSharpness = faceStddev.val[0] * faceStddev.val[0];

								if( faceSharpness < 30.0 )
								{
									LOG_DEBUG( "Reprocess: Skipping blurry face crop (sharpness=%.1f)", faceSharpness );
									continue;
								}

								// Only save if this is sharper than the best recent crop for this trackID
								{
									auto it = faceSharpnessMap.find( trackID );
									if( it != faceSharpnessMap.end() )
									{
										if( frameTimestamp - it->second.timestamp < 2.0 && faceSharpness <= it->second.sharpness )
										{
											LOG_DEBUG( "Reprocess: Skipping face crop (sharpness=%.1f <= best=%.1f for track %llu)",
												faceSharpness, it->second.sharpness, trackID );
											continue;
										}
									}
									faceSharpnessMap[trackID] = { faceSharpness, frameTimestamp };
								}

								std::ostringstream faceFilename;
								faceFilename << std::fixed << std::setprecision( 3 ) << frameTimestamp
									<< "_" << trackID << ".jpg";
								auto facePath = facesDir / faceFilename.str();

								std::vector<int> faceParams = { cv::IMWRITE_JPEG_QUALITY, 85 };
								cv::imwrite( facePath.string(), faceCrop, faceParams );

								// Find matching face result for landmarks
								float lmX[5] = {}, lmY[5] = {};
								for( auto& fr : faceResults )
								{
									float fx = fr.X1 / frameWidth;
									float fy = fr.Y1 / frameHeight;
									if( std::abs( fx - box.X ) < 0.01f && std::abs( fy - box.Y ) < 0.01f )
									{
										// Normalize landmarks to crop-relative 0-1
										for( int lm = 0; lm < 5; lm++ )
										{
											lmX[lm] = ( fr.LandmarkX[lm] / frameWidth - box.X ) / box.W;
											lmY[lm] = ( fr.LandmarkY[lm] / frameHeight - box.Y ) / box.H;
										}
										break;
									}
								}

								SQLiteDatabaseQueryInstance fq( Database, "InsertFaceCrop" );
								fq->Bind( "@CameraID", camera );
								fq->Bind( "@Timestamp", frameTimestamp );
								fq->Bind( "@FrameUID", frameUID );
								fq->Bind( "@TrackingID", static_cast<int>( trackID ) );
								fq->Bind( "@FilePath", facePath.string().c_str() );
								fq->Bind( "@Confidence", static_cast<double>( box.Confidence ) );
								fq->Bind( "@Landmark0X", static_cast<double>( lmX[0] ) );
								fq->Bind( "@Landmark0Y", static_cast<double>( lmY[0] ) );
								fq->Bind( "@Landmark1X", static_cast<double>( lmX[1] ) );
								fq->Bind( "@Landmark1Y", static_cast<double>( lmY[1] ) );
								fq->Bind( "@Landmark2X", static_cast<double>( lmX[2] ) );
								fq->Bind( "@Landmark2Y", static_cast<double>( lmY[2] ) );
								fq->Bind( "@Landmark3X", static_cast<double>( lmX[3] ) );
								fq->Bind( "@Landmark3Y", static_cast<double>( lmY[3] ) );
								fq->Bind( "@Landmark4X", static_cast<double>( lmX[4] ) );
								fq->Bind( "@Landmark4Y", static_cast<double>( lmY[4] ) );
								fq->Execute( nullptr );
								int64_t cropUID = fq->GetLastInsertionId();

								// Generate face embedding if model is available
								if( FaceEmbModel && FaceEmbModel->IsModelLoaded() && box.Confidence >= 0.8f )
								{
									bool hasLandmarks = false;
									for( int lm = 0; lm < 5; lm++ )
									{
										if( lmX[lm] != 0.0f || lmY[lm] != 0.0f ) { hasLandmarks = true; break; }
									}

									if( hasLandmarks )
									{
										// Convert crop-relative 0-1 landmarks to 112px crop pixels for geometry check
										float cropLmX[5], cropLmY[5];
										for( int lm = 0; lm < 5; lm++ )
										{
											cropLmX[lm] = lmX[lm] * 112.0f;
											cropLmY[lm] = lmY[lm] * 112.0f;
										}

										// Orientation validation (same thresholds as CameraWorker)
										float eyeAvgY = ( cropLmY[0] + cropLmY[1] ) * 0.5f;
										float mouthAvgY = ( cropLmY[3] + cropLmY[4] ) * 0.5f;
										float interEyeDist = std::abs( cropLmX[1] - cropLmX[0] );
										float eyeMidX = ( cropLmX[0] + cropLmX[1] ) * 0.5f;
										float noseOffsetX = std::abs( cropLmX[2] - eyeMidX );
										float eyeHeightDiff = std::abs( cropLmY[0] - cropLmY[1] );

										bool validGeometry = eyeAvgY < cropLmY[2] && cropLmY[2] < mouthAvgY
											&& interEyeDist > 12.0f
											&& noseOffsetX < 30.0f
											&& eyeHeightDiff < 25.0f;

										if( validGeometry )
										{
											cv::Mat alignedFace = Witness::Camera::FaceEmbeddingModel::AlignFace( faceCrop, cropLmX, cropLmY );
											auto embedding = FaceEmbModel->GetEmbedding( alignedFace );

											if( !embedding.empty() )
											{
												double matchConf = 0.0;
												int matchedKnownFaceUID = 0;

												if( FaceCache )
												{
													auto match = FaceCache->Match( embedding, (float)FaceRecThreshold );
													if( match.Matched )
													{
														matchConf = match.Similarity;
														matchedKnownFaceUID = match.KnownFaceUID;
														LOG_INFO( "ClipReprocess: Face match '%s' (%.3f) crop %lld",
															match.Name.c_str(), matchConf, (long long)cropUID );
													}
													else
													{
														LOG_DEBUG( "ClipReprocess: No match for crop %lld (best=%.3f '%s', threshold=%.3f)",
															(long long)cropUID, match.Similarity,
															match.Name.c_str(), FaceRecThreshold );
													}
												}

												SQLiteDatabaseQueryInstance embQ( Database, "InsertFaceEmbedding" );
												embQ->Bind( "@FaceCropUID", static_cast<int>( cropUID ) );
												if( matchedKnownFaceUID > 0 )
													embQ->Bind( "@KnownFaceUID", matchedKnownFaceUID );
												else
													embQ->BindNull( "@KnownFaceUID" );
												embQ->BindBlob( "@Embedding", embedding.data(), static_cast<int>( embedding.size() * sizeof( float ) ) );
												embQ->Bind( "@Dimension", static_cast<int>( embedding.size() ) );
												embQ->Bind( "@MatchConfidence", matchConf );
												embQ->Bind( "@Verified", 0 );
												embQ->Bind( "@CreatedAt", frameTimestamp );
												embQ->Execute( nullptr );

												LOG_DEBUG( "ClipReprocess: Stored embedding for crop %lld (conf=%.2f, match=%d)",
													(long long)cropUID, box.Confidence, matchedKnownFaceUID );
											}
										}
										else
										{
											LOG_DEBUG( "ClipReprocess: Orientation rejected crop %lld (interEye=%.1f nose=%.1f eyeDiff=%.1f)",
												(long long)cropUID, interEyeDist, noseOffsetX, eyeHeightDiff );
										}
									}
									else
									{
										LOG_DEBUG( "ClipReprocess: No landmarks for crop %lld", (long long)cropUID );
									}
								}

								detectedTags.insert( "face" );
							}
						}
					}

			av_frame_unref( frame );

			frameIndex++;
			BroadcastProgress( clipUID, "processing", frameIndex, totalFrames, queuePosition, queueTotal );

			if( frameTime >= durationSec )
				break;
		}

		if( frameIndex >= totalFrames || ( nextSampleTime - sampleInterval ) >= durationSec )
			break;
	}

	// Cleanup FFmpeg resources
	av_frame_free( &bgrFrame );
	av_frame_free( &frame );
	av_packet_free( &pkt );
	sws_freeContext( swsCtx );
	avcodec_free_context( &codecCtx );
	avformat_close_input( &fmtCtx );

	// Build combined tag string (merge with existing tags)
	std::set<std::string> allTags;
	if( !existingTags.empty() )
	{
		std::istringstream iss( existingTags );
		std::string tag;
		while( std::getline( iss, tag, ';' ) )
		{
			size_t start = tag.find_first_not_of( ' ' );
			size_t end = tag.find_last_not_of( ' ' );
			if( start != std::string::npos )
				allTags.insert( tag.substr( start, end - start + 1 ) );
		}
	}
	allTags.insert( detectedTags.begin(), detectedTags.end() );

	std::string tagString;
	for( auto& t : allTags )
	{
		if( !tagString.empty() )
			tagString += ";";
		tagString += t;
	}

	// Update clip in database
	{
		SQLiteDatabaseQueryInstance UpdateClipDetection( Database, "UpdateClipDetection" );
		UpdateClipDetection->Bind( "@ClipUID", clipUID );
		UpdateClipDetection->Bind( "@Tags", tagString.c_str() );
		UpdateClipDetection->Bind( "@DetectionVersion", CURRENT_DETECTION_VERSION );
		UpdateClipDetection->Bind( "@Lighting", (int)lighting );
		UpdateClipDetection->Execute( nullptr );
	}

	// Sync ClipTag junction table
	TagHelpers::SyncClipTags( Database, clipUID, tagString );

	// Pre-compute trails for this clip
	ComputeAndStoreTrails( clipUID, camera, static_cast<double>( timestamp ), static_cast<double>( timestamp ) + durationSec );

	LOG_INFO( "Reprocessed clip %lld: %s", (long long)clipUID, tagString.c_str() );
	BroadcastProgress( clipUID, "complete", totalFrames, totalFrames, queuePosition, queueTotal, tagString, (int)lighting );
}

void ClipReprocessWorker::BroadcastProgress( int64_t clipUID, const std::string& stage, int frame, int totalFrames, int queuePos, int queueTotal,
	const std::string& tags, int lighting )
{
	if( !Events )
		return;

	crow::json::wvalue ev;
	ev["clipUID"] = clipUID;
	ev["stage"] = stage;
	ev["frame"] = frame;
	ev["totalFrames"] = totalFrames;
	ev["queuePosition"] = queuePos;
	ev["queueTotal"] = queueTotal;
	if( !tags.empty() )
		ev["tags"] = tags;
	if( lighting >= 0 )
		ev["lighting"] = lighting;
	Events->Broadcast( "reprocess:progress", std::move( ev ) );
}

void ClipReprocessWorker::BackfillLighting()
{
	struct ClipInfo { int64_t ClipUID; int64_t Timestamp; int Camera; int RecordMode; };
	std::vector<ClipInfo> batch;

	{
		SQLiteDatabaseQueryInstance q( Database, "SelectClipsNeedingLighting" );
		q->Execute( [&]( const SQLiteDatabaseQuery& query ) -> bool
		{
			ClipInfo c;
			c.ClipUID = query.GetColumnValueInt64( 0 );
			c.Timestamp = query.GetColumnValueInt64( 1 );
			c.Camera = query.GetColumnValueInt( 2 );
			c.RecordMode = query.GetColumnValueInt( 3 );
			batch.push_back( c );
			return true;
		});
	}

	if( batch.empty() )
	{
		LightingBackfillComplete = true;
		LOG_INFO( "Lighting backfill complete" );
		return;
	}

	UpdateLastTimedAction( "Backfilling lighting" );
	int classified = 0;

	for( auto& clip : batch )
	{
		if( !IsIdle() )
			break;

		std::stringstream nameStream;
		nameStream << clip.Camera << "_" << ( clip.RecordMode == 1 ? "Auto" : "Manual" ) << "_" << clip.Timestamp << ".mp4";
		std::string clipPath = ( fs::path( CachePath ) / nameStream.str() ).string();

		int lighting = 0; // Unknown

		if( fs::exists( clipPath ) )
		{
			// Open clip, decode first frame, classify lighting
			AVFormatContext* fmtCtx = nullptr;
			if( avformat_open_input( &fmtCtx, clipPath.c_str(), nullptr, nullptr ) >= 0 )
			{
				if( avformat_find_stream_info( fmtCtx, nullptr ) >= 0 )
				{
					int videoIdx = -1;
					for( unsigned i = 0; i < fmtCtx->nb_streams; i++ )
					{
						if( fmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO )
						{ videoIdx = i; break; }
					}

					if( videoIdx >= 0 )
					{
						auto codecpar = fmtCtx->streams[videoIdx]->codecpar;
						auto codec = avcodec_find_decoder( codecpar->codec_id );
						if( codec )
						{
							auto codecCtx = avcodec_alloc_context3( codec );
							avcodec_parameters_to_context( codecCtx, codecpar );
							if( avcodec_open2( codecCtx, codec, nullptr ) >= 0 )
							{
								SwsContext* swsCtx = sws_getContext(
									codecCtx->width, codecCtx->height, codecCtx->pix_fmt,
									codecCtx->width, codecCtx->height, AV_PIX_FMT_BGR24,
									SWS_BILINEAR, nullptr, nullptr, nullptr );

								if( swsCtx )
								{
									AVPacket* pkt = av_packet_alloc();
									AVFrame* frame = av_frame_alloc();
									AVFrame* bgrFrame = av_frame_alloc();
									int w = codecCtx->width, h = codecCtx->height;
									int bufSize = av_image_get_buffer_size( AV_PIX_FMT_BGR24, w, h, 1 );
									std::vector<uint8_t> buf( bufSize );
									av_image_fill_arrays( bgrFrame->data, bgrFrame->linesize, buf.data(), AV_PIX_FMT_BGR24, w, h, 1 );

									// Decode first video frame
									while( av_read_frame( fmtCtx, pkt ) >= 0 )
									{
										if( pkt->stream_index == videoIdx )
										{
											avcodec_send_packet( codecCtx, pkt );
											if( avcodec_receive_frame( codecCtx, frame ) == 0 )
											{
												sws_scale( swsCtx, frame->data, frame->linesize, 0, h,
													bgrFrame->data, bgrFrame->linesize );
												cv::Mat mat( h, w, CV_8UC3, bgrFrame->data[0], bgrFrame->linesize[0] );
												lighting = (int)Witness::Camera::ClassifyLighting( mat );
												av_packet_unref( pkt );
												break;
											}
										}
										av_packet_unref( pkt );
									}

									av_frame_free( &bgrFrame );
									av_frame_free( &frame );
									av_packet_free( &pkt );
									sws_freeContext( swsCtx );
								}
							}
							avcodec_free_context( &codecCtx );
						}
					}
				}
				avformat_close_input( &fmtCtx );
			}
		}

		// Update just the lighting column
		{
			SQLiteDatabaseQueryInstance q( Database, "UpdateClipLighting" );
			q->Bind( "@ClipUID", clip.ClipUID );
			q->Bind( "@Lighting", lighting );
			q->Execute( nullptr );
		}
		classified++;
	}

	LOG_INFO( "Lighting backfill: classified %d clips this batch", classified );
}

// Ramer-Douglas-Peucker line simplification on normalized coordinates
static std::vector<std::array<double,3>> SimplifyRDP( const std::vector<std::array<double,3>>& pts, double epsilon )
{
	if( pts.size() <= 2 )
		return pts;

	double maxDist = 0;
	size_t maxIdx = 0;
	const auto& a = pts.front();
	const auto& b = pts.back();
	double dx = b[0] - a[0], dy = b[1] - a[1];
	double lenSq = dx * dx + dy * dy;

	for( size_t i = 1; i < pts.size() - 1; i++ )
	{
		double dist;
		if( lenSq == 0 )
		{
			dist = std::sqrt( std::pow( pts[i][0] - a[0], 2 ) + std::pow( pts[i][1] - a[1], 2 ) );
		}
		else
		{
			double t = std::max( 0.0, std::min( 1.0, ( ( pts[i][0] - a[0] ) * dx + ( pts[i][1] - a[1] ) * dy ) / lenSq ) );
			double px = a[0] + t * dx, py = a[1] + t * dy;
			dist = std::sqrt( std::pow( pts[i][0] - px, 2 ) + std::pow( pts[i][1] - py, 2 ) );
		}
		if( dist > maxDist )
		{
			maxDist = dist;
			maxIdx = i;
		}
	}

	if( maxDist > epsilon )
	{
		auto left = SimplifyRDP( { pts.begin(), pts.begin() + maxIdx + 1 }, epsilon );
		auto right = SimplifyRDP( { pts.begin() + maxIdx, pts.end() }, epsilon );
		left.pop_back();
		left.insert( left.end(), right.begin(), right.end() );
		return left;
	}
	return { a, b };
}

void ClipReprocessWorker::ComputeAndStoreTrails( int64_t clipUID, int cameraID, double fromTime, double toTime )
{
	// Delete old trails for this clip
	{
		SQLiteDatabaseQueryInstance q( Database, "DeleteTrailsForClip" );
		q->Bind( "@ClipUID", clipUID );
		q->Execute( nullptr );
	}

	// Fetch all detection boxes for this clip's time range
	struct TrailPoint { double x, y, timestamp; };
	struct Trail
	{
		std::string className;
		std::string faceName;
		std::vector<TrailPoint> points;
	};
	std::map<std::string, Trail> trailMap;

	{
		SQLiteDatabaseQueryInstance query( Database, "SelectDetectionFramesWithBoxes" );
		query->Bind( "@CameraID", cameraID );
		query->Bind( "@TimestampFrom", fromTime );
		query->Bind( "@TimestampTo", toTime );

		query->Execute( [&]( const SQLiteDatabaseQuery& q ) -> bool
		{
			const char* className = q.GetColumnValueText( 6 );
			if( !className )
				return true;

			int trackingId = q.GetColumnValueInt( 4 );
			double timestamp = q.GetColumnValueDouble( 1 );
			double x = q.GetColumnValueDouble( 8 );
			double y = q.GetColumnValueDouble( 9 );
			double w = q.GetColumnValueDouble( 10 );
			double h = q.GetColumnValueDouble( 11 );

			// Bottom-center anchor
			double ax = x + w / 2.0;
			double ay = y + h;

			std::string key = std::to_string( trackingId ) + ":" + className;
			auto& trail = trailMap[key];
			if( trail.className.empty() )
				trail.className = className;

			const char* faceName = q.GetColumnValueText( 14 );
			if( faceName && trail.faceName.empty() )
				trail.faceName = faceName;

			trail.points.push_back( { ax, ay, timestamp } );
			return true;
		});
	}

	// Split on discontinuities and store (all points preserved for client-side frame scrubbing)
	constexpr double MAX_TIME_GAP = 10.0;
	constexpr double MAX_SPATIAL_JUMP = 0.50;

	for( auto& [key, trail] : trailMap )
	{
		if( trail.points.size() < 2 )
			continue;

		std::sort( trail.points.begin(), trail.points.end(),
			[]( const TrailPoint& a, const TrailPoint& b ) { return a.timestamp < b.timestamp; } );

		// 90th percentile outlier filtering — remove spikey jumps
		if( trail.points.size() >= 4 )
		{
			std::vector<double> dists;
			dists.reserve( trail.points.size() - 1 );
			for( size_t i = 1; i < trail.points.size(); i++ )
			{
				double dist = std::sqrt( std::pow( trail.points[i].x - trail.points[i-1].x, 2 ) +
										 std::pow( trail.points[i].y - trail.points[i-1].y, 2 ) );
				dists.push_back( dist );
			}
			std::vector<double> sorted = dists;
			std::sort( sorted.begin(), sorted.end() );
			double p90 = sorted[static_cast<size_t>( sorted.size() * 0.9 )];
			double threshold = std::max( p90 * 2.5, 0.01 );

			std::vector<TrailPoint> filtered;
			filtered.push_back( trail.points[0] );
			for( size_t i = 1; i < trail.points.size(); i++ )
			{
				if( dists[i-1] <= threshold )
					filtered.push_back( trail.points[i] );
			}
			trail.points = std::move( filtered );
			if( trail.points.size() < 2 )
				continue;
		}

		// Split into segments on discontinuities
		std::vector<std::vector<TrailPoint>> segments;
		std::vector<TrailPoint> current;
		current.push_back( trail.points[0] );

		for( size_t i = 1; i < trail.points.size(); i++ )
		{
			const auto& prev = trail.points[i - 1];
			const auto& curr = trail.points[i];
			double dt = curr.timestamp - prev.timestamp;
			double dist = std::sqrt( std::pow( curr.x - prev.x, 2 ) + std::pow( curr.y - prev.y, 2 ) );

			if( dt > MAX_TIME_GAP || dist > MAX_SPATIAL_JUMP )
			{
				if( current.size() >= 2 )
					segments.push_back( std::move( current ) );
				current.clear();
			}
			current.push_back( curr );
		}
		if( current.size() >= 2 )
			segments.push_back( std::move( current ) );

		// Store each segment with all points (no RDP — client needs full granularity for frame scrubbing)
		for( auto& seg : segments )
		{
			if( seg.size() < 2 )
				continue;

			// Build compact JSON: [[x,y,t],[x,y,t],...]
			std::ostringstream json;
			json << std::fixed << std::setprecision( 6 ) << "[";
			for( size_t i = 0; i < seg.size(); i++ )
			{
				if( i > 0 ) json << ",";
				json << "[" << seg[i].x << "," << seg[i].y << ","
					 << std::setprecision( 2 ) << seg[i].timestamp << std::setprecision( 6 ) << "]";
			}
			json << "]";

			double startTime = seg.front().timestamp;
			double endTime = seg.back().timestamp;

			SQLiteDatabaseQueryInstance q( Database, "InsertTrail" );
			q->Bind( "@ClipUID", clipUID );
			q->Bind( "@CameraID", cameraID );
			q->Bind( "@ClassName", trail.className.c_str() );
			if( !trail.faceName.empty() )
				q->Bind( "@FaceName", trail.faceName.c_str() );
			else
				q->BindNull( "@FaceName" );
			q->Bind( "@StartTime", startTime );
			q->Bind( "@EndTime", endTime );
			q->Bind( "@PointData", json.str().c_str() );
			q->Execute( nullptr );
		}
	}
}
