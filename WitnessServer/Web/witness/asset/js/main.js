var AuthenticationViewModel = function( parent ) {
	"use strict";
	
	var self = this;	
	
	self.parent = parent;
	
	self.ready = ko.observable(false);
	
	self.sessionToken = getCookie('SessionToken');
	self.csrfToken = ko.observable('');
	self.username = ko.observable('');
		
	$.ajax({
			method: 'POST',
			url: '/auth/profile',
			dataType: 'json',
			data: '{}',
			contentType: 'application/json; charset=utf-8',
		} )
		.done( function( result ) {
			
			self.csrfToken(result.csrf);
			self.username(result.username);
			
			self.ready(true);
		} )
		.fail( function( result ) {
			window.location.replace( "/" );
		} );
	
	
	
	self.logoutAction = function() {
				
		var logoutData = JSON.stringify( { 
			'csrf': self.csrfToken()
		} );
		
		$.ajax({
			method: 'POST',
			url: '/auth/logout',
			dataType: 'json',
			data: logoutData,
			contentType: 'application/json; charset=utf-8',
		} )
		.done( function( result ) {
			window.location.replace( "/" );
		} )
		.fail( function( result ) {
			$.toast( {
				text: "Error while logging out.",
				type: 'danger',
				bgColor: '#a94442',
				position: 'top-center'
			} );
		} );
	};
};

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
		return self.isSelected() && self.parent.isViewingClips();
	} );
	
	self.isSelectedStream = ko.computed( function() {
		return self.isSelected() && self.parent.isViewingStream();
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
	};
	
	self.selectCameraStream = function() {
		self.selectCamera();
		self.parent.isViewingClips(false);
	};
	
	self.selectCameraClips = function() {
		self.selectCamera();
		self.parent.isViewingClips(true);
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

var WitnessViewModel = function() {
	"use strict";
	
	var self = this;
	
	self.authentication = new AuthenticationViewModel( self );
		
	self.cameraListReceived = ko.observable(false);
	self.cameras = ko.observableArray([]);
	self.cameras.extend({ rateLimit: 50 });
	self.focusedCamera = ko.observable(null);
	
	self.isViewingClips = ko.observable(true);
	self.isViewingStream = ko.computed( function() { return !self.isViewingClips(); } );
		
	self.ready = ko.computed( function() {
		return self.authentication.ready()
			&& self.cameraListReceived();
	} );
	
	self.notReady = ko.computed( function() {
		return !self.ready();
	} );
	
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
	
var g_viewModel = null;

$(document).ready(function() {
	ko.options.deferUpdates = true;
	
	g_viewModel = new WitnessViewModel();
	
	ko.applyBindings(g_viewModel);
});