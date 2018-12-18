import 'package:json_annotation/json_annotation.dart';

part 'WitnessProfile.g.dart';

@JsonSerializable()
class WitnessProfile{

  WitnessProfile(this.csrf, this.username, this.userUid, this.admin, this.displayName);

  String csrf;
  String username;
  int userUid;
  int admin;
  String displayName;

  factory WitnessProfile.fromJson(Map<String, dynamic> json) => _$WitnessProfileFromJson(json);
  Map<String, dynamic> toJson() => _$WitnessProfileToJson(this);
}