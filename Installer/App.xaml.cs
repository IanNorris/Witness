using System;
using System.Windows;

namespace Installer
{
	/// <summary>
	/// Interaction logic for App.xaml
	/// </summary>
	public partial class App : Application
	{
		private void Application_Startup(object sender, StartupEventArgs e)
		{
			if ( e.Args.Length == 1 )
			{
				if( string.Compare( e.Args[0], "/uninstall" ) == 0 )
				{
					var AM = new AutomationManager( StartupMode.Task, false, AppDomain.CurrentDomain.BaseDirectory);
					AM.Uninstall();
				}
			}

			var Window = new MainWindow();
			this.MainWindow = Window;
			Window.Show();
		}
	}
}
