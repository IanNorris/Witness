var UserViewModel = function( parent, username, enabled, admin, displayName, permissions ) {
	"use strict";
	
	var self = this;	
	
	self.parent = parent;
	
	self.username = ko.observable(username);
	self.enabled = ko.observable(enabled);
	self.admin = ko.observable(admin);
	self.displayName = ko.observable(displayName);
	self.userPermissions = ko.observable(permissions);
	self.availablePermissions = [
		new PermissionViewModel("External", 0),
		new PermissionViewModel("Internal", 1),
	];
	
	self.toggleEnabled = function(){
		return true;
	};
	
	self.toggleAdmin = function(){
		return true;
	};
	
	self.isSelf = ko.computed( function() {
		return self.username() == self.parent.username();
	} );
};