var CameraDetailViewModel = function( cameraName, cameraPath, cameraGroups, cameraStatus ) {
	"use strict";
	
	var self = this;	

	self.cameraName = ko.observable(cameraName);
	self.cameraPath = ko.observable(cameraPath);
	self.cameraGroups = ko.observable(cameraGroups);
	self.cameraStatus = ko.observable(cameraStatus);
	self.availableGroups = [
		new GroupViewModel( "Public", 0 ),
		new GroupViewModel( "Inside", 1 ),
		new GroupViewModel( "Front", 2 ),
		new GroupViewModel( "Back", 3 )
	];
};