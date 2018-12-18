import 'dart:io';
import 'dart:convert';

import 'package:Witness/home.dart';
import 'package:Witness/loading.dart';
import 'package:Witness/logoWidget.dart';
import 'package:Witness/models/WitnessLogin.dart';
import 'package:Witness/sessionInfo.dart';
import 'package:Witness/models/WitnessProfile.dart';
import 'package:dio/dio.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:cookie_jar/cookie_jar.dart';

class LoginData {
  String hostname;
  int port;
  String username;
  String password;
  bool loggingIn = false;

  LoginData(this.hostname, this.port, this.username, this.password);
}

class Login extends StatefulWidget {
  @override
  LoginState createState() {
    return LoginState();
  }
}

class LoginState extends State<Login> {
  final _formKey = GlobalKey<FormState>();
  final TextEditingController _controllerHostname = new TextEditingController();
  final TextEditingController _controllerPort = new TextEditingController();
  final TextEditingController _controllerUsername = new TextEditingController();
  final TextEditingController _controllerPassword = new TextEditingController();

  @override
  void didChangeDependencies() {
    super.didChangeDependencies();

    var sessionData = SessionDataContainer.of(context);
    _controllerHostname.text = sessionData.state.loginData.hostname;
    _controllerPort.text = sessionData.state.loginData.port.toString();
    _controllerUsername.text = sessionData.state.loginData.username;

    _controllerHostname.addListener((){
      var sessionData = SessionDataContainer.of(context);
      sessionData.setState(() {
        sessionData.state.loginData.hostname = _controllerHostname.text.trim();
      });
    });

    _controllerPort.addListener((){
      var sessionData = SessionDataContainer.of(context);
      sessionData.setState(() {
        sessionData.state.loginData.port = int.parse(_controllerPort.text);
      });
    });

    _controllerUsername.addListener((){
      var sessionData = SessionDataContainer.of(context);
      sessionData.setState(() {
        sessionData.state.loginData.username = _controllerUsername.text.trim();
      });
    });

    signInAsyc(null);
  }

  Future<Null> signInAsyc(LoginData loginDataIn) async{
    var sessionData = SessionDataContainer.of(context);
    var session = sessionData.state;

    if( session.loginData.hostname == null || session.loginData.hostname.length == 0)
    {
      return null;
    }

    Uri profileUri = session.buildUri('/auth/profile');

    bool loggedIn = false;
    var sessionCookieName = "SessionToken-${session.loginData.port}";
    CookieJar cookieJar = await session.getCookieJar();
    List<Cookie> cookies = cookieJar.loadForRequest(profileUri);
    for( Cookie cookie in cookies ){
      if( cookie.name == sessionCookieName ) {
        loggedIn = true;
        sessionData.setSessionToken(cookie.value);
      }
    }

    if( loginDataIn != null && !loggedIn ){
      WitnessLogin loginData = new WitnessLogin( loginDataIn.username, loginDataIn.password );
      String jsonString = json.encode(loginData);
      Response response = await session.makePostRequest('/auth/login', jsonString);
      if( response == null || response.statusCode != 200){
        sessionData.setSessionToken(null);
        sessionData.state.loginData.loggingIn = false;
        //TODO Report the error
        return null;
      }

      List<Cookie> cookies = cookieJar.loadForRequest(profileUri);
      for( Cookie cookie in cookies ){
        if( cookie.name == sessionCookieName ) {
          loggedIn = true;
          sessionData.setSessionToken(cookie.value);
        }
      }
    }

    Response profileResponse = await session.makePostRequest('/auth/profile', "{}");
    if( profileResponse == null || profileResponse.statusCode != 200){
      setState(() {
        sessionData.setSessionToken(null);
        sessionData.state.loginData.loggingIn = false;
      });
      return null;
    }

    var profile = WitnessProfile.fromJson(profileResponse.data);

    sessionData.setProfile(profile);

    Navigator.push( context, MaterialPageRoute(builder: (context) => Home()));
  }

  void signIn(BuildContext context) {
    if (_formKey.currentState.validate()) {
      int port = 0;
      try {
        port = int.parse(_controllerPort.text.trim());
      } catch (FormatException) {
        return;
      }
      LoginData loginData = LoginData(_controllerHostname.text.trim(), port,
          _controllerUsername.text, _controllerPassword.text);

      LoginData loginDataSaved = LoginData(
          _controllerHostname.text.trim(), port, _controllerUsername.text.trim(), "");
      loginDataSaved.loggingIn = true;

      setState(() {
        var sessionData = SessionDataContainer.of(context);
        sessionData.setLoginData(loginDataSaved);
        sessionData.state.saveSessionData();
      });
      
      signInAsyc( loginData );
    }
  }

  Widget build(BuildContext context) {
    var sessionData = SessionDataContainer.of(context);

    if( !sessionData.state.loadedPrefs ){
      return WidgetUtilities.createLoadingWidget('Loading...');
    }
    else if( sessionData.state.loginData.loggingIn || sessionData.state.sessionToken != null ){
      return WidgetUtilities.createLoadingWidget('Connecting to ${sessionData.state.loginData.hostname}...');
    }
    else{
      return Material(
        child: Form(
            key: _formKey,
            child: Container(
                padding: const EdgeInsets.all(32.0),
                child: Center(
                    child: Column(
                        mainAxisAlignment: MainAxisAlignment.center,
                        crossAxisAlignment: CrossAxisAlignment.center,
                        children: <Widget>[
                      Hero(tag: 'logo', child: LogoWidget()),
                      Padding(padding: const EdgeInsets.only(bottom: 32.0)),
                      Row(
                          crossAxisAlignment: CrossAxisAlignment.start,
                          mainAxisSize: MainAxisSize.min,
                          children: <Widget>[
                            Expanded(
                                child: TextFormField(
                                  decoration:
                                      InputDecoration(labelText: 'Hostname'),
                                  keyboardType: TextInputType.url,
                                  controller: _controllerHostname,
                                  validator: (value) {
                                    return value.trim().length > 1
                                        ? null
                                        : 'Must be a valid hostname';
                                  },
                                ),
                                flex: 4),
                            Expanded(
                                child: TextFormField(
                                  decoration:
                                      InputDecoration(labelText: 'Port'),
                                  inputFormatters: [
                                    WhitelistingTextInputFormatter.digitsOnly
                                  ],
                                  autovalidate: false,
                                  keyboardType: TextInputType.number,
                                  controller: _controllerPort,
                                  validator: (value) {
                                    try {
                                      int.parse(_controllerPort.text);
                                      return null;
                                    } catch (FormatException) {
                                      return 'Invalid port';
                                    }
                                  },
                                ),
                                flex: 1)
                          ]),
                      TextFormField(
                          decoration: InputDecoration(labelText: 'Username'),
                          controller: _controllerUsername,
                          validator: (value) {
                            return value.trim().length > 1
                                ? null
                                : 'Must be a valid hostname';
                          }),
                      TextFormField(
                          decoration: InputDecoration(labelText: 'Password'),
                          obscureText: true,
                          controller: _controllerPassword),
                      Padding(padding: const EdgeInsets.only(bottom: 32.0)),
                      Row(children: <Widget>[
                        Expanded(
                            child: RaisedButton(
                                child: Text('Sign in'),
                                onPressed: () => signIn(context),
                                color: Colors.blue,
                                textColor: Colors.white))
                      ])
                    ])))));
    }
  }
}
