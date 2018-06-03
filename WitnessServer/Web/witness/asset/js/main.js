var g_viewModel = null;

$('body').on( "witnessTemplatesLoaded", function( event ) {

	ko.options.deferUpdates = true;
	
	g_viewModel = new WitnessViewModel();
	
	ko.applyBindings(g_viewModel);
});