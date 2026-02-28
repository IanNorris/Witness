using Microsoft.Win32.TaskScheduler;
using System;
using System.Collections.Generic;
using System.Linq;
using System.IO;
using System.Threading.Tasks;
using System.Windows;
using System.Diagnostics;
using System.Threading;

namespace Installer
{
	public class AutomationManager
	{
		const string WitnessTaskScheduler_Startup = "WitnessCameraServer_Startup";
		const string WitnessTaskScheduler_KeepAlive = "WitnessCameraServer_KeepAlive";
		const string WitnessFirewallRule = "Allow Witness Camera Remote Access";

		StartupMode Startup;
		bool RestartOnCrash;
		string RootPath;

		public AutomationManager( StartupMode InStartup, bool InRestartOnCrash, string InRootPath )
		{
			Startup = InStartup;
			RestartOnCrash = InRestartOnCrash;
			RootPath = InRootPath;
		}

		public static void StopService()
		{
			try
			{
				string Messages = "";
				var Result = CommandRunner.RunCommandAndDetermineSuccess<CommandRunner.Status>($"net stop WitnessCameraServer", CommandRunner.Status.Failure, (msg, status) =>
				{
					Messages += msg;
					if (msg.Contains("is not started"))
					{
						return CommandRunner.Status.Success_AlreadyDone;
					}
					else if (msg.Contains("service name is invalid"))
					{
						return CommandRunner.Status.Success_AlreadyDone;
					}
					else if (msg.Contains("was stopped successfully."))
					{
						return CommandRunner.Status.Success_Done;
					}
					else if (msg.Contains("Access is denied"))
					{
						return CommandRunner.Status.Success_AlreadyDone;
					}
					return status;
				});
			}
			catch { /* Service may not exist or we lack permissions — not fatal */ }
		}

		private static void ConfigureFirewall()
		{
			try
			{
				string Server = Path.Combine(InstallerPaths.ExeDirectory, "WitnessServer.exe");

				CommandRunner.RunCommand($"Remove-NetFirewallRule -DisplayName \"{WitnessFirewallRule}\" -ErrorAction SilentlyContinue");
				CommandRunner.RunCommand($"(New-NetFirewallRule -DisplayName \"{WitnessFirewallRule}\" -Direction Inbound -Program \"{Server}\" -Action Allow).PrimaryStatus");
			}
			catch { }
		}

		public void UpdateConfig()
		{
			Uninstall();

			if ( Startup == StartupMode.Task )
			{
				try
				{
					var task = TaskService.Instance.NewTask();
					task.Settings.AllowDemandStart = true;
					task.Settings.AllowHardTerminate = true;
					task.Settings.ExecutionTimeLimit = TimeSpan.FromDays(30 * 365);
					task.Settings.MultipleInstances = TaskInstancesPolicy.StopExisting;
					task.RegistrationInfo.Description = "WitnessCamera server task to start the server running at startup.";
					task.Triggers.Add(new BootTrigger());
					task.Actions.Add(new ExecAction(Path.Combine(RootPath, "WitnessServer.exe")));

					TaskService.Instance.RootFolder.RegisterTaskDefinition(WitnessTaskScheduler_Startup, task);
				}
				catch (Exception e)
				{
					Application.Current.Dispatcher.Invoke(() => MessageBox.Show("Could not create scheduled task. You may need administrator privileges.\n\n" + e.Message,
						"Task Scheduler", MessageBoxButton.OK, MessageBoxImage.Warning));
				}
			}
			else if( Startup == StartupMode.Service )
			{
				string Server = Path.Combine(InstallerPaths.ExeDirectory, "WitnessServer.exe");
				CommandRunner.RunCommand($"& \"{Server}\" /installservice");
			}

			ConfigureFirewall();
		}

		public void Start()
		{
			try
			{
				if (Startup == StartupMode.Task)
				{
					var Task = TaskService.Instance.FindTask(WitnessTaskScheduler_Startup);
					if (Task != null)
					{
						Task.Run();

						int MaxWait = 30;
						bool TaskResult = false;

						do
						{
							Task = TaskService.Instance.FindTask(WitnessTaskScheduler_Startup);
							if (Task?.State == TaskState.Running)
							{
								TaskResult = true;
							}
							else
							{
								Thread.Sleep(1000);
							}
						} while (MaxWait-- > 0 && !TaskResult);
					}
				}
				else if (Startup == StartupMode.Service)
				{
					CommandRunner.RunCommand($"net start WitnessCameraServer");
				}
			}
			catch { }
		}

		public void Uninstall()
		{
			StopService();

			try { TaskService.Instance.RootFolder.DeleteTask(WitnessTaskScheduler_Startup, false); } catch { }
			try { TaskService.Instance.RootFolder.DeleteTask(WitnessTaskScheduler_KeepAlive, false); } catch { }

			try
			{
				string Server = Path.Combine(InstallerPaths.ExeDirectory, "WitnessServer.exe");
				CommandRunner.RunCommandAndDetermineSuccess<CommandRunner.Status>($"& \"{Server}\" /uninstallservice", CommandRunner.Status.Failure, (msg, status) =>
				{
					if (msg.Contains("Service not found"))
						return CommandRunner.Status.Success_AlreadyDone;
					else if (msg.Contains("Deleted service"))
						return CommandRunner.Status.Success_Done;
					return status;
				});
			}
			catch { /* Service may not exist or we lack permissions — not fatal */ }
		}
	}
}
