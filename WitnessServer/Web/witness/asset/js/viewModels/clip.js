var ClipViewModel = function( parent, newClipUID, newTimestamp, newCameraID, newMotionTimestamp, newActiveDuration, newDuration, newRecordMode, newMaxMotion, newDescription, newSaved ) {
	"use strict";
	
	var self = this;
		
	self.parent = parent;
	
	self.clipUID = ko.observable(newClipUID);
	self.timestamp = ko.observable(newTimestamp);
	self.cameraID = ko.observable(newCameraID);
	self.motionTimestamp = ko.observable(newMotionTimestamp);
	self.activeDuration = ko.observable(newActiveDuration);
	self.duration = ko.observable(newDuration);
	self.recordMode = ko.observable(newRecordMode);
	self.maxMotion = ko.observable(newMaxMotion);
	self.description = ko.observable(newDescription);
	self.saved = ko.observable(newSaved);
	
	self.recursing = false;
	
	self.toggleSaved = function(){
		self.parent.toggleSave(self.clipUID(), self.saved());
		return true;
	};
	
	self.deleteClip = function(){
		self.parent.showDeleteDialog(self.clipUID());
	};
	
	self.thumbnail = ko.computed( function() {
		return "/clip/thumb/" + self.cameraID() + "/" + self.timestamp();
	} );
	
	self.video = ko.computed( function() {
		return "/clip/video/" + self.cameraID() + "/" + self.timestamp();
	} );
	
	self.timeDesc = ko.computed( function() {
		return moment(self.timestamp() * 1000).local().format();
	} );
	
	self.agoDesc = ko.computed( function() {
		return moment(self.timestamp() * 1000).local().calendar();
	} );
	
	self.durationDesc = ko.computed( function() {
		return moment.duration(self.duration() * 1000).humanize();
	} );
	
	self.motionDesc = ko.computed( function() {
		return (self.maxMotion() * 100.0).toFixed(0) +'%';
	} );
	
	self.typeDesc = ko.computed( function() {
		return self.recordMode() == 0 ? 'Manual' : 'Motion activated';
	} );
};