import 'package:Witness/loading.dart';
import 'package:Witness/logoWidget.dart';
import 'package:flutter/material.dart';
import 'package:Witness/sessionInfo.dart';

class Splash extends StatefulWidget {
  @override
  SplashState createState() {
    return new SplashState();
  }
}

class SplashState extends State<Splash> {
  Future<Null> processSessionData() async {
    SessionData data = new SessionData();
    await data.loadSessionData();

    if (data.sessionToken != null) {
      Navigator.of(context).pushNamed('/Home');
    } else {
      Navigator.of(context).pushNamed('/Login');
    }
  }

  @override
  void initState() {
    super.initState();
    processSessionData();
  }

  Widget build(BuildContext context) {
    return WidgetUtilities.createLoadingWidget('Loading...');
  }
}
