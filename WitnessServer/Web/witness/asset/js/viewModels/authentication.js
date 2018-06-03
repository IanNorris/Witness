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

	self.queryUserProfile = function( callback ) {	
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
				
				callback();
				
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
	
	//Forwarding action
	self.adminAction = function() {
		parent.adminAction();
	};
};