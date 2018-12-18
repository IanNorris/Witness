// GENERATED CODE - DO NOT MODIFY BY HAND

part of 'WitnessProfile.dart';

// **************************************************************************
// JsonSerializableGenerator
// **************************************************************************

WitnessProfile _$WitnessProfileFromJson(Map<String, dynamic> json) {
  return WitnessProfile(
      json['csrf'] as String,
      json['username'] as String,
      json['userUid'] as int,
      json['admin'] as int,
      json['displayName'] as String);
}

Map<String, dynamic> _$WitnessProfileToJson(WitnessProfile instance) =>
    <String, dynamic>{
      'csrf': instance.csrf,
      'username': instance.username,
      'userUid': instance.userUid,
      'admin': instance.admin,
      'displayName': instance.displayName
    };
