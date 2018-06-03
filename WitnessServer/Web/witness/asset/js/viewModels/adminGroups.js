var AdminGroupsViewModel = function() {
	"use strict";
	
	var self = this;	
	
	self.groups = ko.observableArray([]);
	
	self.refreshGroupsAsAdmin = function() {	
		makeQuery( null, '/group/enum/', true, "error|Error fetching group list.",
			function(result){
				var groupList = result.groups;
				if( groupList ) {
					self.groups.remove( function( item ) {
						var found = false;
						for( var user = 0; user < groupList.length; user++ ) {
							if( item.id == groupList[user].id ) {
								found = true;
								break;
							}
						}
						
						return !found;
					} );
					
					for( var user = 0; user < groupList.length; user++ ) {
						
						var newId = groupList[user].id;
						var newDisplayName = groupList[user].displayName;
						var newDescription = groupList[user].description;
						
						var existing = null;
						
						for( var existingGroup = 0; existingGroup < self.groups().length; existingGroup++ )
						{
							if( self.groups()[ existingGroup ].id == newId ) {
								existing = self.groups()[ existingGroup ];
								break;
							}
						}
						
						if( existing ){
							existing.displayName(newAdmin);
							existing.description(newDescription);
						}
						else {
							self.groups.push(  new AdminGroupViewModel( newId, newDisplayName, newDescription ) );
						}
						
						self.groups.sort( function( left, right ) {
							return left.displayName() < right.displayName();
						} );
					}
				}
			} 
		);
	};
	
	self.adminAction = function() {
		self.refreshGroupsAsAdmin();
	};
};