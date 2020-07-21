var VIEW_MODE_NONE = 0;
var VIEW_MODE_CLIPS = 1;
var VIEW_MODE_STREAM = 2;
var VIEW_MODE_ADMIN = 100;

var anchorClickDelay = 100;

var WitnessViewModel = function() {
	"use strict";
	
	var self = this;
	
	self.adminController = ko.observable(null);
	self.adminAction = function(){
		if( self.adminController() ) {
			self.viewMode(VIEW_MODE_ADMIN);
			window.location.hash = "#Administration";
			self.adminController().adminAction();
		}
	};
	
	self.authentication = new AuthenticationViewModel( self );
	self.authentication.queryUserProfile( function() {
		self.adminController( self.authentication.admin() ? new AdminViewModel( self ) : null );
	});
	
	self.clipBrowser = ko.observable(null);
	self.stream = ko.observable(null);
		
	self.cameraListReceived = ko.observable(false);
	self.cameras = ko.observableArray([]);
	self.cameras.extend({ rateLimit: 50 });
    self.focusedCamera = ko.observable(null);
	self.allClipsSelected = ko.observable(false);
	self.sentReady = ko.observable(false);
	
	self.selectAllCameraClips = function() {
		self.viewMode(VIEW_MODE_CLIPS);
		self.stream(null);
		
		var cameraList = self.cameras();
		for( var c = 0; c < cameraList.length; c++ ) {
			cameraList[c].isSelected(false);
		}
		self.allClipsSelected(true);
		
		self.clipBrowser( new CameraClipsViewModel( self.witness, self.authentication, -1 ) );
		window.location.hash = "#Clip_All";
	};
	
	self.isSelectedAllClips = ko.computed(function(){
		return self.allClipsSelected() && self.isViewMode(VIEW_MODE_CLIPS);
	});

    self.cameraPreviewScale = ko.observable( localStorage.cameraPreviewScale ? parseInt(localStorage.cameraPreviewScale) : 15 );

    self.increaseScale = function () {
        self.cameraPreviewScale( Math.min( self.cameraPreviewScale() + 5, 100 ) );
		localStorage.cameraPreviewScale = self.cameraPreviewScale();
    }

    self.decreaseScale = function () {
        self.cameraPreviewScale( Math.max( self.cameraPreviewScale() - 5, 5) );
		localStorage.cameraPreviewScale = self.cameraPreviewScale();
    }
	
	self.viewMode = ko.observable(VIEW_MODE_NONE);
	self.isViewMode = function(modeToCheck) { return self.viewMode() == modeToCheck; }
		
	self.ready = ko.computed( function() {
		var isReady = self.authentication.ready()
				   && self.cameraListReceived();

		if( isReady && !self.sentReady() ){
			setTimeout( self.onFinishdRender, anchorClickDelay );
			self.sentReady(true);
		}
		
						
		return isReady;
	} );
	
	self.notReady = ko.computed( function() {
		return !self.ready();
	} );
	
	self.onFinishdRender = function() {
		if( self.ready() ) {
			if( window.location.hash ) {
				$( ".clickable[name='" + window.location.hash.substr(1) + "']" ).click();
			}
		}
	};
	
	var first = true;
	self.refreshCameraData = function() {
		makeQuery( null, first ? '/camera/enum' : '/camera/enum_longpoll', true, "error|Error fetching camera list.",
			function( result ) {	
				for( var camera = 0; camera < result.length; camera++ ) {
					
					var newId = result[camera].id;
					var newEnabled = result[camera].enabled;
					var newName = result[camera].name;
					var newDescription = result[camera].description;
					var newRecording = result[camera].recording;
					var newRecording = result[camera].recording;
					var newStatus = result[camera].status;
									
					var found = false;
					for( var existingCamera = 0; existingCamera < self.cameras().length; existingCamera++ )
					{
						if( self.cameras()[ existingCamera ].id == newId ) {
							self.cameras()[ existingCamera ].enabled( newEnabled );
							self.cameras()[ existingCamera ].name( newName );
							self.cameras()[ existingCamera ].description( newDescription );
							self.cameras()[ existingCamera ].isRecording( newRecording );
							self.cameras()[ existingCamera ].status( newStatus );
							
							self.cameras()[ existingCamera ].lastTimestamp( result[camera].lastTimestamp );
							self.cameras()[ existingCamera ].statFrameCount( result[camera].frameCount );
							
							found = true;
						}
					}
					
					if( !found )
					{
						self.cameras.push(  new CameraViewModel( self, newId, newEnabled, newName, newDescription, '', '', [], '', newRecording, result[camera] ) );
					}
					
					self.cameras.sort( function( left, right ) {
						return left.id < right.id;
					} );
				}
				
				self.cameraListReceived( true );
		},
		function( result ) {
			first = false;
			var timer = window.setInterval( function() {
				clearInterval(timer);
				self.refreshCameraData();
			}, 100 );
		} );
	};
	self.refreshCameraData();
	
	
};