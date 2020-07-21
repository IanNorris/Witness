var AdminCamerasViewModel = function( authentication, groups, witness ) {
	"use strict";
	
	var self = this;
	self.witness = witness;
	self.groups = groups;
	self.authentication = authentication;
	
	self.isBusy = ko.observable(false);
	self.idToDelete = 0;
	self.displayNameToDelete = ko.observable('');
	self.newName = ko.observable('');
	self.newDescription = ko.observable('');
	self.newHostname = ko.observable('');
	self.newUsername = ko.observable('');
	self.newPassword = ko.observable('');
	self.newStreamPath = ko.observable('');
	self.newSubStreamPath = ko.observable('');
	self.newBrand = ko.observable(1);
	
	self.brands = ko.observableArray([
		{ 'id': 0, text: 'Manual', connectionString: '{StreamPath}', connectionStringSub: '{SubStreamPath}' },
		{ 'id': 1, text: 'Hikvision', connectionString: 'rtsp://{Username}:{Password}@{Hostname}:554/Streaming/Channels/101?transportmode=unicast&profile=Profile_1', connectionStringSub: 'rtsp://{Username}:{Password}@{Hostname}:554/Streaming/Channels/102?transportmode=unicast&profile=Profile_1' }
	]);
	
	self.variableNameToValue = function(variable){
		if( variable == "Name" ) {
			return self.newName();
		}
		if( variable == "Description" ) {
			return self.newDescription();
		}
		if( variable == "Hostname" ) {
			return self.newHostname();
		}
		if( variable == "Username" ) {
			return self.newUsername();
		}
		if( variable == "Password" ) {
			return self.newPassword();
		}
		if( variable == "StreamPath" ) {
			return self.newStreamPath();
		}
		if( variable == "SubStreamPath" ) {
			return self.newSubStreamPath();
		}
		throw `Unknown variable ${variable}`;
	};
	
	self.replaceAllVariables = function(input){
		var variables = input.matchAll( /\{(?<variable>\w+)\}/g );
		for(var foundVar of variables ){
			input = input.replace( foundVar[0], self.variableNameToValue(foundVar[1]) );
		}
		return input;
	};
	
	self.cameras = ko.observableArray([]);
	
	self.newCameraPageIndex = ko.observable(0);
	
	self.wizardSteps = ko.observableArray([
		{ name: 'Basics', content: 'CameraSetupBasics' },
		{ name: 'Authentication', content: 'CameraSetupAuth', condition: function(){ 
			return Number(self.newBrand()) != 0;
		} },
		{ name: 'Stream', content: 'CameraSetupStream', condition: function(){
			return Number(self.newBrand()) == 0;
		} },
		{ name: 'Finish', content: 'CameraSetupFinish' }
	]);
	
	self.isHostnameEnabled = ko.computed(function() {
		return self.newBrand() != 0;
	} );
	
	self.isVisible = function(inputName) {
		return inputName == self.wizardSteps()[self.newCameraPageIndex()].name;
	};
	
	self.shouldSelectPage = function(index) {
		if(self.wizardSteps()[index].condition) {
			return self.wizardSteps()[index].condition();
		}
		return true;
	};
	
	self.previousAddCameraPage = function() {
		if( self.newCameraPageIndex() > 0 ) {
			var newIndex = self.newCameraPageIndex();
			while( !self.shouldSelectPage(--newIndex ) );
			self.newCameraPageIndex( newIndex );
		}
	};
	
	self.nextAddCameraPage = function() {
		if( self.newCameraPageIndex() < self.wizardSteps().length - 1 ) {
			var newIndex = self.newCameraPageIndex();
			while( !self.shouldSelectPage(++newIndex ) );
			self.newCameraPageIndex( newIndex );
		}
		else
		{
			self.createNewCamera();
		}
	};
	
	self.cannotGoBackAddNewCamera = ko.computed(function() {
		return  self.newCameraPageIndex() <= 0;
	} );
	
	self.cannotGoForwardAddNewCamera = ko.computed(function() {
		return self.newCameraPageIndex() > self.wizardSteps().length - 1;
	});
	
	self.newCameraSelectedColour = function(index){
		return index == self.newCameraPageIndex() ? 'black' : 'lightgrey';
	};
	
	self.newCameraNextLabel = ko.computed(function(){
		return self.newCameraPageIndex() == self.wizardSteps().length - 1 ? 'Finish' : 'Next';
	});
	
	self.createNewCameraData = function() {
		
		self.newCameraPageIndex(0);
		self.newName('');
		self.newDescription('');
		self.newHostname('');
		self.newUsername('');
		self.newPassword('');
	};
	self.createNewCameraData();
	
	
	
	self.createNewCamera = function(){
		
		var newBrand = Number(self.newBrand());
		var connectionStringTemplate = self.brands()[newBrand].connectionString;
		var connectionStringSubTemplate = self.brands()[newBrand].connectionStringSub;
		
		self.isBusy(true);
		var connectionString = self.replaceAllVariables( connectionStringTemplate );
		var connectionStringSub = self.replaceAllVariables( connectionStringSubTemplate );
		var newCamera = {
			'csrf': self.authentication.csrfToken(),
			displayName: self.newName(),
			description: self.newDescription(),
			connectionString: connectionString,
			connectionStringSub: connectionStringSub
		};
		makeQuery( newCamera, '/camera/admin_create/', true, "error|Error creating camera.",
			function(result){
				self.cameras.push(  new CameraViewModel( self.witness, self, result.id, newCamera.displayName, newCamera.description, newCamera.connectionString, newCamera.connectionStringSub, [], '', false, {} ) );
				$('#addCameraAdmin').modal('toggle');
				self.newName('');
				self.newDescription('');
				
				self.createNewCameraData();
			},
			function(result){ /*finally*/
				self.isBusy(false);
			}
		);
	};
	
	self.sortCameras = function() {
		self.cameras.sort( function( left, right ) {
			return left.name() < right.name();
		} );
	};
	
	self.resetStats = function() {
		var query = {
			'csrf': self.authentication.csrfToken()
		}
		makeQuery( query, '/camera/admin_reset_stats/', true, "error|Error fetching cameras list.",
			function(result){
				self.refreshCamerasAsAdmin();
			} 
		);
	}
	
	self.refreshCamerasAsAdmin = function() {	
		makeQuery( null, '/camera/admin_enum/', true, "error|Error fetching cameras list.",
			function(result){
				var cameraList = result;
				if( cameraList ) {
					self.cameras.remove( function( item ) {
						var found = false;
						for( var camera = 0; camera < cameraList.length; camera++ ) {
							if( item.id == cameraList[camera].id ) {
								found = true;
								break;
							}
						}
						
						return !found;
					} );
					
					for( var camera = 0; camera < cameraList.length; camera++ ) {
						var newId = cameraList[camera].id;
						var newEnabled = cameraList[camera].enabled;
						var newName = cameraList[camera].name;
						var newDescription = cameraList[camera].description;
						var newConnectionString = cameraList[camera].connectionString;
						var newConnectionStringSub = cameraList[camera].connectionStringSub;
						var newGroups = cameraList[camera].groups;
						var newStatus = cameraList[camera].status;
						var newRecording = cameraList[camera].recording;
						
						var existing = null;
						
						for( var existingCamera = 0; existingCamera < self.cameras().length; existingCamera++ )
						{
							if( self.cameras()[ existingCamera ].id == newId ) {
								existing = self.cameras()[ existingCamera ];
								break;
							}
						}
						
						if( existing ){
							existing.name(newName);
							existing.enabled(newEnabled);
							existing.description(newDescription);
							existing.connectionString(newConnectionString);
							existing.connectionStringSub(newConnectionStringSub);
							existing.status(newStatus);
							existing.isRecording(newRecording);
							existing.groups(newGroups);
							
							existing.lastTimestamp( cameraList[camera].lastTimestamp );
							existing.statFrameCount( cameraList[camera].frameCount );
							
							existing.populateStats( cameraList[camera] );
						}
						else {
							self.cameras.push(  new CameraViewModel( self.witness, newId, newEnabled, newName, newDescription, newConnectionString, newConnectionStringSub, newGroups, newStatus, false, cameraList[camera] ) );
						}
						
						self.sortCameras();
					}
				}
			} 
		);
	};
	
	self.adminAction = function() {
		self.refreshCamerasAsAdmin();
	};
};