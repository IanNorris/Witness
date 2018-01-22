var LoginViewModel = function() {
	"use strict";
	
	var self = this;
	
	self.errorMessage = ko.observable('');
	self.invalidLogin = ko.observable(false);
	self.loginInProgress = ko.observable(false);
	self.loginVisible = ko.observable(false);
	self.username = ko.observable('');
	self.password = ko.observable('');
	self.loginButtonLabel = ko.observable('');
	
	self.setLoginInProgress = function( inProgress ) {
		if( inProgress ) {
			self.loginInProgress(true);
			self.loginButtonLabel('<i class="fa fa-spinner fa-spin"/> Signing in');
		}
		else {
			self.loginButtonLabel('Sign in');
			self.loginInProgress(false);
		}
	};
	
	self.setLoginInProgress(false);
	
	self.canSubmit = ko.computed( function() {
		self.invalidLogin(false);
		
		return self.username().length > 0
		&& self.password().length > 0
		&& !self.loginInProgress();
	} );
	
	self.submitLogin = function() {
		self.setLoginInProgress(true);
		
		var loginData = JSON.stringify( { 
			'username': self.username(), 
			'password': self.password() 
		} );
		
		$.ajax({
			method: 'POST',
			url: '/auth',
			dataType: 'json',
			data: loginData,
			contentType: 'application/json; charset=utf-8',
		} )
		.done( function( result ) {
			window.location.replace( "/witness/" );
		} )
		.fail( function( result ) {
			self.setLoginInProgress( false );
			if( result.status == 401 ) {
				self.errorMessage('Invalid username or password.');
				self.invalidLogin(true);
			}
			else if( result.readyState == 4 ) {
				self.errorMessage( result.responseText );
				self.invalidLogin(true);
			}
			else {
				self.errorMessage( 'Unable to connect to server.' );
				self.invalidLogin(true);
			}
		} );
	};
};

var g_viewModel = null;

var sessionCookie = getCookie('SessionToken');
if( sessionCookie )
{
	window.location.replace( "/witness/" );
}
else
{
	$(document).ready(function() {
		g_viewModel = new LoginViewModel();
		
		ko.applyBindings(g_viewModel);
		
		g_viewModel.loginVisible(true);
	});
}