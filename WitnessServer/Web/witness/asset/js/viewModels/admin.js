var AdminViewModel = function( witness ) {
	"use strict";
	
	var self = this;
	self.authentication = witness.authentication;
	
	self.groups = new AdminGroupsViewModel( witness.authentication );
	self.users = new AdminUsersViewModel( witness.authentication, self.groups );
	self.cameras = new AdminCamerasViewModel( witness.authentication, self.groups, witness );
	
	self.adminAction = function(){
		self.groups.adminAction(function(){
			self.users.adminAction();
			self.cameras.adminAction();
		});
	};
};