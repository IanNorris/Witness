var AdministrationViewModel = function() {
	"use strict";
	
	var self = this;
	
	self.availablePermissions = ko.observableArray(["External", "Internal"]);
};