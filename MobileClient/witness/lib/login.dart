import 'package:Witness/logoWidget.dart';
import 'package:Witness/signIn.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

class LoginData {
  String hostname;
  int port;
  String username;
  String password;

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

  void signIn(BuildContext context) {
    if( _formKey.currentState.validate() ){
      int port = 0;
      try{
        port = int.parse(_controllerPort.text);
      } catch( FormatException )
      {
        return;
      }
      LoginData loginData = LoginData(
          _controllerHostname.text,
          port,
          _controllerUsername.text,
          _controllerPassword.text
        );
      Navigator.pushReplacement(context, MaterialPageRoute(
        builder: (context) => SignInPage( Key('SignIn'), loginData ) )
      );
    }
  }

  Widget build(BuildContext context) {

    

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
                  Hero( tag: 'logo', child: LogoWidget() ),
                  Padding(padding: const EdgeInsets.only(bottom: 32.0)),
                  Row(
                      crossAxisAlignment: CrossAxisAlignment.start,
                      mainAxisSize: MainAxisSize.min,
                      children: <Widget>[
                        Expanded(
                            child: TextFormField(
                              decoration: InputDecoration(labelText: 'Hostname'),
                              keyboardType: TextInputType.url,
                              controller: _controllerHostname
                            ),
                            flex: 4
                        ),
                        Expanded(
                            child: TextFormField(
                                decoration: InputDecoration(labelText: 'Port'),
                                inputFormatters: [ WhitelistingTextInputFormatter.digitsOnly ],
                                autovalidate: false,
                                keyboardType: TextInputType.number,
                                controller: _controllerPort,
                                validator: (value) {
                                  try{
                                      int.parse(_controllerPort.text);
                                      return null;
                                    } catch( FormatException )
                                    {
                                      return 'Invalid port';
                                    }
                                },
                            ),
                            flex: 1)
                      ]),
                  TextFormField(
                      decoration: InputDecoration(labelText: 'Username'),
                      controller: _controllerUsername),
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