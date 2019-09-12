using SQLite;
using System;
using System.Collections.Generic;
using System.IO;
using System.Diagnostics;
using System.Windows;
using System.Security.Cryptography;
using System.Text;

namespace Installer
{
	public class Setting
	{
		public string Name { get; set; }
		public string Value { get; set; }
	}

	public class User
	{
		[PrimaryKey]
		public int UserUID { get; set; }
		public string Username { get; set; }
		public string DisplayName { get; set; }
		public string PasswordHash { get; set; }
		public int HashMethod { get; set; }
		public int Enabled { get; set; }
		public int Admin { get; set; }
	}

	public class SettingsPublisher
	{
		const int DefaultHashMethod = 0;

		public List<Setting> Settings { get; private set; } = new List<Setting>();

		public static string GetRootSettingsPath( bool CreateFolder )
		{
			var Root = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData), "Witness");
			if (CreateFolder)
			{
				Directory.CreateDirectory(Root);
			}
			return Root;
		}

		private string GetDBPath( bool CreateFolder )
		{
			string DBRoot = GetRootSettingsPath( CreateFolder );

			if( CreateFolder )
			{
				Directory.CreateDirectory(DBRoot);
			}

			return Path.Combine(DBRoot, "server.db");
		}

		public string GetWebRoot()
		{
			return Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "Web");
		}

		private void EnsureDBCreated()
		{
			string Server = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "WitnessServer.exe");

			Process ExternalProcess = new Process();
			ExternalProcess.StartInfo.FileName = Server;
			ExternalProcess.StartInfo.Arguments = "/createdb";
			ExternalProcess.StartInfo.WindowStyle = ProcessWindowStyle.Hidden;
			ExternalProcess.StartInfo.Verb = "runas";
			ExternalProcess.Start();
			ExternalProcess.WaitForExit();
		}

		public bool ReadSettings()
		{
			EnsureDBCreated();

			SQLiteConnection database = null;

			var ConnectionString = new SQLiteConnectionString(
				GetDBPath(false),
				true
			);

			bool FailedToOpen = false;
			try
			{
				database = new SQLiteConnection(ConnectionString);
				Settings = database.Table<Setting>().ToList();
			}
			catch (SQLiteException e)
			{
				FailedToOpen = e.Result == SQLite3.Result.CannotOpen;

				if (!FailedToOpen)
				{
					MessageBox.Show(e.ToString());
				}
				return false;
			}

			return true;
		}

		public bool WriteSettings( Setup Setup )
		{
			EnsureDBCreated();

			SQLiteConnection database = null;

			var ConnectionString = new SQLiteConnectionString(
				GetDBPath(true),
				true
			);

			bool FailedToOpen = false;
			try
			{
				database = new SQLiteConnection(ConnectionString);
				database.BeginTransaction();
				foreach (var Setting in Settings)
				{
					database.InsertOrReplace(Setting);
				}

				string FixedUsername = Setup.Username.ToLower().Trim();

				bool AlreadyExisted = true;
				User User = database.Find<User>(u => u.Username == FixedUsername);
				if (User == null)
				{
					AlreadyExisted = false;
					User = new User();
				}

				Setup.Username = FixedUsername;
				User.DisplayName = Setup.Username;

				

				var PasswordSalt = new byte[16];
				var RNG = new RNGCryptoServiceProvider();
				RNG.GetBytes(PasswordSalt);

				string HashedPasswordInput = Setup.Username + ":" + Setup.Password;
				var HasswordPasswordByteBuffer = Encoding.ASCII.GetBytes(HashedPasswordInput);

				var PasswordBuffer = new byte[128];

				SodiumLibrary.crypto_pwhash_str(PasswordBuffer, HashedPasswordInput, (ulong)HashedPasswordInput.Length, SodiumLibrary.crypto_pwhash_argon2id_OPSLIMIT_MODERARE, SodiumLibrary.crypto_pwhash_MEMLIMIT_MODERATE);

				string HashedPassword = Encoding.ASCII.GetString(PasswordBuffer).TrimEnd((Char)0);

				User.Username = Setup.Username;
				User.HashMethod = DefaultHashMethod;
				User.Enabled = 1;
				User.Admin = 1;
				User.PasswordHash = HashedPassword;

				if (AlreadyExisted)
				{
					database.Update(User);
				}
				else
				{
					database.Insert(User);
				}

				database.Commit();
			}
			catch (SQLiteException e)
			{
				FailedToOpen = e.Result == SQLite3.Result.CannotOpen;

				if (FailedToOpen)
				{
					MessageBox.Show("Unable to write settings. Installation failed.", "Error", MessageBoxButton.OK, MessageBoxImage.Error);
				}
				else
				{
					MessageBox.Show(e.ToString());
				}
				return false;
			}

			return true; 
		}

	}
}
