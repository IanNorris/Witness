import 'package:json_annotation/json_annotation.dart';

part 'WitnessCamera.g.dart';

@JsonSerializable()
class WitnessCamera{

  WitnessCamera(
    this.id, this.name, this.description, this.enabled, this.connectionString, this.groups, 
    this.status, this.recording, this.lastTimestamp, this.frameCount, this.processingTimeMS, 
    this.scaleProcessingTimeMS, this.motionDetectionProcessingTimeMS, this.secondPassProcessingTimeMS,
    this.streamReadTimeMS, this.streamDecodeTimeMS, this.streamOutputTimeMS
  );

  int id;
  String name;
  String description;
  int enabled;
  String connectionString;
  List<int> groups;
  int status;
  int recording;
  int lastTimestamp;
  int frameCount;
  num processingTimeMS;
  num scaleProcessingTimeMS;
  num motionDetectionProcessingTimeMS;
  num secondPassProcessingTimeMS;
  num streamReadTimeMS;
  num streamDecodeTimeMS;
  num streamOutputTimeMS;

  factory WitnessCamera.fromJson(Map<String, dynamic> json) => _$WitnessCameraFromJson(json);
  Map<String, dynamic> toJson() => _$WitnessCameraToJson(this);
}