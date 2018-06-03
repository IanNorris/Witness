var CameraClipsViewModel = function( parent, cameraID ) {
	"use strict";
	
	var self = this;
		
	self.parent = parent;
	self.cameraID = ko.observable(cameraID);
	
	self.totalClipsInRange = ko.observable(0);
	
	self.rangeOptions = ko.observableArray([1,2,3,5,10,20,50,100]);
	self.maxCount = ko.observable(5);
	
	//self.startDate = ko.observable(0);
	//self.rangePeriod = ko.observable(24 * 60 * 60);
	self.rangePeriod = ko.observable(9999999999);
	self.pageOffset = ko.observable(0);
	self.visiblePages = ko.observableArray([]);
	
	self.clips = ko.observableArray([]);
	
	self.page = ko.computed( function() {
		return Math.floor( self.pageOffset() / self.maxCount() );
	} );
	
	self.clipPageStart = ko.computed( function() {
		return self.page() * self.maxCount() + 1;
	} );
	
	self.clipCount = ko.computed( function() {
		return self.clips().length;
	} );
	
	self.clipPageEnd = ko.computed( function() {
		return Math.min( self.pageOffset() + self.clipCount(), self.totalClipsInRange() );
	} );
	
	self.totalPages = ko.computed( function() {
		return self.totalClipsInRange() / self.maxCount();
	} );
	
	self.previousPage = ko.computed( function() {
		return (self.page()-1);
	} );
	
	self.nextPage = ko.computed( function() {
		return (self.page()+1);
	} );
	
	self.incrementPage = function() {
		if( self.canIncrementPage() ) {
			self.pageOffset( Math.max( Math.min( self.pageOffset() + self.maxCount(), self.totalClipsInRange() - self.maxCount() ), 0 ) );
			self.refreshClipData();
		}
	};
	
	self.decrementPage = function() {
		if( self.canDecrementPage() ) {
			self.pageOffset( Math.max( self.pageOffset() - self.maxCount(), 0 ) );
			self.refreshClipData();
		}
	};
	
	self.setPage = function(newPage) {
		self.pageOffset( newPage * self.maxCount() );
		self.refreshClipData();
	};
	
	self.getDisplayPage = function(newPage) {
		return newPage+1;
	};
	
	self.setFirstPage = function() {
		self.pageOffset(0);
		self.refreshClipData();
	};
	
	self.setLastPage = function() {
		self.pageOffset(self.totalClipsInRange()-self.maxCount());
		self.refreshClipData();
	};
	
	self.isPage = function(pageNo) {
		return self.page() == pageNo;
	};
	
	self.changeMaxCount = function() {
		self.refreshClipData();
	};
	
	self.updateVisiblePages = function() {
		var pages = [];
		
		if( self.page() >= 2 )
		{
			pages.push( self.page() - 2 );
		}
		
		if( self.page() >= 1 )
		{
			pages.push( self.page() - 1 );
		}
		
		pages.push( self.page() );
		
		if( self.page() + 1 < self.totalPages() )
		{
			pages.push( self.page() + 1 );
		}
		
		if( self.page() + 2 < self.totalPages() )
		{
			pages.push( self.page() + 2 );
		}
		
		self.visiblePages(pages);
	};
	
	self.canIncrementPage = ko.computed( function() {
		return (self.page()+1) < self.totalPages();
	} );
	
	self.canDecrementPage = ko.computed( function() {
		return (self.page()-1) >= 0;
	} );
	
	self.canNotIncrementPage = ko.computed( function() {
		return !self.canIncrementPage();
	} );
	
	self.canNotDecrementPage = ko.computed( function() {
		return !self.canDecrementPage();
	} );
	
	self.refreshClipData = function() {
		var timeNow = ((moment().utc() / 1000) - 1).toFixed(0);
		$.ajax({
			method: 'GET',
			url: '/clip/enum/' + self.cameraID() + '/' + self.maxCount() + '/' + timeNow + '/' + self.rangePeriod() + '/' + self.pageOffset(),
			contentType: 'application/json; charset=utf-8',
		} )
		.done( function( result ) {
			
			self.totalClipsInRange( result.count );
			
			self.clips.remove( function( item ) {
				var found = false;
				for( var clip = 0; clip < result.clips.length; clip++ ) {
					if( item.timestamp() == result.clips[clip].timestamp ) {
						found = true;
						break;
					}
				}
				
				return !found;
			} );
			
			for( var clip = 0; clip < result.clips.length; clip++ ) {
				
				var newTimestamp = result.clips[clip].timestamp;
				var newCameraID = result.clips[clip].cameraID;
				var newMotionTimestamp = result.clips[clip].motionTimestamp;
				var newActiveDuration = result.clips[clip].activeDuration;
				var newDuration = result.clips[clip].duration;
				var newRecordMode = result.clips[clip].recordMode;
				var newMaxMotion = result.clips[clip].maxMotion;
				var newDescription = result.clips[clip].description;

				var existing = null;
				
				for( var existingClip = 0; existingClip < self.clips().length; existingClip++ )
				{
					if( self.clips()[ existingClip ].timestamp() == newTimestamp ) {
						existing = self.clips()[ existingClip ];
						break;
					}
				}
				
				if( existing ){
					existing.cameraID(newCameraID);
					existing.motionTimestamp(newMotionTimestamp);
					existing.activeDuration(newActiveDuration);
					existing.duration(newDuration);
					existing.recordMode(newRecordMode);
					existing.maxMotion(newMaxMotion);
					existing.description(newDescription);
				}
				else {
					self.clips.push(  new ClipViewModel( self, newTimestamp, newCameraID, newMotionTimestamp, newActiveDuration, newDuration, newRecordMode, newMaxMotion, newDescription ) );
				}
				
				self.clips.sort( function( left, right ) {
					return left.timestamp() < right.timestamp();
				} );
			}
			
			self.updateVisiblePages();
		} )
		.fail( function( result ) {
			if( result.status == 401 || result.status == 403 ) {
				window.location.replace( "/" );
				return;
			}
			$.toast( {
				text: "Error fetching clip list.",
				type: 'danger',
				bgColor: '#a94442',
				position: 'top-center'
			} );
		} );
	};
	self.refreshClipData();
};