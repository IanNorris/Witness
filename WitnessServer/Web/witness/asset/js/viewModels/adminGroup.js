var AdminGroupViewModel = function(parent, id, name, description) {
	"use strict";
	
	var self = this;
	self.parent = parent;

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
	
	self.displayName.subscribe( function(){
		self.parent.updateGroup( self );
	} );
	
	self.description.subscribe( function(){
		self.parent.updateGroup( self );
	} );
};