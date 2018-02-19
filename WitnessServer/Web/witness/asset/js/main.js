var anchorClickDelay = 100;

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

var ClipViewModel = function( parent, newTimestamp, newCameraID, newMotionTimestamp, newActiveDuration, newDuration, newRecordMode, newMaxMotion, newDescription ) {
	"use strict";
	
	var self = this;
		
	self.parent = parent;
	
	self.timestamp = ko.observable(newTimestamp);
	self.cameraID = ko.observable(newCameraID);
	self.motionTimestamp = ko.observable(newMotionTimestamp);
	self.activeDuration = ko.observable(newActiveDuration);
	self.duration = ko.observable(newDuration);
	self.recordMode = ko.observable(newRecordMode);
	self.maxMotion = ko.observable(newMaxMotion);
	self.description = ko.observable(newDescription);
	
	self.thumbnail = ko.computed( function() {
		return "/clip/thumb/" + self.cameraID() + "/" + self.timestamp();
	} );
	
	self.video = ko.computed( function() {
		return "/clip/video/" + self.cameraID() + "/" + self.timestamp();
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

var CameraClipsViewModel = function( parent, cameraID ) {
	"use strict";
	
	var self = this;
		
	self.parent = parent;
	self.cameraID = ko.observable(cameraID);
	
	self.totalClipsInRange = ko.observable(0);
	
	self.maxCount = ko.observable(5);
	//self.startDate = ko.observable(0);
	//self.rangePeriod = ko.observable(24 * 60 * 60);
	self.rangePeriod = ko.observable(9999999999);
	self.page = ko.observable(0);
	
	self.clips = ko.observableArray([]);
	
	self.clipPageStart = ko.computed( function() {
		return self.page() * self.maxCount();
	} );
	
	self.clipCount = ko.computed( function() {
		return self.clips().length;
	} );
	
	self.clipPageEnd = ko.computed( function() {
		return self.clipPageStart() + self.clipCount();
	} );
	
	self.totalPages = ko.computed( function() {
		return self.totalClipsInRange() / self.maxCount();
	} );
	
	self.incrementPage = function() {
		self.page( self.page() + 1 );
		self.refreshClipData();
	};
	
	self.decrementPage = function() {
		self.page( self.page() - 1 );
		self.refreshClipData();
	};
	
	self.canIncrementPage = ko.computed( function() {
		return (self.page()+1) < self.totalPages();
	} );
	
	self.canDecrementPage = ko.computed( function() {
		return (self.page()-1) >= 0;
	} );
	
	self.refreshClipData = function() {
		var timeNow = ((moment().utc() / 1000) - 1).toFixed(0);
		$.ajax({
			method: 'GET',
			url: '/clip/enum/' + self.cameraID() + '/' + self.maxCount() + '/' + timeNow + '/' + self.rangePeriod() + '/' + self.page(),
			contentType: 'application/json; charset=utf-8',
		} )
		.done( function( result ) {
			
			self.totalClipsInRange( result.count );
			
			self.clips.remove( function( item ) {
				var found = false;
				for( var clip = 0; clip < result.clips.length; clip++ ) {
					if( item.timestamp() == result.clips[clip].timestamp ) {
						found = true;
						break;
					}
				}
				
				return !found;
			} );
			
			for( var clip = 0; clip < result.clips.length; clip++ ) {
				
				var newTimestamp = result.clips[clip].timestamp;
				var newCameraID = result.clips[clip].cameraID;
				var newMotionTimestamp = result.clips[clip].motionTimestamp;
				var newActiveDuration = result.clips[clip].activeDuration;
				var newDuration = result.clips[clip].duration;
				var newRecordMode = result.clips[clip].recordMode;
				var newMaxMotion = result.clips[clip].maxMotion;
				var newDescription = result.clips[clip].description;

				var existing = null;
				
				for( var existingClip = 0; existingClip < self.clips().length; existingClip++ )
				{
					if( self.clips()[ existingClip ].timestamp() == newTimestamp ) {
						existing = self.clips()[ existingClip ];
						break;
					}
				}
				
				if( existing ){
					existing.cameraID(newCameraID);
					existing.motionTimestamp(newMotionTimestamp);
					existing.activeDuration(newActiveDuration);
					existing.duration(newDuration);
					existing.recordMode(newRecordMode);
					existing.maxMotion(newMaxMotion);
					existing.description(newDescription);
				}
				else {
					self.clips.push(  new ClipViewModel( self, newTimestamp, newCameraID, newMotionTimestamp, newActiveDuration, newDuration, newRecordMode, newMaxMotion, newDescription ) );
				}
				
				self.clips.sort( function( left, right ) {
					return left.timestamp() < right.timestamp();
				} );
			}
		} )
		.fail( function( result ) {
			$.toast( {
				text: "Error fetching clip list.",
				type: 'danger',
				bgColor: '#a94442',
				position: 'top-center'
			} );
		} );
	};
	self.refreshClipData();
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
		self.parent.isViewingClips(false);
		window.location.hash = "#" + self.streamName();
	};
	
	self.selectCameraClips = function() {
		var cameraID = self.selectCamera();
		self.parent.isViewingClips(true);
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
	
	self.clipBrowser = ko.observable(null);
		
	self.cameraListReceived = ko.observable(false);
	self.cameras = ko.observableArray([]);
	self.cameras.extend({ rateLimit: 50 });
	self.focusedCamera = ko.observable(null);
	
	self.isViewingClips = ko.observable(true);
	self.isViewingStream = ko.computed( function() { return !self.isViewingClips(); } );
		
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