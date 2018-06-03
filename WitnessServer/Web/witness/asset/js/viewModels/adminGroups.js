var AdminGroupsViewModel = function() {
	"use strict";
	
	var self = this;	
	
	self.groupList = ko.observableArray([
		new AdminGroupViewModel( "Public", 0 ),
		new AdminGroupViewModel( "Inside", 1 ),
		new AdminGroupViewModel( "Front", 2 ),
		new AdminGroupViewModel( "Back", 3 )
	]);
	
	self.adminAction = function() {
		
	};
};