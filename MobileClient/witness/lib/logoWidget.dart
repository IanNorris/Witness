import 'package:flutter/material.dart';
import 'package:flutter_svg/flutter_svg.dart';

class LogoWidget extends StatelessWidget {
  Widget build(BuildContext context) {
    final Widget logo =
        new SvgPicture.asset('assets/logo.svg', color: Colors.blue);

    return Row(
        mainAxisSize: MainAxisSize.min,
        mainAxisAlignment: MainAxisAlignment.center,
        children: <Widget>[
          CircleAvatar(child: logo, backgroundColor: Colors.transparent),
          Padding(padding: const EdgeInsets.only(right: 16.0)),
          Text("Witness", style: TextStyle(fontSize: 48.0))
        ]);
  }
}
