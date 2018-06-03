$('body').on( "witnessTemplatesLoaded", function( event ) {
	// Variable
	var $ = jQuery;
	$.fn.ripple = function () {
		$(this).click(function (e) {
			var rippler = $(this),
				ink = rippler.find(".ink");

			if (rippler.find(".ink").length === 0) {
				rippler.append("<span class='ink'></span>");
			}


			ink.removeClass("animate");
			if (!ink.height() && !ink.width()) {
				var d = Math.max(rippler.outerWidth(), rippler.outerHeight());
				ink.css({
					height: d,
					width: d
				});
			}

			var x = e.pageX - rippler.offset().left - ink.width()/2;
			var y = e.pageY - rippler.offset().top - ink.height()/2;
			ink.css({
			  top: y+'px',
			  left:x+'px'
			}).addClass("animate");
		});
	};

	this.hide = function()
	{
		$(".tree").hide();
		$(".sub-tree").hide();
	};

	this.treeMenu = function()
	{

		$('.tree-toggle').click(function(e){
			e.preventDefault();
			var $this = $(this).parent().children('ul.tree');
			$(".tree").not($this).slideUp(600);
			$this.toggle(700);

			$(".tree").not($this).parent("li").find(".tree-toggle .right-arrow").removeClass("fa-angle-down").addClass("fa-angle-right");
			$this.parent("li").find(".tree-toggle .right-arrow").toggleClass("fa-angle-right fa-angle-down");
		});

		$('.sub-tree-toggle').click(function(e) {
			e.preventDefault();
			var $this = $(this).parent().children('ul.sub-tree');
			$(".sub-tree").not($this).slideUp(600);
			$this.toggle(700);

			$(".sub-tree").not($this).parent("li").find(".sub-tree-toggle .right-arrow").removeClass("fa-angle-down").addClass("fa-angle-right");
			$this.parent("li").find(".sub-tree-toggle .right-arrow").toggleClass("fa-angle-right fa-angle-down");
		});
	};



	this.leftMenu =  function()
	{

		 $('.opener-left-menu').on('click', function(){
			$(".line-chart").width("100%");
			$(".mejs-video").height("auto").width("100%");
			if($('#right-menu').is(":visible"))
			{
			   $('#right-menu').animate({ 'width': '0px' }, 'slow', function(){
					  $('#right-menu').hide();
				  });
			}
			if( $('#left-menu .sub-left-menu').is(':visible') ) {
				$('#content').animate({ 'padding-left': '0px'}, 'slow');
				$('#left-menu .sub-left-menu').animate({ 'width': '0px' }, 'slow', function(){
					$('.overlay').show();
					  $('.opener-left-menu').removeClass('is-open');
					  $('.opener-left-menu').addClass('is-closed');
					$('#left-menu .sub-left-menu').hide();
				});

			}
			else {
				$('#left-menu .sub-left-menu').show();
				$('#left-menu .sub-left-menu').animate({ 'width': '230px' }, 'slow');
				$('#content').animate({ 'padding-left': '230px','padding-right':'0px'}, 'slow');
				$('.overlay').hide();
					  $('.opener-left-menu').removeClass('is-closed');
					  $('.opener-left-menu').addClass('is-open');
			}
		});
	};

	$(".ripple div").ripple();

	$("#left-menu .sub-left-menu").niceScroll();

	var datetime = null,
		date = null;

	var update = function () {
		date = moment(new Date())
		datetime.html(date.format('HH:mm'));
		datetime2.html(date.format('dddd, MMMM Do YYYY'));
	};

	$(document).ready(function(){
		datetime = $('.time h1');
		datetime2 = $('.time p');
		update();
		setInterval(update, 1000);
	});


	$("body").tooltip({ selector: '[data-toggle=tooltip]' });
	this.leftMenu();
	this.treeMenu();
	this.hide();
});
