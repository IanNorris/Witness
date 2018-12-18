import 'package:Witness/login.dart';
import 'package:Witness/splash.dart';
import 'package:fluro/fluro.dart';
import 'package:flutter/material.dart';

var splashRouteHandler = new Handler(
  handlerFunc: (BuildContext context, Map<String, List<String>> params){
    return Splash();
  }
);

var loginRouteHandler = new Handler(
  handlerFunc: (BuildContext context, Map<String, List<String>> params){
    //String username = params['username']?.first;
    //https://github.com/theyakka/fluro/blob/master/example/lib/config/route_handlers.dart
    return Login();
  }
);