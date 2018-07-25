var AuthenticationViewModel = function( parent ) {
	"use strict";
	
	var self = this;	
	
	self.parent = parent;
	
	self.ready = ko.observable(false);
	
	self.sessionToken = getCookie('SessionToken-'+location.port);
	self.csrfToken = ko.observable('');
	self.username = ko.observable('');
	self.admin = ko.observable(false);
	self.displayName = ko.observable('');

	self.queryUserProfile = function( callback ) {	
		makeQuery( {}, '/auth/profile', true, "error|Error while querying user profile.", function(result) {				
			self.csrfToken(result.csrf);
			self.username(result.username);
			self.admin(result.admin ? true : false);
			self.displayName(result.displayName);
			
			callback();
			
			self.ready(true);
		} );
	};
	
	
	self.logoutAction = function() {
				
		var data = { 
			'csrf': self.csrfToken()
		};
		
		makeQuery( data, '/auth/logout', true, "warning|Error while logging out.", function(result){
			window.location.replace( "/" );
		});
	};
	
	//Forwarding action
	self.adminAction = function() {
		parent.adminAction();
	};
};