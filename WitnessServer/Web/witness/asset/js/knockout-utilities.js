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

ko.bindingHandlers.select2 = {
    after: ["options", "selected"],
    init: function (el, valueAccessor, allBindingsAccessor, viewModel) {
        $(el).val( ko.unwrap(allBindingsAccessor().selected) );
        $(el).select2( ko.unwrap(allBindingsAccessor().select2) );
        
        ko.utils.domNodeDisposal.addDisposeCallback(el, function () {
            $(el).select2('destroy');
        });

        var onChange = function(e) {

            var newData = [];
            var data = $(el).select2('data');
            for( var i = 0; i < data.length; i++ ) {
                newData.push(data[i][allBindingsAccessor().optionsValue]);
            }

            allBindingsAccessor().selected( newData );
        };

        $(el).on("change", onChange );
    },
    update: function (el, valueAccessor, allBindingsAccessor, viewModel) {
        var newValue = ko.unwrap(allBindingsAccessor().selected);
        $(el).select2("data", newValue );
    }
};

ko.extenders.numeric = function(target, precision) {
    //create a writable computed observable to intercept writes to our observable
    var result = ko.pureComputed({
        read: target,  //always return the original observables value
        write: function(newValue) {
            var current = target(),
                roundingMultiplier = Math.pow(10, precision),
                newValueAsNum = isNaN(newValue) ? 0 : +newValue,
                valueToWrite = Math.round(newValueAsNum * roundingMultiplier) / roundingMultiplier;
 
            //only write if it changed
            if (valueToWrite !== current) {
                target(valueToWrite);
            } else {
                //if the rounded value is the same, but a different value was written, force a notification for the current field
                if (newValue !== current) {
                    target.notifySubscribers(valueToWrite);
                }
            }
        }
    }).extend({ notify: 'always' });
 
    //initialize with current value to make sure it is rounded appropriately
    result(target());
 
    //return the new computed observable
    return result;
};
