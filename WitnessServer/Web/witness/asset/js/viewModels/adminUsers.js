var AdminUsersViewModel = function( authentication, groups ) {
	"use strict";
	
	var self = this;
	self.groups = groups;
	self.authentication = authentication;
	
	self.currentUsername = authentication.username();
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
				self.users.push( new AdminUserViewModel( self, result.username, true, false, result.displayName, [] ) );
				$('#addUserAdmin').modal('toggle');
				
				self.newPassword( result.password );
				$('#createdUserAdmin').modal('toggle');
			},
			function(result){ /*finally*/
				self.isBusy(false);
			}
		);
	};
	
	self.toggleEnabled = function( username, newValue ) {	
		if( !self.isBusy() ){
			var data = {
				'csrf': self.authentication.csrfToken(),
				username: username,
				value: newValue
			};
			makeQuery( data, '/auth/toggle_enabled/', true, "error|Error toggling account enabled for " + username + ".",
				function(result){
				},
				function(result){ /*finally*/
					self.isBusy(false);
				}
			);
		}
	};
	
	self.toggleAdmin = function( username, newValue ) {	
		if( !self.isBusy() ){
			var data = {
				'csrf': self.authentication.csrfToken(),
				username: username,
				value: newValue
			};
			makeQuery( data, '/auth/toggle_admin/', true, "error|Error toggling account admin for " + username + ".",
				function(result){
				},
				function(result){ /*finally*/
					self.isBusy(false);
				}
			);
		}
	};
	
	self.setDisplayName = function( username, newValue ) {	
		if( !self.isBusy() ){
			var data = {
				'csrf': self.authentication.csrfToken(),
				username: username,
				value: newValue
			};
			makeQuery( data, '/auth/set_display_name/', true, "error|Error setting account display name for " + username + ".",
				function(result){
				},
				function(result){ /*finally*/
					self.isBusy(false);
				}
			);
		}
	};
	
	self.setUserGroups = function( userID, username, newValue ) {	
		if( !self.isBusy() ){
			var data = {
				'csrf': self.authentication.csrfToken(),
				userid: userID,
				value: newValue
			};
			makeQuery( data, '/auth/set_user_groups/', true, "error|Error setting user groups for " + username + ".",
				function(result){
				},
				function(result){ /*finally*/
					self.isBusy(false);
				}
			);
		}
	};
	
	self.refreshUsersAsAdmin = function() {	
		makeQuery( null, '/auth/admin_enum/', true, "error|Error fetching user list.",
			function(result){
			self.users([]);
			
			for( var user = 0; user < result.length; user++ ) {
				
				var newUserID = result[user].userid;
				var newUsername = result[user].username;
				var newEnabled = result[user].enabled;
				var newAdmin = result[user].admin;
				var newDisplayName = result[user].displayName;
				var newGroups = result[user].groups;
				
				self.users.push(  new AdminUserViewModel( self, newUserID, newUsername, newEnabled, newAdmin, newDisplayName, newGroups ) );
			}
			
			self.users.sort( function( left, right ) {
				return left.username() < right.username();
			} );
		} );
	};
	
	self.adminAction = function() {
		self.refreshUsersAsAdmin();
	};
};