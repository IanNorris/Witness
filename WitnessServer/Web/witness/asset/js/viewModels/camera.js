var CameraViewModel = function( parent, cameraID, cameraName, cameraRecording ) {
	"use strict";
	
	var self = this;
	
	self.parent = parent;
	
	self.cameraName = ko.observable( cameraName );
	self.cameraID = ko.observable( cameraID );
	self.cameraPath = ko.observable('');
	self.isSelected = ko.observable(cameraID == 0);
	self.isRecording = ko.observable(cameraRecording);
	
	self.isSelectedClip = ko.computed( function() {
		return self.isSelected() && self.parent.isViewMode(VIEW_MODE_CLIPS);
	} );
	
	self.isSelectedStream = ko.computed( function() {
		return self.isSelected() && self.parent.isViewMode(VIEW_MODE_STREAM);
	} );
	
	self.streamName = ko.computed( function() {
		return "Stream_" + self.cameraID();
	} );
	
	self.clipName = ko.computed( function() {
		return "Clip_" + self.cameraID();
	} );
		
	self.frameIndex = 0;
	
	
	
	self.setNextCameraFrame = function() {
		self.cameraPath( '/camera/preview/' + self.cameraID() + '#' + self.frameIndex );
		self.frameIndex++;
	};
	
	self.selectCamera = function() {
		var cameras = self.parent.cameras();
		for( var c = 0; c < cameras.length; c++ ) {
			cameras[c].isSelected(false);
		}
		self.isSelected(true);
		return self.cameraID();
	};
	
	self.selectCameraStream = function() {
		var cameraID = self.selectCamera();
		self.parent.viewMode(VIEW_MODE_STREAM);
		window.location.hash = "#" + self.streamName();
	};
	
	self.selectCameraClips = function() {
		var cameraID = self.selectCamera();
		self.parent.viewMode(VIEW_MODE_CLIPS);
		self.parent.clipBrowser( new CameraClipsViewModel( self.parent, cameraID ) );
		window.location.hash = "#" + self.clipName();
	};
	
	self.toggleRecording = function() {
		self.isRecording( !self.isRecording() );
		
		var logoutData = JSON.stringify( { 
			'csrf': self.parent.authentication.csrfToken(),
			'record': self.isRecording()
		} );
		
		$.ajax({
			method: 'POST',
			url: '/camera/record/' + self.cameraID(),
			dataType: 'json',
			data: logoutData,
			contentType: 'application/json; charset=utf-8',
		} )
		.done( function( result ) {
			//Nothing
		} )
		.fail( function( result ) {
			if( result.status == 401 || result.status == 403 ) {
				window.location.replace( "/" );
				return;
			}
			$.toast( {
				text: "Error while attempting to set recording to " + self.isRecording() + ".",
				type: 'warning',
				position: 'top-center'
			} );
		} );
	}
	
	self.setNextCameraFrame();
	window.setInterval( function() {
		self.setNextCameraFrame();		
	}, 250 );
};