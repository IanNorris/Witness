using Microsoft.Win32.TaskScheduler;
using System;
using System.Collections.Generic;
using System.Linq;
using System.IO;
using System.Threading.Tasks;

namespace Installer
{
	public class AutomationManager
	{
		const string WitnessTaskScheduler_Startup = "WitnessCameraServer_Startup";
		const string WitnessTaskScheduler_KeepAlive = "WitnessCameraServer_KeepAlive";
		const string WitnessTaskScheduler_CertUpdate = "WitnessCameraServer_CertUpdate";

		bool RunOnStartup;
		bool RestartOnCrash;
		bool CertificateUpdates;
		string RootPath;

		public AutomationManager( bool InRunOnStartup, bool InRestartOnCrash, bool InCertificateUpdates, string InRootPath )
		{
			RunOnStartup = InRunOnStartup;
			RestartOnCrash = InRestartOnCrash;
			CertificateUpdates = InCertificateUpdates;
			RootPath = InRootPath;
		}

		public void UpdateConfig()
		{
			Uninstall();

			if ( RunOnStartup )
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
		}

		public void Uninstall()
		{
			TaskService.Instance.RootFolder.DeleteTask(WitnessTaskScheduler_Startup, false);
			TaskService.Instance.RootFolder.DeleteTask(WitnessTaskScheduler_KeepAlive, false);
			TaskService.Instance.RootFolder.DeleteTask(WitnessTaskScheduler_CertUpdate, false);
		}
	}
}
