var AdminUserViewModel = function( parent, username, enabled, admin, displayName, groups ) {
	"use strict";
	
	var self = this;	
	
	self.parent = parent;
	
	self.username = ko.observable(username);
	self.enabled = ko.observable(enabled);
	self.admin = ko.observable(admin);
	self.displayName = ko.observable(displayName);
	self.userGroups = ko.observable(groups);
	
	self.toggleEnabled = function(){
		return true;
	};
	
	self.toggleAdmin = function(){
		return true;
	};
	
	self.isSelf = ko.computed( function() {
		return self.username() == self.parent.currentUsername;
	} );
};