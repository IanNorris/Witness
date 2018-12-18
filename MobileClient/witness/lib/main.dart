import 'package:Witness/home.dart';
import 'package:Witness/login.dart';
import 'package:Witness/routes.dart';
import 'package:Witness/sessionInfo.dart';
import 'package:Witness/splash.dart';
import 'package:flutter/material.dart';
import 'package:fluro/fluro.dart';

class Application{
  static Router router;
}

void main() => runApp(
  SessionDataContainer( 
      child: WitnessApp()));

class WitnessApp extends StatefulWidget{
  @override
  State createState() {
    return new WitnessAppState();
  }
}

class WitnessAppState extends State<WitnessApp> {

  WitnessAppState(){
    final router = new Router();
    Routes.configureRoutes(router);
    Application.router = router;
  }

  @override
  Widget build(BuildContext context) {
      return MaterialApp(
        title: 'Witness',
        theme: ThemeData(
          primarySwatch: Colors.blue,
        ),
        initialRoute: '/',
        routes: {
          '/': (context) => Splash(),
          '/Home': (context) => Login(),
          '/Login': (context) => Login()
        }        
        //onGenerateRoute: Application.router.generator,
        );
  }
}
