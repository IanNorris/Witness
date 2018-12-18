import 'dart:io';

import 'package:Witness/login.dart';
import 'package:Witness/models/WitnessProfile.dart';
import 'package:flutter/material.dart';
import 'package:shared_preferences/shared_preferences.dart';
import 'package:dio/dio.dart';
import 'package:cookie_jar/cookie_jar.dart';
import 'package:path_provider/path_provider.dart';

enum ViewMode{
  Grid,
  Live,
  Recording,
}

class SelectedView{
  ViewMode view = ViewMode.Grid;
  int cameraIndex = 0;
}

class SessionData{
  LoginData loginData = new LoginData('',11236,'','');
  WitnessProfile profile;
  String sessionToken;
  bool loadedPrefs = false;
  SelectedView view = new SelectedView();

  CookieJar cookies;
  SharedPreferences prefs;

  Uri buildUri(String path){
    return new Uri(scheme: "https", host: loginData.hostname, port: loginData.port, path: path);
  }

  Future<CookieJar> getCookieJar() async {
    if( cookies == null ) {
      Directory appPath = await getApplicationDocumentsDirectory();
      cookies = PersistCookieJar(appPath.path);
      return cookies;
    }
    else {
      return cookies;
    }
  }

  Future<Response> makePostRequest(String path, String jsonData) async{
    var cj = await getCookieJar();

    var uri = buildUri(path);
    var dio = new Dio();
    dio.cookieJar = cj;

    Response response;
    try{
      response = await dio.post(uri.toString(), data: jsonData);
      return response;
    } on DioError catch(e) {
      return response;
    }
  }

  Future<Response> makeGetRequest(String path) async{
    var cj = await getCookieJar();

    var uri = buildUri(path);
    var dio = new Dio();
    dio.cookieJar = cj;

    Response response;
    try{
      response = await dio.get(uri.toString());
      return response;
    } on DioError catch(e) {
      return response;
    }
  }

  Future<Null> loadSessionData() async{
    if( prefs == null ){
        prefs = await SharedPreferences.getInstance();
    }
    sessionToken = prefs.getString('sessionToken');
    loginData.hostname = prefs.getString('hostname');
    loginData.port = prefs.getInt('port');
    loginData.username = prefs.getString('username');

    loadedPrefs = true;
  }

  Future<Null> saveSessionData() async{
    if( prefs == null ){
        prefs = await SharedPreferences.getInstance();
    }

    prefs.setString('sessionToken', sessionToken);
    prefs.setString('hostname', loginData.hostname);
    prefs.setInt('port', loginData.port);
    prefs.setString('username', loginData.username);
  }
}

class SessionDataContainer extends StatefulWidget{
  final SessionData state;
  final Widget child;

  SessionDataContainer({@required this.child,this.state});

  static SessionDataContainerState of(BuildContext context) {
    return (context.inheritFromWidgetOfExactType(_InheritedSessionDataContainerState) as _InheritedSessionDataContainerState).data;
  }

  @override
  SessionDataContainerState createState() => new SessionDataContainerState();
}

class SessionDataContainerState extends State<SessionDataContainer> {
  SessionData state;

  @override 
  void initState(){
    super.initState();

    if( state == null ){
      state = new SessionData();
    }

    state.loadSessionData();
  }

  @override
  Widget build(BuildContext context) {
    return new _InheritedSessionDataContainerState(child: widget.child, data: this);
  }

  void setSessionToken( String token ){
    setState( (){
      state.sessionToken = token;
      state.saveSessionData();
    } );
  }

  void setLoginData( LoginData loginData ){
    setState( (){
      //Ensure we never save the password
      loginData.password = null;
      state.loginData = loginData;
      state.saveSessionData();
    } );
  }

  void setProfile( WitnessProfile profile ){
    setState( (){
      //Ensure we never save the password
      state.profile = profile;
      state.saveSessionData();
    } );
  }
}

class _InheritedSessionDataContainerState extends InheritedWidget{
  final SessionDataContainerState data;

  const _InheritedSessionDataContainerState({Key key, @required this.data, @required Widget child}): super(key: key, child: child);

  @override
  bool updateShouldNotify(_InheritedSessionDataContainerState oldWidget) {
    if( (data == null) || (oldWidget.data == null) || (data.state == null) || (oldWidget.data.state == null) ) {
      return true;
    }

    return data.state.sessionToken != oldWidget.data.state.sessionToken;
  }
}