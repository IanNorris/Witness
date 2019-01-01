var AdminDebugValueViewModel = function( parent, name, value ) {
	"use strict";
	
	var self = this;	
	
	self.parent = parent;
	
	self.name = ko.observable(name);
	self.value = ko.observable(value);
	
	self.resetValue = function(){
		self.parent.resetValue(self.name());
		return true;
	};
	
	self.value.subscribe( function( newValue ){
		self.parent.setValue(self.name(), newValue);
		return true;
	} );
};