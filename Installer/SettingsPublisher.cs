using SQLite;
using System;
using System.Collections.Generic;
using System.IO;
using System.Diagnostics;
using System.Windows;

namespace Installer
{
	public class Setting
	{
		public string Name { get; set; }
		public string Value { get; set; }
	}

	public class SettingsPublisher
	{
		public List<Setting> Settings { get; private set; } = new List<Setting>();

		private string GetDBPath( bool CreateFolder )
		{
			string DBRoot = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData), "Witness");

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

		public bool WriteSettings()
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
