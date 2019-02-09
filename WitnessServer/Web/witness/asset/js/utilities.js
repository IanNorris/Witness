var makeToast = function(message, messageType ) {
	var colour = '#a94442'; //Red
	var type = 'danger';
	
	if( messageType == "warning" ) {
		colour = '#F0AD4E';
		type = 'warning';
	}
	else if( messageType == "info" ) {
		colour = '#73CAEF';
		type = 'info';
	}
	//else use error
	
	$.toast( {
		text: message,
		type: type,
		bgColor: colour,
		position: 'top-center'
	} );	
}

var makeQuery = function( postData, queryString, redirectOnFail, messageOnFail, onSuccess, onAlways ) {
	var queryType = postData ? 'POST' : 'GET';
	
	var queryData = postData != null ? {
		dataType: 'json',
		data: JSON.stringify(postData),
		method: queryType,
		url: queryString,
		contentType: 'application/json; charset=utf-8'
	} : {
		method: queryType,
		url: queryString,
		contentType: 'application/json; charset=utf-8'
	};
	
	$.ajax(queryData).done( function( result ) {
		if( onSuccess ) {
			onSuccess( result );
		}
		if( onAlways ) {
			onAlways( result );
		}
	} ).fail( function( result ) {
		if( onAlways ) {
			onAlways( result );
		}
		
		if( redirectOnFail ) {
			if( result.status == 401 || result.status == 403 ) {
				window.location.replace( "/" );
				return;
			}
		}
		
		if( messageOnFail ) {	
			var messageSplit = messageOnFail.split("|");
			var messageType = messageSplit[0];
			var message = messageSplit[1];
			
			if( result.responseJSON && result.responseJSON.errorMessage ) {
				
				var error = result.responseJSON.errorMessage;
				if( error.includes("UNIQUE ") ) {
					message += "<br/>Item is not unique.";
				}
				else {
					message += "<br/>" + result.responseJSON.errorMessage;
				}
			}
			
			makeToast( message, messageType );
		}
	} );
};