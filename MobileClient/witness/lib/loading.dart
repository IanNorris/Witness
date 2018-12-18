import 'package:Witness/logoWidget.dart';
import 'package:flutter/material.dart';

class WidgetUtilities{
  static Widget createLoadingWidget( String message ){
    return Material( child: Container(
            padding: const EdgeInsets.all(32.0),
            child: Center(
                child: Column(
                    mainAxisAlignment: MainAxisAlignment.center,
                    crossAxisAlignment: CrossAxisAlignment.center,
                    children: <Widget>[
                       Hero( tag: 'logo', child: LogoWidget() ),
                        Padding(padding: const EdgeInsets.only(bottom: 64.0)),
                        Hero( tag: 'progress', child: CircularProgressIndicator() ),
                        Padding(padding: const EdgeInsets.only(bottom: 64.0)),
                        Hero( tag: 'status', child: Text(message) ),
                    ]
                  )
                )));
  }
}