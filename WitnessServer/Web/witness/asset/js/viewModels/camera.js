var DebugCameraStats = [ 
	[ "processing", "Processing" ],
	[ "scale", "Scale" ],
	[ "jpegEncoding", "Jpeg Encoding" ],
	[ "observer", "Observer Filter" ],
	[ "firstPassFilter", "First Pass Filter" ],
	[ "secondPassFilter", "Second Pass Filter" ],
	[ "thirdPassFilter", "Third Pass Filter" ],
	[ "debug", "Debug" ],
	[ "mvfInternal", "MVF Internal" ],
	[ "mvfSideData", "MVF Side Data" ],
	[ "mvfVectorPass", "MVF Vector Pass" ],
	[ "mvfClusterPass", "MVF Cluster Pass" ],
	[ "mvfObjectPass", "MVF Object Pass" ]
];

var CameraViewModel = function( witness, id, enabled, name, description, connectionString, connectionStringSub, groups, status, cameraRecording, allData ) {
	"use strict";
	
	var self = this;
	
	self.witness = witness;
	self.parent = parent;
	
	self.id = id;
	self.enabled = ko.observable(enabled);
	self.name = ko.observable(name);
	self.description = ko.observable(description);
	self.previewPath = ko.observable('');
	self.livePreviewPath = ko.observable('');
	self.connectionString = ko.observable(connectionString);
	self.connectionStringSub = ko.observable(connectionStringSub);
	self.status = ko.observable(status);
	self.isSelected = ko.observable(false);
	self.isRecording = ko.observable(cameraRecording);
	self.groups = ko.observableArray(groups);
	
	self.lastTimestamp = ko.observable(allData.lastTimestamp);
	self.statFrameCount = ko.observable(allData.frameCount).extend({numeric: 1});
	
	self.stats = ko.observableArray([]);
	
	self.populateStats = function( cameraObject ) {
		self.stats.removeAll();
		for( var stat = 0; stat < DebugCameraStats.length; stat++ ) {
			self.stats.push( { 
				name: ko.observable( DebugCameraStats[stat][1] ),
				each: ko.observable( cameraObject[DebugCameraStats[stat][0]+"TimeOfEachMS"]).extend({numeric: 1}),
				total: ko.observable( cameraObject[DebugCameraStats[stat][0]+"ActualMS"]).extend({numeric: 1}),
			} );
		}
	};
	self.populateStats( allData );
	
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
	self.liveFrameIndex = 0;
	
	self.setNextCameraFrame = function() {
		self.previewPath( '/camera/preview/' + self.id + '#' + self.frameIndex );
		self.frameIndex++;
	};
	
	self.setNextCameraLiveFrame = function() {
		self.livePreviewPath( '/stream/pl/' + self.id );
		self.liveFrameIndex++;
	};
	
	self.selectCamera = function() {
		var cameras = self.witness.cameras();
		for( var c = 0; c < cameras.length; c++ ) {
			cameras[c].isSelected(false);
		}
		self.isSelected(true);
		self.witness.allClipsSelected(false);
		return self.id;
	};
	
	self.selectCameraStream = function() {
		var cameraID = self.selectCamera();
		self.witness.viewMode(VIEW_MODE_STREAM);
		self.witness.stream( new CameraStreamViewModel( self.witness, self, cameraID ) );
		self.witness.clipBrowser(null);
		window.location.hash = "#" + self.streamName();
	};
	
	self.selectCameraClips = function() {
		var cameraID = self.selectCamera();
		self.witness.viewMode(VIEW_MODE_CLIPS);
		self.witness.stream(null);
		self.witness.clipBrowser( new CameraClipsViewModel( self.witness, self.witness.authentication, cameraID ) );
		window.location.hash = "#" + self.clipName();
	};
	
	self.groups.subscribe(  function( newValue ){	
		var data = { 
			'csrf': self.witness.authentication.csrfToken(),
			camera: self.id,
			value: newValue.map(Number)
		};
		
		makeQuery( data, '/camera/set_groups/' , true, "warning|Error while setting camera groups.", function(result) {} );
		
		return true;
	} );
	
	self.toggleRecording = function() {
		self.isRecording( !self.isRecording() );
		
		var data = { 
			'csrf': self.witness.authentication.csrfToken(),
			'record': self.isRecording()
		};
		
		makeQuery( data, '/camera/record/' + self.id, true, "warning|Error while toggling camera recording.", function(result) {} );
	}
	
	self.setNextCameraFrame();
	/*window.setInterval( function() {
		self.setNextCameraFrame();		
	}, 250 );*/
	
	self.setNextCameraLiveFrame();
	/*window.setInterval( function() {
		self.setNextCameraLiveFrame();
	}, 40 );*/
};