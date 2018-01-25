function getCookie(name){
    var pattern = RegExp(name + "=.[^;]*")
    matched = document.cookie.match(pattern)
    if(matched){
        var cookie = matched[0].split('=')
        return cookie[1]
    }
    return false
}

ko.bindingHandlers.fadeVisible = {
    init: function(element, valueAccessor) {
        // Initially set the element to be instantly visible/hidden depending on the value
        var value = valueAccessor();
        $(element).toggle(ko.unwrap(value)); // Use "unwrapObservable" so we can handle values that may or may not be observable
    },
    update: function(element, valueAccessor) {
        // Whenever the value subsequently changes, slowly fade the element in or out
        var value = valueAccessor();
        ko.unwrap(value) ? $(element).fadeIn(500) : $(element).fadeOut(250);
    }
};

//For example:
//	remember = ko.observable(false);
//  <label class="pull-left">
//		<input name="remember" type="checkbox" class="icheck pull-left" data-bind="icheck: remember"/> Remember me
//  </label>

ko.bindingHandlers.icheck = {
    init: function (element, valueAccessor) {
        $(element).iCheck({
            checkboxClass: 'icheckbox_flat-aero',
            increaseArea: '10%'
        });

        $(element).on('ifChanged', function () {
            var observable = valueAccessor();
            observable($(element)[0].checked);
        });
    },
    update: function (element, valueAccessor) {
        var value = ko.unwrap(valueAccessor());   
        if (value) {
            $(element).iCheck('check');            
        } else {
            $(element).iCheck('uncheck');
        }
    }
};
