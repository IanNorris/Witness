var AdminCamerasViewModel = function( groups ) {
	"use strict";
	
	var self = this;
	self.groups = groups;
	
	self.cameraList = ko.observableArray([
		new AdminCameraViewModel( "Hello", "X:\\Path\\", [0,1,2,3], "Offline" ),
		new AdminCameraViewModel( "World", "X:\\Path2\\", [2,3], "Online" )
	]);
	
	self.adminAction = function() {
		
	};
};