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

var CameraViewModel = function( parent, cameraID, cameraName ) {
	"use strict";
	
	var self = this;
	
	self.parent = parent;
	
	self.cameraName = ko.observable( cameraName );
	self.cameraID = ko.observable( cameraID );
	self.cameraPath = ko.observable('');
	self.isSelected = ko.observable(cameraID == 0);
	
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
	
	self.setNextCameraFrame();
	/*window.setInterval( function() {
		self.setNextCameraFrame();		
	}, 20 );*/
};

var WitnessViewModel = function() {
	"use strict";
	
	var self = this;
	
	self.authentication = new AuthenticationViewModel( self );
		
	self.cameraListReceived = ko.observable(false);
	self.cameras = ko.observableArray([]);
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
	
	$.ajax({
		method: 'GET',
		url: '/camera/enum',
		contentType: 'application/json; charset=utf-8',
	} )
	.done( function( result ) {
		
		for( var camera = 0; camera < result.length; camera++ ) {
			self.cameras.push( new CameraViewModel( self, result[camera].id, result[camera].name ) );
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
	
var g_viewModel = null;

$(document).ready(function() {
	g_viewModel = new WitnessViewModel();
	
	ko.applyBindings(g_viewModel);
});