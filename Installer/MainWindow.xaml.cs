using System.IO;
using System.Windows;
using WinForms = System.Windows.Forms;
using Liphsoft.Crypto.Argon2;

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

			Setup.Username = "hello";
			Setup.Password = "world";
			string InputPassword = Setup.Username + ":" + Setup.Password;

			var Hasher = new PasswordHasher( 3, 262144, 1, Argon2Type.Argon2d );

			string ResultingPassword = Hasher.Hash(InputPassword);
			//string ResultingPassword = PasswordHash.ArgonHashString(InputPassword, PasswordHash.StrengthArgon.Moderate);

			Login.CanSelectNextPage = false;
		}

		private void Wizard_Finish(object sender, Xceed.Wpf.Toolkit.Core.CancelRoutedEventArgs e)
		{
			var SP = new SettingsPublisher();
			SP.Settings.AddRange(Setup.GetSettings());
			SP.WriteSettings();
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

		private void OnPasswordChanged(object sender, RoutedEventArgs e)
		{
			if( sender == Password1 )
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

			Login.CanSelectNextPage = Setup.PasswordsMatch;
		}
	}
}