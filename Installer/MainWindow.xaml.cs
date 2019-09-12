using System.IO;
using System.Windows;
using WinForms = System.Windows.Forms;
using System;

namespace Installer
{
	/// <summary>
	/// Interaction logic for MainWindow.xaml
	/// </summary>
	public partial class MainWindow : Window
	{
		public Setup Setup { get; set; } = new Setup();

		public MainWindow()
		{
			var SP = new SettingsPublisher();
			SP.ReadSettings();
			Setup.SetSettings(SP.Settings);

			Setup.WebRoot = SP.GetWebRoot();

			InitializeComponent();
			DataContext = Setup;

			Login.CanSelectNextPage = false;
			RemoteAccess.CanSelectNextPage = false;
		}

		private void CompleteInstallation( InstallProgress.StatusUpdateDelegate Update )
		{
			int Total = 0;

			var SP = new SettingsPublisher();

			SP.Settings.AddRange(Setup.GetSettings());
			SP.WriteSettings(Setup);

			Total += 10;
			Update(Total);

			Permissions.GrantPermissions(AppDomain.CurrentDomain.BaseDirectory, Setup.StartupMode == Installer.StartupMode.Service ? "NT AUTHORITY\\NetworkService" : Permissions.GetCurrentUser(), System.Security.AccessControl.FileSystemRights.ReadAndExecute);
			Permissions.GrantPermissions(SettingsPublisher.GetRootSettingsPath(false), Setup.StartupMode == Installer.StartupMode.Service ? "NT AUTHORITY\\NetworkService" : Permissions.GetCurrentUser(), System.Security.AccessControl.FileSystemRights.FullControl);

			Total += 20; //30
			Update(Total);

			var AM = new AutomationManager(Setup.StartupMode, Setup.RestartOnFailure.Value, AppDomain.CurrentDomain.BaseDirectory);
			AM.UpdateConfig();

			Total += 30; //60
			Update(Total);

			AM.Start();

			Total += 40; //100
			Update(Total);
		}

		private void Wizard_Finish(object sender, Xceed.Wpf.Toolkit.Core.CancelRoutedEventArgs e)
		{
			var InstallProgress = new InstallProgress(CompleteInstallation);
			InstallProgress.ShowDialog();
		}

		private void CacheBrowse_Click(object sender, RoutedEventArgs e)
		{
			var Browser = new WinForms.FolderBrowserDialog();
			Browser.SelectedPath = Cache.Text;
			if (Browser.ShowDialog() == WinForms.DialogResult.OK)
			{
				Cache.Text = Browser.SelectedPath;
			}
		}

		private void OnPasswordOrUsernameChanged(object sender, RoutedEventArgs e)
		{
			if( sender == Username )
			{
				Setup.Username = Username.Text;
			}
			else if( sender == Password1 )
			{
				Setup.Password = Password1.Password;
			}
			else
			{
				Setup.PasswordConfirm = Password2.Password;
			}

			if (Setup.Password == null || Setup.PasswordConfirm == null)
			{
				Setup.PasswordsMatch = false;
			}
			else
			{
				Setup.PasswordsMatch = string.Compare(Setup.Password, Setup.PasswordConfirm) == 0 && Setup.Password.Length > 0;
			}

			Login.CanSelectNextPage = Setup.PasswordsMatch && Setup.Username != null && Setup.Username.Length > 0;
		}

		

		private void OnConfigureRemoteAccess_Click(object sender, RoutedEventArgs e)
		{
			RemoteAccess.CanSelectNextPage = false;

			string CurrentUser = Permissions.GetCurrentUser();
			string Username = Setup.StartupMode == Installer.StartupMode.Service ? "NT AUTHORITY\\NetworkService" : CurrentUser;

			bool IsOk = Tls.Configure( SettingsPublisher.GetRootSettingsPath( true ), Setup.Hostname, Setup.TlsContact, Setup.TlsMode, Username);
			
			if(IsOk)
			{
				MessageBox.Show($"Successfully configured!");
				RemoteAccess.CanSelectNextPage = true;
			}
		}

		private void TlsModeCombo_SelectionChanged(object sender, System.Windows.Controls.SelectionChangedEventArgs e)
		{
			RemoteAccess.CanSelectNextPage = false;
		}

		private void StartupMode_SelectionChanged(object sender, System.Windows.Controls.SelectionChangedEventArgs e)
		{

		}

		private void LoginField_KeyDown(object sender, System.Windows.Input.KeyEventArgs e)
		{
			if (e.Key == System.Windows.Input.Key.Return )
			{
				e.Handled = true;
			}
		}
	}
}