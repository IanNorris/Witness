var AdminGroupsViewModel = function( authentication ) {
	"use strict";
	
	var self = this;
	self.authentication = authentication;

	self.isBusy = ko.observable(false);
	self.idToDelete = 0;
	self.displayNameToDelete = ko.observable('');
	self.newGroupNameBound = ko.observable('');
	self.newGroupDescriptionBound = ko.observable('');
	
	self.createNewGroup = function(){
		self.isBusy(true);
		var newGroup = {
			'csrf': self.authentication.csrfToken(),
			displayName: self.newGroupNameBound(),
			description: self.newGroupDescriptionBound()
		};
		makeQuery( newGroup, '/group/create/', true, "error|Error creating group.",
			function(result){
				self.groups.push(  new AdminGroupViewModel( self, result.id, newGroup.displayName, newGroup.description ) );
				$('#addGroupAdmin').modal('toggle');
				self.newGroupNameBound('');
				self.newGroupDescriptionBound('');
				
				
			},
			function(result){ /*finally*/
				self.isBusy(false);
			}
		);
	};
	
	self.updateGroup = function(existingGroup) {
		var groupinfo = {
			'csrf': self.authentication.csrfToken(),
			id: existingGroup.id,
			displayName: existingGroup.displayName(),
			description: existingGroup.description()
		};
		makeQuery( groupinfo, '/group/update/', true, "error|Error updating group.",
			function(result){
				self.groups.remove( function( item ) {
					return item.id == existingGroup.id;
				} );
				self.groups.push(  existingGroup );
				
				makeToast( existingGroup.text + ' group updated.', 'info' );
			},
			null
		);
	}
	
	self.showDeleteDialog = function( id, displayName ) {
		self.idToDelete = id;
		self.displayNameToDelete( displayName );
		$('#deleteGroupAdmin').modal('toggle');
	};
	
	self.deleteGroup = function(){
		self.isBusy(true);
		var groupToDelete = {
			'csrf': self.authentication.csrfToken(),
			id: self.idToDelete
		};
		makeQuery( groupToDelete, '/group/delete/', true, "error|Error deleting group.",
			function(result){
				self.groups.remove( function( item ) {
						return item.id == self.idToDelete;
					} );
				
				$('#deleteGroupAdmin').modal('toggle');
				self.displayNameToDelete('');
				self.idToDelete = 0;
			},
			function(result){ /*finally*/
				self.isBusy(false);
			}
		);
	};
	
	self.groups = ko.observableArray([]);
	
	self.sortGroups = function() {
		self.groups.sort( function( left, right ) {
			return left.displayName() < right.displayName();
		} );
	};
	
	self.refreshGroupsAsAdmin = function() {	
		makeQuery( null, '/group/enum/', true, "error|Error fetching group list.",
			function(result){
				var groupList = result.groups;
				if( groupList ) {
					self.groups.remove( function( item ) {
						var found = false;
						for( var user = 0; user < groupList.length; user++ ) {
							if( item.id == groupList[user].id ) {
								found = true;
								break;
							}
						}
						
						return !found;
					} );
					
					for( var user = 0; user < groupList.length; user++ ) {
						
						var newId = groupList[user].id;
						var newDisplayName = groupList[user].displayName;
						var newDescription = groupList[user].description;
						
						var existing = null;
						
						for( var existingGroup = 0; existingGroup < self.groups().length; existingGroup++ )
						{
							if( self.groups()[ existingGroup ].id == newId ) {
								existing = self.groups()[ existingGroup ];
								break;
							}
						}
						
						if( existing ){
							existing.displayName(newDisplayName);
							existing.description(newDescription);
						}
						else {
							self.groups.push(  new AdminGroupViewModel( self, newId, newDisplayName, newDescription ) );
						}
						
						self.sortGroups();
					}
				}
			} 
		);
	};
	
	self.adminAction = function() {
		self.refreshGroupsAsAdmin();
	};
};