using System.Diagnostics;
using System.IO;
using System.Security.Principal;
using System.Windows;
using System.Windows.Controls;
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

		private bool _canAdvanceFromLogin = false;
		private bool _canAdvanceFromRemoteAccess = false;

		public MainWindow()
		{
			var SP = new SettingsPublisher();
			SP.ReadSettings();
			Setup.SetSettings(SP.Settings);

			Setup.WebRoot = SP.GetWebRoot();

			InitializeComponent();
			DataContext = Setup;

			WizardTabs.SelectedIndex = 0;
			UpdateNavigationButtons();
		}

		private void UpdateNavigationButtons()
		{
			int index = WizardTabs.SelectedIndex;
			int lastIndex = WizardTabs.Items.Count - 1;

			BackButton.IsEnabled = index > 0;
			NextButton.Visibility = index < lastIndex ? Visibility.Visible : Visibility.Collapsed;
			FinishButton.Visibility = index == lastIndex ? Visibility.Visible : Visibility.Collapsed;

			// Disable Next on pages that require validation
			if (index == 1) // RemoteAccess page
				NextButton.IsEnabled = _canAdvanceFromRemoteAccess;
			else if (index == 2) // Login page
				NextButton.IsEnabled = _canAdvanceFromLogin;
			else
				NextButton.IsEnabled = true;

			// Show admin elevation warning on Finish page
			if (index == lastIndex && FinishAdminHint != null)
			{
				if (NeedsElevation())
				{
					FinishAdminHint.Text = "⚠ Clicking Finish will request administrator privileges to install the Windows service.";
					FinishAdminHint.Visibility = Visibility.Visible;
				}
				else
				{
					FinishAdminHint.Visibility = Visibility.Collapsed;
				}
			}

			// Update header
			var tab = WizardTabs.SelectedItem as TabItem;
			if (tab?.Tag is string tagStr)
			{
				var parts = tagStr.Split('|');
				PageTitle.Text = parts.Length > 0 ? parts[0] : "";
				PageDescription.Text = parts.Length > 1 ? parts[1] : "";
			}
		}

		private void WizardTabs_SelectionChanged(object sender, SelectionChangedEventArgs e)
		{
			if (e.Source == WizardTabs)
				UpdateNavigationButtons();
		}

		private void BackButton_Click(object sender, RoutedEventArgs e)
		{
			if (WizardTabs.SelectedIndex > 0)
				WizardTabs.SelectedIndex--;
		}

		private void NextButton_Click(object sender, RoutedEventArgs e)
		{
			if (WizardTabs.SelectedIndex < WizardTabs.Items.Count - 1)
				WizardTabs.SelectedIndex++;
		}

		private static bool IsAdministrator()
		{
			using var identity = WindowsIdentity.GetCurrent();
			var principal = new WindowsPrincipal(identity);
			return principal.IsInRole(WindowsBuiltInRole.Administrator);
		}

		private bool NeedsElevation()
		{
			return Setup.StartupMode == Installer.StartupMode.Service && !IsAdministrator();
		}

		private void FinishButton_Click(object sender, RoutedEventArgs e)
		{
			if (NeedsElevation())
			{
				try
				{
					var psi = new ProcessStartInfo
					{
						FileName = Environment.ProcessPath,
						UseShellExecute = true,
						Verb = "runas"
					};
					Process.Start(psi);
					Application.Current.Shutdown();
					return;
				}
				catch (System.ComponentModel.Win32Exception)
				{
					MessageBox.Show("Administrator privileges are required for Service mode.\n\n" +
						"Please either grant admin access or choose a different startup mode.",
						"Elevation Required", MessageBoxButton.OK, MessageBoxImage.Warning);
					return;
				}
			}

			var InstallProgress = new InstallProgress(CompleteInstallation);
			InstallProgress.ShowDialog();
		}

		private void CompleteInstallation( InstallProgress.StatusUpdateDelegate Update )
		{
			int Total = 0;

			var SP = new SettingsPublisher();

			SP.Settings.AddRange(Setup.GetSettings());
			SP.WriteSettings(Setup);

			Total += 10;
			Update(Total);

			Permissions.GrantPermissions(InstallerPaths.ExeDirectory, Setup.StartupMode == Installer.StartupMode.Service ? "NT AUTHORITY\\NetworkService" : Permissions.GetCurrentUser(), System.Security.AccessControl.FileSystemRights.ReadAndExecute);
			Permissions.GrantPermissions(SettingsPublisher.GetRootSettingsPath(false), Setup.StartupMode == Installer.StartupMode.Service ? "NT AUTHORITY\\NetworkService" : Permissions.GetCurrentUser(), System.Security.AccessControl.FileSystemRights.FullControl);

			Total += 20; //30
			Update(Total);

			var AM = new AutomationManager(Setup.StartupMode, Setup.RestartOnFailure.Value, InstallerPaths.ExeDirectory);
			AM.UpdateConfig();

			Total += 30; //60
			Update(Total);

			AM.Start();

			Total += 40; //100
			Update(Total);
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

			_canAdvanceFromLogin = Setup.PasswordsMatch && Setup.Username != null && Setup.Username.Length > 0;
			UpdateNavigationButtons();
		}

		private void OnConfigureRemoteAccess_Click(object sender, RoutedEventArgs e)
		{
			_canAdvanceFromRemoteAccess = false;
			UpdateNavigationButtons();

			string CurrentUser = Permissions.GetCurrentUser();
			string Username = Setup.StartupMode == Installer.StartupMode.Service ? "NT AUTHORITY\\NetworkService" : CurrentUser;

			string certPath = Setup.TlsCertPath;
			string keyPath = Setup.TlsKeyPath;

			bool IsOk = Tls.Configure( SettingsPublisher.GetRootSettingsPath( true ), Setup.Hostname, Setup.TlsContact, Setup.TlsMode, Username,
				out certPath, out keyPath);
			
			if(IsOk)
			{
				if (certPath != null) Setup.TlsCertPath = certPath;
				if (keyPath != null) Setup.TlsKeyPath = keyPath;

				MessageBox.Show($"Successfully configured!");
				_canAdvanceFromRemoteAccess = true;
				UpdateNavigationButtons();
			}
		}

		private void TlsModeCombo_SelectionChanged(object sender, SelectionChangedEventArgs e)
		{
			_canAdvanceFromRemoteAccess = false;

			// Show/hide fields based on selected TLS mode
			if (TlsContactLabel == null) return; // Not yet initialized

			var selectedItem = TlsModeCombo.SelectedItem as ComboBoxItem;
			var tag = selectedItem?.Tag?.ToString();

			bool showEmail = (tag == "LetsEncrypt");
			bool showCertFields = (tag == "Manual");

			TlsContactLabel.Visibility = showEmail ? Visibility.Visible : Visibility.Collapsed;
			TlsContactBox.Visibility = showEmail ? Visibility.Visible : Visibility.Collapsed;
			TlsCertLabel.Visibility = showCertFields ? Visibility.Visible : Visibility.Collapsed;
			TlsCertBox.Visibility = showCertFields ? Visibility.Visible : Visibility.Collapsed;
			TlsKeyLabel.Visibility = showCertFields ? Visibility.Visible : Visibility.Collapsed;
			TlsKeyBox.Visibility = showCertFields ? Visibility.Visible : Visibility.Collapsed;
			TlsAdminHint.Visibility = showEmail ? Visibility.Visible : Visibility.Collapsed;

			UpdateNavigationButtons();
		}

		private void StartupMode_SelectionChanged(object sender, SelectionChangedEventArgs e)
		{
			if (StartupAdminHint == null) return;

			var selectedItem = StartupMode.SelectedItem as ComboBoxItem;
			var tag = selectedItem?.Tag?.ToString();
			StartupAdminHint.Visibility = (tag == "Service") ? Visibility.Visible : Visibility.Collapsed;
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
