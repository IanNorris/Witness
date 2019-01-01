var AdminDebugViewModel = function( authentication ) {
	"use strict";
	
	var self = this;
	self.authentication = authentication;
	
	self.values = ko.observableArray([]);
	
	self.resetValue = function( name ) {	
		var data = {
			'csrf': self.authentication.csrfToken(),
			name: name
		};
		makeQuery( data, '/debug/reset/', true, "error|Error resetting " + name + ".",
			function(result){
			},
			function(result){ /*finally*/
				self.refreshDebugValuesAsAdmin();
			}
		);
	};
	
	self.setValue = function( name, newValue ) {	
		var data = {
			'csrf': self.authentication.csrfToken(),
			name: name,
			value: '' + newValue
		};
		makeQuery( data, '/debug/set/', true, "error|Error setting debug value for " + name + ".", function(result){},
		function(result){
			self.refreshDebugValuesAsAdmin();
		}
		);
	};
	
	self.refreshDebugValuesAsAdmin = function() {	
		makeQuery( null, '/debug/enum/', true, "error|Error fetching debug values.",
			function(result){
			self.values([]);
			
			for( var index = 0; index < result.values.length; index++ ) {
				
				var newName = result.values[index].name;
				var newValue = result.values[index].value;
				
				self.values.push(  new AdminDebugValueViewModel( self, newName, newValue ) );
			}
			
			self.values.sort( function( left, right ) {
				return left.name() < right.name();
			} );
		} );
	};
	
	self.adminAction = function() {
		self.refreshDebugValuesAsAdmin();
	};
};