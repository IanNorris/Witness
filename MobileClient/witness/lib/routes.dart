import 'package:Witness/route_handlers.dart';
import 'package:fluro/fluro.dart';
import 'package:flutter/material.dart';

class Routes {
  static void configureRoutes( Router router ){
    router.notFoundHandler = new Handler( handlerFunc:
      (BuildContext context, Map<String, List<String>> params) {
        print("Route not found.");
      });

      router.define( '/', handler: splashRouteHandler );
      router.define( '/Login', handler: loginRouteHandler);
  }
}