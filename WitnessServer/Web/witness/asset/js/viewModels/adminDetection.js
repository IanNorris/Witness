var AdminDetectionViewModel = function( authentication ) {
	"use strict";
	
	var self = this;
	self.authentication = authentication;
	
	self.detectionBackend = ko.observable('');
	self.detectionProvider = ko.observable('cpu');
	self.detectionConfidence = ko.observable('0.6');
	self.detectionMaxFPS = ko.observable('10');
	self.cudnnPath = ko.observable('');
	self.clipCleanupEnabled = ko.observable('false');
	self.clipRetentionDays = ko.observable('10');
	
	self.saving = ko.observable(false);
	self.saved = ko.observable(false);
	self.testingCuda = ko.observable(false);
	self.cudaTestResult = ko.observable(null);
	
	self.refresh = function() {
		makeQuery( null, '/api/setup/settings', true, "error|Error fetching detection settings.",
			function(result) {
				if( result.detection_backend )    self.detectionBackend( result.detection_backend );
				if( result.detection_provider )   self.detectionProvider( result.detection_provider );
				if( result.detection_confidence ) self.detectionConfidence( result.detection_confidence );
				if( result.detection_max_fps )    self.detectionMaxFPS( result.detection_max_fps );
				if( result.cudnn_path )           self.cudnnPath( result.cudnn_path );
				if( result.clip_cleanup_enabled ) self.clipCleanupEnabled( result.clip_cleanup_enabled );
				if( result.clip_retention_days )  self.clipRetentionDays( result.clip_retention_days );
				self.saved(false);
			}
		);
	};
	
	self.save = function() {
		self.saving(true);
		self.saved(false);
		
		var data = {
			'csrf': self.authentication.csrfToken(),
			detection_backend: self.detectionBackend(),
			detection_provider: self.detectionProvider(),
			detection_confidence: self.detectionConfidence(),
			detection_max_fps: self.detectionMaxFPS(),
			cudnn_path: self.cudnnPath(),
			clip_cleanup_enabled: self.clipCleanupEnabled(),
			clip_retention_days: self.clipRetentionDays()
		};
		
		makeQuery( data, '/api/setup/apply', true, "error|Error saving detection settings.",
			function(result) {
				self.saved(true);
			},
			function(result) {
				self.saving(false);
			}
		);
	};
	
	self.testCuda = function() {
		self.testingCuda(true);
		self.cudaTestResult(null);
		
		var data = {
			'csrf': self.authentication.csrfToken(),
			cudnn_path: self.cudnnPath()
		};
		
		makeQuery( data, '/api/setup/test-cuda', false, "error|Error testing CUDA.",
			function(result) {
				self.cudaTestResult( result.success === true );
				self.testingCuda(false);
			},
			function() {
				self.testingCuda(false);
			}
		);
	};
	
	self.adminAction = function() {
		self.refresh();
	};
};
