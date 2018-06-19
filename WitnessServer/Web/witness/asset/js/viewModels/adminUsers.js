var AdminUsersViewModel = function( authentication, groups ) {
	"use strict";
	
	var self = this;
	self.groups = groups;
	self.authentication = authentication;
	
	self.currentUsername = authentication.currentUsername;
	self.users = ko.observableArray([]);
	
	self.isBusy = ko.observable(false);
	self.newUsername = ko.observable('');
	self.newPassword = ko.observable('');
	
	self.closeCreatedUser = function(){
		$('#createdUserAdmin').modal('toggle');
		self.newUsername('');
		self.newPassword('');
	};
	
	self.createNewUser = function(){
		self.isBusy(true);
		var newUser = {
			'csrf': self.authentication.csrfToken(),
			username: self.newUsername()
		};
		makeQuery( newUser, '/auth/new_user/', true, "error|Error creating user.",
			function(result){
				self.users.push(  new AdminUserViewModel( self, result.username, true, false, result.displayName, [] ) );
				$('#addUserAdmin').modal('toggle');
				
				self.newPassword( result.password );
				$('#createdUserAdmin').modal('toggle');
			},
			function(result){ /*finally*/
				self.isBusy(false);
			}
		);
	};
	
	self.refreshUsersAsAdmin = function() {	
		makeQuery( null, '/auth/admin_enum/', true, "error|Error fetching user list.",
			function(result){
			self.users.remove( function( item ) {
				var found = false;
				for( var user = 0; user < result.length; user++ ) {
					if( item.username() == result[user].username ) {
						found = true;
						break;
					}
				}
				
				return !found;
			} );
			
			for( var user = 0; user < result.length; user++ ) {
				
				var newUsername = result[user].username;
				var newEnabled = result[user].enabled;
				var newAdmin = result[user].admin;
				var newDisplayName = result[user].displayName;
				
				result[user].userGroups = [0,1];
				var newGroups = result[user].userGroups;

				var existing = null;
				
				for( var existingUser = 0; existingUser < self.users().length; existingUser++ )
				{
					if( self.users()[ existingUser ].username() == newUsername ) {
						existing = self.users()[ existingUser ];
						break;
					}
				}
				
				if( existing ){
					existing.admin(newAdmin);
					existing.enabled(newEnabled);
					existing.displayName(newDisplayName);
					existing.userGroups(newGroups);
				}
				else {
					self.users.push(  new AdminUserViewModel( self, newUsername, newEnabled, newAdmin, newDisplayName, newGroups ) );
				}
				
				self.users.sort( function( left, right ) {
					return left.username() < right.username();
				} );
			}
		} );
	};
	
	self.adminAction = function() {
		self.refreshUsersAsAdmin();
	};
};