var CameraViewModel = function( witness, id, enabled, name, description, connectionString, groups, status, cameraRecording, allData ) {
	"use strict";
	
	var self = this;
	
	self.witness = witness;
	self.parent = parent;
	
	self.id = id;
	self.enabled = ko.observable(enabled);
	self.name = ko.observable(name);
	self.description = ko.observable(description);
	self.previewPath = ko.observable('');
	self.connectionString = ko.observable(connectionString);
	self.status = ko.observable(status);
	self.isSelected = ko.observable(id == 0);
	self.isRecording = ko.observable(cameraRecording);
	self.groups = ko.observableArray(groups);
	
	self.lastTimestamp = ko.observable(allData.lastTimestamp);
	self.statFrameCount = ko.observable(allData.frameCount).extend({numeric: 1});
	self.statProcessingTimeMS = ko.observable(allData.processingTimeMS).extend({numeric: 1});
	self.statScaleProcessingTimeMS = ko.observable(allData.scaleProcessingTimeMS).extend({numeric: 1});
	self.statMotionDetectionProcessingTimeMS = ko.observable(allData.motionDetectionProcessingTimeMS).extend({numeric: 1});
	self.statSecondPassProcessingTimeMS = ko.observable(allData.secondPassProcessingTimeMS).extend({numeric: 1});
	self.statStreamReadTimeMS = ko.observable(allData.streamReadTimeMS).extend({numeric: 1});
	self.statStreamDecodeTimeMS = ko.observable(allData.streamDecodeTimeMS).extend({numeric: 1});
	self.statStreamOutputTimeMS = ko.observable(allData.streamOutputTimeMS).extend({numeric: 1});
	
	self.statActualTotalMS = ko.computed( function() {
		return self.statProcessingTimeMS() + self.statStreamDecodeTimeMS() + self.statStreamOutputTimeMS();
	} ).extend({numeric: 1});
	
	self.isSelectedClip = ko.computed( function() {
		return self.isSelected() && self.witness.isViewMode(VIEW_MODE_CLIPS);
	} );
	
	self.isSelectedStream = ko.computed( function() {
		return self.isSelected() && self.witness.isViewMode(VIEW_MODE_STREAM);
	} );
	
	self.streamName = ko.computed( function() {
		return "Stream_" + self.id;
	} );
	
	self.clipName = ko.computed( function() {
		return "Clip_" + self.id;
	} );
		
	self.frameIndex = 0;
	
	
	
	self.setNextCameraFrame = function() {
		self.previewPath( '/camera/preview/' + self.id + '#' + self.frameIndex );
		self.frameIndex++;
	};
	
	self.selectCamera = function() {
		var cameras = self.witness.cameras();
		for( var c = 0; c < cameras.length; c++ ) {
			cameras[c].isSelected(false);
		}
		self.isSelected(true);
		return self.id;
	};
	
	self.selectCameraStream = function() {
		var cameraID = self.selectCamera();
		self.witness.viewMode(VIEW_MODE_STREAM);
		window.location.hash = "#" + self.streamName();
	};
	
	self.selectCameraClips = function() {
		var cameraID = self.selectCamera();
		self.witness.viewMode(VIEW_MODE_CLIPS);
		self.witness.clipBrowser( new CameraClipsViewModel( self.witness, cameraID ) );
		window.location.hash = "#" + self.clipName();
	};
	
	self.toggleRecording = function() {
		self.isRecording( !self.isRecording() );
		
		var data = { 
			'csrf': self.witness.authentication.csrfToken(),
			'record': self.isRecording()
		};
		
		makeQuery( data, '/camera/record/' + self.id, true, "warning|Error while toggling camera recording.", function(result) {} );
	}
	
	self.setNextCameraFrame();
	window.setInterval( function() {
		self.setNextCameraFrame();		
	}, 250 );
};