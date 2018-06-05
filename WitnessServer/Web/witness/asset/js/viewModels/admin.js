var AdminViewModel = function( authentication ) {
	"use strict";
	
	var self = this;
	self.authentication = authentication;
	
	self.groups = new AdminGroupsViewModel( authentication );
	self.users = new AdminUsersViewModel( authentication.username(), self.groups );
	self.cameras = new AdminCamerasViewModel( self.groups );
	
	self.adminAction = function(){
		self.groups.adminAction();
		self.users.adminAction();
		self.cameras.adminAction();
	};
};