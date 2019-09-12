using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Management.Automation;
using System.Text;
using System.Threading.Tasks;
using System.Windows;

namespace Installer
{
	internal static class Tls
	{
		const string AppId = "{790C242A-DAA6-46FF-8E04-B1EB280F3BA2}";

		

		public static bool UndoBindings(bool Listen, bool CertBinding, bool ACLBinding, string Hostname, ushort Port, string DomainUsername, string Thumbprint)
		{
			string Errors = "";

			if (Listen)
			{
				var IPListenSuccess = CommandRunner.RunCommandAndDetermineSuccess<CommandRunner.Status>("netsh http delete iplisten ipaddress=0.0.0.0", CommandRunner.Status.Failure, (line, current) =>
				{
					if (line.Contains("IP address successfully deleted"))
					{
						return CommandRunner.Status.Success_Done;
					}
					else if (line.Contains("Element not found"))
					{
						return CommandRunner.Status.Success_AlreadyDone;
					}

					return current;
				});

				if (IPListenSuccess == CommandRunner.Status.Failure)
				{
					Errors += "Unable to unbind IP listen from address 0.0.0.0.\n";
				}
			}

			if(CertBinding)
			{
				var CertSuccess = CommandRunner.RunCommandAndDetermineSuccess<CommandRunner.Status>($"netsh http delete sslcert ipport=0.0.0.0:{Port}", CommandRunner.Status.Failure, (line, current) =>
				{
					if (line.Contains("SSL Certificate successfully deleted"))
					{
						return CommandRunner.Status.Success_Done;
					}
					else if (line.Contains("cannot find the file specified"))
					{
						return CommandRunner.Status.Success_AlreadyDone;
					}

					return current;
				});

				if (CertSuccess == CommandRunner.Status.Failure)
				{
					Errors += $"Unable to unbind certificate from address 0.0.0.0:{Port}.\n";
				}
			}

			if (ACLBinding)
			{
				var ACLSuccess = CommandRunner.RunCommandAndDetermineSuccess<CommandRunner.Status>($"netsh http delete urlacl url=https://{Hostname}:{Port}/", CommandRunner.Status.Failure, (line, current) =>
				{
					if (line.Contains("URL reservation successfully deleted"))
					{
						return CommandRunner.Status.Success_Done;
					}
					else if (line.Contains("cannot find the file specified"))
					{
						return CommandRunner.Status.Success_AlreadyDone;
					}

					return current;
				});

				if (ACLSuccess == CommandRunner.Status.Failure)
				{
					Errors += $"Unable to unbind ACL for hostname {Hostname}:{Port}.\n";
				}
			}

			if (Errors.Length > 0)
			{
				MessageBox.Show($"Error during uninstallation. The application may not be fully uninstalled.\n\n{Errors}", "Error during uninstallation", MessageBoxButton.OK, MessageBoxImage.Warning);
			}
			return Errors.Length == 0;
		}

		public static bool ConfigureBindings( string Hostname, ushort Port, string DomainUsername, string Thumbprint )
		{
			string Errors = "";

			UndoBindings(true, true, true, Hostname, Port, DomainUsername, Thumbprint);

			var IPListenSuccess = CommandRunner.RunCommandAndDetermineSuccess<CommandRunner.Status>("netsh http add iplisten ipaddress=0.0.0.0", CommandRunner.Status.Failure, (line, current) =>
			{
				if (line.Contains("IP address successfully added"))
				{
					return CommandRunner.Status.Success_Done;
				}
				else if (line.Contains("already exists"))
				{
					return CommandRunner.Status.Success_AlreadyDone;
				}

				return current;
			});

			if (IPListenSuccess == CommandRunner.Status.Failure)
			{
				Errors += "Unable to bind IP listen from address 0.0.0.0.";
			}

			var CertSuccess = CommandRunner.RunCommandAndDetermineSuccess<CommandRunner.Status>($"netsh http add sslcert ipport=0.0.0.0:{Port} certhash={Thumbprint} \"appid={AppId}\"", CommandRunner.Status.Failure, (line, current) =>
			{
				if (line.Contains("SSL Certificate successfully added"))
				{
					return CommandRunner.Status.Success_Done;
				}
				else if (line.Contains("already exists"))
				{
					return CommandRunner.Status.Success_AlreadyDone;
				}

				return current;
			});

			if (CertSuccess == CommandRunner.Status.Failure)
			{
				Errors += "Unable to bind certificate to address 0.0.0.0.\n";
			}

			var ACLSuccess = CommandRunner.RunCommandAndDetermineSuccess<CommandRunner.Status>($"netsh http add urlacl url=https://{Hostname}:{Port}/ user=\"{DomainUsername}\"", CommandRunner.Status.Failure, (line, current) =>
			{
				if (line.Contains("URL reservation successfully added"))
				{
					return CommandRunner.Status.Success_Done;
				}
				else if (line.Contains("already exists"))
				{
					return CommandRunner.Status.Success_AlreadyDone;
				}

				return current;
			});

			if (ACLSuccess == CommandRunner.Status.Failure)
			{
				Errors += $"Unable to bind ACL for user {DomainUsername} and hostname {Hostname}:{Port}.\n";
			}

			if (Errors.Length > 0)
			{
				MessageBox.Show($"Error during uninstallation. The application may not be fully uninstalled.\n\n{Errors}", "Error during uninstallation", MessageBoxButton.OK, MessageBoxImage.Warning);
			}
			return Errors.Length == 0;
		}
		
