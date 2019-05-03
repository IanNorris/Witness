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

		private string GetDBPath()
		{
			string AppData = Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData);
			return Path.Combine(AppData, "Witness", "server.db");
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
			ExternalProcess.Start();
			ExternalProcess.WaitForExit();
		}

		public bool ReadSettings()
		{
			EnsureDBCreated();

			SQLiteConnection database = null;

			var ConnectionString = new SQLiteConnectionString(
				GetDBPath(),
				true
			);

			try
			{
				database = new SQLiteConnection(ConnectionString);
				Settings = database.Table<Setting>().ToList();
			}
			catch (Exception e)
			{
				MessageBox.Show(e.ToString());
				return false;
			}

			return true;
		}

		public bool WriteSettings()
		{
			EnsureDBCreated();

			SQLiteConnection database = null;

			var ConnectionString = new SQLiteConnectionString(
				GetDBPath(),
				true
			);

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
			catch (Exception e)
			{
				MessageBox.Show(e.ToString());
				return false;
			}

			return true; 
		}

	}
}
