var VIEW_MODE_NONE = 0;
var VIEW_MODE_CLIPS = 1;
var VIEW_MODE_STREAM = 2;
var VIEW_MODE_ADMIN = 100;

var anchorClickDelay = 100;

var WitnessViewModel = function() {
	"use strict";
	
	var self = this;
	
	self.authentication = new AuthenticationViewModel( self );
	self.authentication.queryUserProfile();

	self.cameraController = new CameraControllerViewModel( self );
		
	self.clipBrowser = ko.observable(null);
	self.adminController = ko.observable( self.authentication.admin() ? new AdministrationViewModel() : null );
		
	self.cameraListReceived = ko.observable(false);
	self.cameras = ko.observableArray([]);
	self.cameras.extend({ rateLimit: 50 });
	self.focusedCamera = ko.observable(null);
	
	self.viewMode = ko.observable(VIEW_MODE_NONE);
	self.isViewMode = function(modeToCheck) { return self.viewMode() == modeToCheck; }
		
	self.ready = ko.computed( function() {
		var isReady = self.authentication.ready()
				   && self.cameraListReceived();
						
		setTimeout( self.onFinishdRender, anchorClickDelay );
						
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
	
	self.refreshCameraData = function() {
		$.ajax({
			method: 'GET',
			url: '/camera/enum',
			contentType: 'application/json; charset=utf-8',
		} )
		.done( function( result ) {
			
			for( var camera = 0; camera < result.length; camera++ ) {
				
				var newCameraID = result[camera].id;
				var newCameraName = result[camera].name;
				var newCameraRecording = result[camera].recording;
								
				var found = false;
				for( var existingCamera = 0; existingCamera < self.cameras().length; existingCamera++ )
				{
					if( self.cameras()[ existingCamera ].cameraID() == newCameraID ) {
						self.cameras()[ existingCamera ].cameraName( newCameraName );
						self.cameras()[ existingCamera ].isRecording( newCameraRecording );
						found = true;
					}
				}
				
				if( !found )
				{
					self.cameras.push(  new CameraViewModel( self, newCameraID, newCameraName, newCameraRecording ) );
				}
				
				self.cameras.sort( function( left, right ) {
					return left.cameraID() < right.cameraID();
				} );
			}
			
			self.cameraListReceived( true );
		} )
		.fail( function( result ) {
			if( result.status == 401 || result.status == 403 ) {
				window.location.replace( "/" );
				return;
			}
			$.toast( {
				text: "Error fetching camera list.",
				type: 'danger',
				bgColor: '#a94442',
				position: 'top-center'
			} );
			
			self.cameraListReceived( true );
		} );
	};
	self.refreshCameraData();
	
	window.setInterval( function() {
		self.refreshCameraData();
	}, 1000 );
};