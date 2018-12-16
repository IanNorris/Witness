import 'package:Witness/login.dart';
import 'package:Witness/logoWidget.dart';
import 'package:flutter/material.dart';

class SignInPage extends StatelessWidget {
  final LoginData loginData;

  void signIn() {
    
  }

  SignInPage( Key key, this.loginData ) : super(key: key);

  Widget build(BuildContext context) {
    return Material(
        child: Container(
            padding: const EdgeInsets.all(32.0),
            child: Center(
                child: Column(
                    mainAxisAlignment: MainAxisAlignment.center,
                    crossAxisAlignment: CrossAxisAlignment.center,
                    children: <Widget>[
                       Hero( tag: 'logo', child: LogoWidget() ),
                        Padding(padding: const EdgeInsets.only(bottom: 64.0)),
                        CircularProgressIndicator(),
                        Padding(padding: const EdgeInsets.only(bottom: 64.0)),
                        Text('Connecting to ${loginData.hostname}...'),
                    ]
                  )
                )));
  }
}