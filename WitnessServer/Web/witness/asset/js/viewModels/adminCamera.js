var AdminCameraViewModel = function( cameraName, cameraPath, cameraGroups, cameraStatus ) {
	"use strict";
	
	var self = this;	

	self.cameraName = ko.observable(cameraName);
	self.cameraPath = ko.observable(cameraPath);
	self.cameraGroups = ko.observable(cameraGroups);
	self.cameraStatus = ko.observable(cameraStatus);
};