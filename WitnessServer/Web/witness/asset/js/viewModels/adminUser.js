var AdminUserViewModel = function( parent, userID, username, enabled, admin, displayName, groups ) {
	"use strict";
	
	var self = this;	
	
	self.parent = parent;
	
	self.userID = userID;
	self.username = ko.observable(username);
	self.enabled = ko.observable(enabled);
	self.admin = ko.observable(admin);
	self.displayName = ko.observable(displayName);
	self.userGroups = ko.observable(groups);
	
	self.toggleEnabled = function(){
		self.parent.toggleEnabled(self.username(), self.enabled());
		return true;
	};
	
	self.toggleAdmin = function(){
		self.parent.toggleAdmin(self.username(), self.admin());
		return true;
	};
	
	self.displayName.subscribe( function( newValue ){
		self.parent.setDisplayName(self.username(), newValue);
		return true;
	} );
	
	self.userGroups.subscribe(  function( newValue ){
		self.parent.setUserGroups(self.userID, self.username(), newValue.map(Number));
		return true;
	} );
	
	self.isSelf = ko.computed( function() {
		return self.username() == self.parent.currentUsername;
	} );
};