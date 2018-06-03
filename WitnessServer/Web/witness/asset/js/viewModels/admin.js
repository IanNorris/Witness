var AdminViewModel = function( currentUsername ) {
	"use strict";
	
	var self = this;
	
	self.groups = new AdminGroupsViewModel();
	self.users = new AdminUsersViewModel( currentUsername, self.groups );
	self.cameras = new AdminCamerasViewModel( self.groups );
	
	self.adminAction = function(){
		self.groups.adminAction();
		self.users.adminAction();
		self.cameras.adminAction();
	};
};