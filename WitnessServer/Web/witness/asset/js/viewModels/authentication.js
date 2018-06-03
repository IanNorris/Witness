var AuthenticationViewModel = function( parent ) {
	"use strict";
	
	var self = this;	
	
	self.parent = parent;
	
	self.ready = ko.observable(false);
	
	self.sessionToken = getCookie('SessionToken');
	self.csrfToken = ko.observable('');
	self.username = ko.observable('');
	self.admin = ko.observable(false);
	self.displayName = ko.observable('');
	
	//Admin functionality
	self.users = ko.observableArray([]);

	self.queryUserProfile = function() {	
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
				self.admin(result.admin ? true : false);
				self.displayName(result.displayName);
				
				self.ready(true);
			} )
			.fail( function( result ) {
				window.location.replace( "/" );
			} );
	};
	
	
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
	
	self.refreshUsersAsAdmin = function() {
		if( !self.admin() ) {
				return;
		}
		
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
		parent.viewMode(VIEW_MODE_ADMIN);
		window.location.hash = "#Administration";
		self.refreshUsersAsAdmin();
	};
};