		private static string GetMostRecentCertificate( string Hostname )
		{
			using (PowerShell PS = PowerShell.Create())
			{
				PS.AddScript($"Get-ChildItem -path cert:\\LocalMachine\\My -recurse | where {{ $_.Subject -match \"CN\\={Hostname}\" -and $_.NotAfter -ge (Get-Date) }}| Sort-Object -Descending {{ $_.NotAfter }} | Select-Object -Index 0 | ForEach-Object {{ $_.Thumbprint }}");
				var Result = PS.Invoke();
				if (Result.Count == 1)
				{
					string Thumbprint = Result[0].ToString();
					if (Thumbprint.Length > 10)
					{
						return Thumbprint;
					}
				}
			}

			return null;
		}

		private static bool CheckCertificateExists( string Hostname )
		{
			return GetMostRecentCertificate(Hostname) != null;
		}

		private static bool RunAutomaticWACS( string Hostname, string CertContact )
		{
			var WACS = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "WACS", "wacs.exe");
			try
			{
				string TempScript = System.IO.Path.GetTempPath() + Guid.NewGuid().ToString() + ".bat";

				File.WriteAllText(TempScript, $"@echo off\n\"{WACS}\" --target manual --host \"{Hostname}\" --emailaddress \"{CertContact}\" --accepttos --centralsslstore\nIF %ERRORLEVEL% NEQ 0 ( pause & exit / b %ERRORLEVEL% )");

				var P = new Process();
				P.StartInfo.FileName = "cmd.exe";
				P.StartInfo.Arguments = $"/C \"{TempScript}\"";
				P.StartInfo.UseShellExecute = false;
				P.Start();

				if (P == null)
				{
					return false;
				}

				P.WaitForExit();

				File.Delete(TempScript);

				if (P.ExitCode == 0)
				{
					return true;
				}
				else
				{
					MessageBox.Show($"WACS failed. Check the output from the window.");
					return false;
				}
			}
			catch (Exception e)
			{
				MessageBox.Show($"Unable to run WACS!\n\n{e.Message}");
			}

			return true;
		}

		private static bool RunManualWACS( string Hostname )
		{
			var WACS = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "WACS", "wacs.exe");
			try
			{
				var P = new Process();
				P.StartInfo.FileName = WACS;

				P.StartInfo.UseShellExecute = false;
				P.Start();

				if (P == null)
				{
					return false;
				}

				P.WaitForExit();

				if (P.ExitCode == 0)
				{
					return true;
				}
				else
				{
					MessageBox.Show($"WACS failed. Check the output from the window.");
					return false;
				}
			}
			catch (Exception e)
			{
				MessageBox.Show($"Unable to run WACS!\n\n{e.Message}");
			}

			return true;
		}

		public static bool Configure( string SettingsRoot, string HostnameAndPort, string CertContact, CertificateMode Mode, string DomainUsername )
		{
			var IsOk = false;

			var HostnameSplit = HostnameAndPort.Trim().Split(new char[] { ':' });
			var Hostname = HostnameSplit[0];
			ushort Port;

			string Thumbprint = GetMostRecentCertificate(Hostname);

			if (Mode == CertificateMode.LetsEncryptAuto || Mode == CertificateMode.LetsEncryptManual)
			{
				if (CertContact == null || !(CertContact.Length >= 4 && CertContact.Contains("@") && CertContact.Contains(".") && !CertContact.Contains(" ") && !CertContact.Contains("\t") && !CertContact.Contains("\"")))
				{
					MessageBox.Show("Email address is not valid.");
					return false;
				}
			}

			if (HostnameSplit.Length == 2 && Hostname.Length >= 1 && !Hostname.Contains(" ") && !Hostname.Contains("\t") && !Hostname.Contains("\""))
			{
				if (!ushort.TryParse(HostnameSplit[1], out Port))
				{
					MessageBox.Show($"Port number specified is not valid.");
					return false;
				}
			}
			else
			{
				MessageBox.Show($"Hostname and port must both be specified and be valid.");
				return false;
			}

			switch (Mode)
			{
				case CertificateMode.LetsEncryptAuto:
					IsOk = RunAutomaticWACS(Hostname, CertContact );
					break;

				case CertificateMode.LetsEncryptManual:
					IsOk = RunManualWACS(Hostname);
					break;

				case CertificateMode.Manual:
					if (Thumbprint != null)
					{
						MessageBox.Show($"Certificate for {Hostname} was not found.\nEnsure it is installed in the local machine Personal certificate store.");
					}
					break;

				case CertificateMode.NoSecurity:
					IsOk = MessageBox.Show(
@"With no security your usernames, passwords and access to 
the site will be available to any people or organizations 
that may be surveilling you. This might include your 
employer, internet service provider, hackers or government.
This is true for all websites that do not use https.

If you are happy to accept this risk (https is free 
now you know!), click Yes to continue.",
"Are you sure?",
					MessageBoxButton.YesNoCancel, MessageBoxImage.Warning) == MessageBoxResult.Yes;
					break;
			}

			if ( !IsOk )
			{
				return false;
			}

			IsOk = CheckCertificateExists(Hostname);

			if (!IsOk)
			{
				MessageBox.Show($"A suitable certificate for {Hostname} was not found.");
				return false;
			}

			ConfigureBindings( Hostname, Port, DomainUsername, Thumbprint );

			return IsOk;
		}
	}
}
