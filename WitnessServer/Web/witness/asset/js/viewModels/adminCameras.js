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
	self.newPath = ko.observable('');
	
	self.cameras = ko.observableArray([]);
	
	self.createNewCamera = function(){
		self.isBusy(true);
		var newCamera = {
			'csrf': self.authentication.csrfToken(),
			displayName: self.newName(),
			description: self.newDescription(),
			path: self.newPath()
		};
		makeQuery( newCamera, '/camera/create/', true, "error|Error creating camera.",
			function(result){
				self.cameras.push(  new CameraViewModel( self.witness, self, result.id, newCamera.displayName, newCamera.description, newCamera.path, [], '', false ) );
				$('#addCameraAdmin').modal('toggle');
				self.newName('');
				self.newDescription('');
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
						var newName = cameraList[camera].name;
						var newDescription = cameraList[camera].description;
						var newConnectionString = cameraList[camera].connectionString;
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
							existing.description(newDescription);
							existing.connectionString(newConnectionString);
							existing.status(newStatus);
							existing.isRecording(newRecording);
							existing.groups(newGroups);
						}
						else {
							self.cameras.push(  new CameraViewModel( self.witness, newId, newName, newDescription, newConnectionString, newGroups, newStatus, false ) );
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