import 'package:json_annotation/json_annotation.dart';

part 'WitnessLogin.g.dart';

@JsonSerializable()
class WitnessLogin{

  WitnessLogin(this.username, this.password);

  String username;
  String password;

  factory WitnessLogin.fromJson(Map<String, dynamic> json) => _$WitnessLoginFromJson(json);
  Map<String, dynamic> toJson() => _$WitnessLoginToJson(this);
}