var AuthenticationViewModel = function() {
	"use strict";
	
	var self = this;	
	
	self.ready = ko.observable(false);
	
	self.sessionToken = getCookie('SessionToken');
	self.csrfToken = ko.observable('');
	self.username = ko.observable('');
		
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
			
			self.ready(true);
		} )
		.fail( function( result ) {
			window.location.replace( "/" );
		} );
	
	
	
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
};

var WitnessViewModel = function() {
	"use strict";
	
	var self = this;
	
	self.authentication = new AuthenticationViewModel();
	
	self.ready = ko.computed( function() {
		return self.authentication.ready();
	} );
	
	self.notReady = ko.computed( function() {
		return !self.ready();
	} );
};
	
var g_viewModel = null;

$(document).ready(function() {
	g_viewModel = new WitnessViewModel();
	
	ko.applyBindings(g_viewModel);
});