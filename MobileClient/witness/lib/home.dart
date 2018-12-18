import 'package:Witness/logoWidget.dart';
import 'package:Witness/sessionInfo.dart';
import 'package:flutter/material.dart';

class Home extends StatelessWidget {
  Widget build(BuildContext context) {
    var sessionData = SessionDataContainer.of(context);

    return Material( child: Container(
            padding: const EdgeInsets.all(32.0),
            child: Center(
                child: Column(
                    mainAxisAlignment: MainAxisAlignment.center,
                    crossAxisAlignment: CrossAxisAlignment.center,
                    children: [
                       Hero( tag: 'logo', child: LogoWidget() ),
                        Padding(padding: const EdgeInsets.only(bottom: 64.0)),
                        Hero( tag: 'status', child: Text('Hello ${sessionData.state.profile.displayName}!') ),
                    ]
                  )
                )));
  }
}