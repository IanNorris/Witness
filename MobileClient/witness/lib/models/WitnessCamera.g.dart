// GENERATED CODE - DO NOT MODIFY BY HAND

part of 'WitnessCamera.dart';

// **************************************************************************
// JsonSerializableGenerator
// **************************************************************************

WitnessCamera _$WitnessCameraFromJson(Map<String, dynamic> json) {
  return WitnessCamera(
      json['id'] as int,
      json['name'] as String,
      json['description'] as String,
      json['enabled'] as int,
      json['connectionString'] as String,
      (json['groups'] as List)?.map((e) => e as int)?.toList(),
      json['status'] as int,
      json['recording'] as int,
      json['lastTimestamp'] as int,
      json['frameCount'] as int,
      json['processingTimeMS'] as num,
      json['scaleProcessingTimeMS'] as num,
      json['motionDetectionProcessingTimeMS'] as num,
      json['secondPassProcessingTimeMS'] as num,
      json['streamReadTimeMS'] as num,
      json['streamDecodeTimeMS'] as num,
      json['streamOutputTimeMS'] as num);
}

Map<String, dynamic> _$WitnessCameraToJson(WitnessCamera instance) =>
    <String, dynamic>{
      'id': instance.id,
      'name': instance.name,
      'description': instance.description,
      'enabled': instance.enabled,
      'connectionString': instance.connectionString,
      'groups': instance.groups,
      'status': instance.status,
      'recording': instance.recording,
      'lastTimestamp': instance.lastTimestamp,
      'frameCount': instance.frameCount,
      'processingTimeMS': instance.processingTimeMS,
      'scaleProcessingTimeMS': instance.scaleProcessingTimeMS,
      'motionDetectionProcessingTimeMS':
          instance.motionDetectionProcessingTimeMS,
      'secondPassProcessingTimeMS': instance.secondPassProcessingTimeMS,
      'streamReadTimeMS': instance.streamReadTimeMS,
      'streamDecodeTimeMS': instance.streamDecodeTimeMS,
      'streamOutputTimeMS': instance.streamOutputTimeMS
    };
