var AdminUsersViewModel = function( currentUsername ) {
	"use strict";
	
	var self = this;
	
	self.currentUsername = currentUsername;
	
	//Admin functionality
	self.users = ko.observableArray([]);
	
	self.refreshUsersAsAdmin = function() {	
		$.ajax({
			method: 'GET',
			url: '/auth/admin_enum/',
			contentType: 'application/json; charset=utf-8',
		} )
		.done( function( result ) {
			
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
				
				result[user].userPermissions = [0,1];
				var newPermissions = result[user].userPermissions;

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
					existing.userPermissions(newPermissions);
				}
				else {
					self.users.push(  new UserViewModel( self, newUsername, newEnabled, newAdmin, newDisplayName, newPermissions ) );
				}
				
				self.users.sort( function( left, right ) {
					return left.username() < right.username();
				} );
			}
		} )
		.fail( function( result ) {
			if( result.status == 401 || result.status == 403 ) {
				window.location.replace( "/" );
				return;
			}
			$.toast( {
				text: "Error fetching user list.",
				type: 'danger',
				bgColor: '#a94442',
				position: 'top-center'
			} );
		} );
	};
	
	self.adminAction = function() {
		self.refreshUsersAsAdmin();
	};
};