var AdminGroupViewModel = function(id, name, description) {
	"use strict";
	
	var self = this;

	self.id = id;
	self.text = name;
	self.description = ko.observable(description);
	
	self.displayName = ko.pureComputed( {
		read: function() {
			return this.text;
		},
		write: function(value) {
			self.text = value;
		},
		owner: self
	} );
};