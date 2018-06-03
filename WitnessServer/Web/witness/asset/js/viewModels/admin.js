var AdminViewModel = function( currentUsername ) {
	"use strict";
	
	var self = this;
	
	self.users = new AdminUsersViewModel( currentUsername );
	self.cameras = new AdminCamerasViewModel();
	
	self.adminAction = function(){		
		self.users.adminAction();
		self.cameras.adminAction();
	};
};