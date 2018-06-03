var CameraControllerViewModel = function( parent ) {
	"use strict";
	
	var self = this;	
	
	self.parent = parent;
	
	self.cameraList = ko.observableArray([
		new CameraDetailViewModel( "Hello", "X:\\Path\\", [0,1,2,3], "Offline" ),
		new CameraDetailViewModel( "World", "X:\\Path2\\", [2,3], "Online" )
	]);
};