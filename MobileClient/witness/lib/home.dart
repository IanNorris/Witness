import 'package:Witness/sessionInfo.dart';
import 'package:flutter/material.dart';

class Home extends StatelessWidget {
  static const String Title = "Witness";

  String getTitle( SelectedView view ){
    switch(view.view){
      case ViewMode.Grid:
        return '$Title - Cameras';

      case ViewMode.Live:
        return '$Title - Camera ${view.cameraIndex}';

      case ViewMode.Recording:
        return '$Title - Camera ${view.cameraIndex} Recordings';
    }

    return Title;
  }

  Widget build(BuildContext context) {
    var sessionData = SessionDataContainer.of(context);

    return Scaffold( 
      drawer: new Drawer( child: new ListView(children: [
        new ListTitle(title: 'Witness'),
        new Divider(),
        new ListTitle(title: 'Live Cameras'),
        new ListTitle(title: 'Recordings'),
      ],)),
      appBar: AppBar( 
        title: Text(getTitle(sessionData.state.view)),
        actions: [
          IconButton(icon: Icon( Icons.camera_alt ), tooltip: 'Live Cameras', onPressed: (){},),
          IconButton(icon: Icon( Icons.camera_roll ), tooltip: 'Recordings', onPressed: (){},)
        ], ),
      body: Container(),
    );
  }
}