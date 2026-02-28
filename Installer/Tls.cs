using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using System.Windows;

namespace Installer
{
	internal static class Tls
	{
		private static string GetTlsOutputDir()
		{
			string dir = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData), "Witness", "tls");
			Directory.CreateDirectory(dir);
			return dir;
		}

		private static string FindOpenSSL()
		{
			// Check alongside the Installer exe first
			// Note: AppDomain.BaseDirectory points to temp extraction dir for single-file apps,
			// so use the actual exe path instead
			var exeDir = InstallerPaths.ExeDirectory;
			var localCandidate = Path.Combine(exeDir, "openssl.exe");
			if (File.Exists(localCandidate))
				return localCandidate;

			// Check PATH
			var pathDirs = Environment.GetEnvironmentVariable("PATH")?.Split(';') ?? Array.Empty<string>();
			foreach (var dir in pathDirs)
			{
				var candidate = Path.Combine(dir, "openssl.exe");
				if (File.Exists(candidate))
					return candidate;
			}

			// Check common locations
			var candidates = new[]
			{
				@"C:\Program Files\OpenSSL-Win64\bin\openssl.exe",
				@"C:\Program Files (x86)\OpenSSL-Win32\bin\openssl.exe",
			};

			foreach (var c in candidates)
			{
				if (File.Exists(c))
					return c;
			}

			return null;
		}

		private static (string certPath, string keyPath)? GenerateSelfSigned(string Hostname)
		{
			string openssl = FindOpenSSL();
			if (openssl == null)
			{
				MessageBox.Show("OpenSSL not found. Please install OpenSSL or ensure it's on your PATH.",
					"Error", MessageBoxButton.OK, MessageBoxImage.Error);
				return null;
			}

			string outputDir = GetTlsOutputDir();
			string certPath = Path.Combine(outputDir, "cert.pem");
			string keyPath = Path.Combine(outputDir, "key.pem");

			try
			{
				// Point OpenSSL at its config file (lives alongside the exe)
				string opensslDir = Path.GetDirectoryName(openssl);
				string cnfPath = Path.Combine(opensslDir, "openssl.cnf");

				var p = new Process();
				p.StartInfo.FileName = openssl;
				p.StartInfo.Arguments = $"req -x509 -newkey rsa:2048 -keyout \"{keyPath}\" -out \"{certPath}\" " +
					$"-days 365 -nodes -subj \"/CN={Hostname}\" " +
					$"-addext \"subjectAltName=DNS:{Hostname},DNS:localhost,IP:127.0.0.1\"";
				p.StartInfo.UseShellExecute = false;
				p.StartInfo.RedirectStandardError = true;
				p.StartInfo.CreateNoWindow = true;
				if (File.Exists(cnfPath))
					p.StartInfo.EnvironmentVariables["OPENSSL_CONF"] = cnfPath;
				p.Start();
				string stderr = p.StandardError.ReadToEnd();
				p.WaitForExit();

				if (p.ExitCode != 0)
				{
					MessageBox.Show($"Failed to generate self-signed certificate.\n\n{stderr}",
						"Error", MessageBoxButton.OK, MessageBoxImage.Error);
					return null;
				}

				return (certPath, keyPath);
			}
			catch (Exception e)
			{
				MessageBox.Show($"Failed to run OpenSSL.\n\n{e.Message}",
					"Error", MessageBoxButton.OK, MessageBoxImage.Error);
				return null;
			}
		}

		private static (string certPath, string keyPath)? RunCertbot(string HostnameAndPort, string Email)
		{
			// Strip port if present — certbot only wants the domain
			string Hostname = HostnameAndPort.Contains(':') ? HostnameAndPort.Split(':')[0] : HostnameAndPort;

			// Check if certbot already has a valid cert for this domain
			string certbotLive = Path.Combine(@"C:\Certbot\live", Hostname);
			string existingCert = Path.Combine(certbotLive, "fullchain.pem");
			string existingKey = Path.Combine(certbotLive, "privkey.pem");

			if (File.Exists(existingCert) && File.Exists(existingKey))
			{
				var result = MessageBox.Show(
					$"A Let's Encrypt certificate already exists for {Hostname}.\n\nUse the existing certificate?",
					"Certificate Found", MessageBoxButton.YesNo, MessageBoxImage.Question);

				if (result == MessageBoxResult.Yes)
					return (existingCert, existingKey);
			}

			string certbot = FindCertbot();

			if (certbot == null)
			{
				var result = MessageBox.Show(
					"certbot is not installed. Would you like to install it now via winget?",
					"certbot not found", MessageBoxButton.YesNo, MessageBoxImage.Question);

				if (result == MessageBoxResult.Yes)
				{
					try
					{
						var p = new Process();
						p.StartInfo.FileName = "winget";
						p.StartInfo.Arguments = "install EFF.Certbot --accept-source-agreements --accept-package-agreements";
						p.StartInfo.UseShellExecute = true;
						p.Start();
						p.WaitForExit();

						if (p.ExitCode == 0)
						{
							// Check common install location after winget install
							if (File.Exists(@"C:\Program Files\Certbot\bin\certbot.exe"))
								certbot = @"C:\Program Files\Certbot\bin\certbot.exe";
							else if (File.Exists(@"C:\Program Files (x86)\Certbot\bin\certbot.exe"))
								certbot = @"C:\Program Files (x86)\Certbot\bin\certbot.exe";
						}

						if (certbot == null)
						{
							MessageBox.Show("winget install completed but certbot was not found.\n\nTry installing manually from https://certbot.eff.org/",
								"Error", MessageBoxButton.OK, MessageBoxImage.Error);
							return null;
						}
					}
					catch
					{
						MessageBox.Show("winget is not available.\n\nInstall certbot manually:\n  https://certbot.eff.org/",
							"Error", MessageBoxButton.OK, MessageBoxImage.Error);
						return null;
					}
				}
				else
				{
					return null;
				}
			}

			try
			{
				var p = new Process();
				p.StartInfo.FileName = "cmd.exe";
				p.StartInfo.Arguments = $"/c \"\"{certbot}\" certonly --standalone -d {Hostname} --agree-tos --email {Email} --non-interactive || (pause & exit /b 1)\" & pause";
				p.StartInfo.UseShellExecute = true;
				p.StartInfo.Verb = "runas";
				p.Start();
				p.WaitForExit();

				if (p.ExitCode != 0)
				{
					MessageBox.Show("certbot failed. Check the console window for details.",
						"Error", MessageBoxButton.OK, MessageBoxImage.Error);
					return null;
				}

				string certPath = Path.Combine(certbotLive, "fullchain.pem");
				string keyPath = Path.Combine(certbotLive, "privkey.pem");

				if (!File.Exists(certPath) || !File.Exists(keyPath))
				{
					MessageBox.Show($"Certificate files not found at:\n{certbotLive}",
						"Error", MessageBoxButton.OK, MessageBoxImage.Error);
					return null;
				}

				return (certPath, keyPath);
			}
			catch (Exception e)
			{
				MessageBox.Show($"Failed to run certbot.\n\n{e.Message}",
					"Error", MessageBoxButton.OK, MessageBoxImage.Error);
				return null;
			}
		}

		private static string FindCertbot()
		{
			var pathDirs = Environment.GetEnvironmentVariable("PATH")?.Split(';') ?? Array.Empty<string>();
			foreach (var dir in pathDirs)
			{
				var candidate = Path.Combine(dir, "certbot.exe");
				if (File.Exists(candidate))
					return candidate;
			}

			var locations = new[]
			{
				@"C:\Program Files\Certbot\bin\certbot.exe",
				@"C:\Program Files (x86)\Certbot\bin\certbot.exe",
			};
			foreach (var c in locations)
			{
				if (File.Exists(c))
					return c;
			}

			return null;
		}

		public static bool SetupRenewalTask()
		{
			try
			{
				string certbot = FindCertbot();

				if (certbot == null)
					return false;

				// Create scheduled task via PowerShell
				string taskName = "Witness TLS Certificate Renewal";
				string deployHook = "curl -s -k -X POST https://localhost/debug/reload_tls -d \"{}\"";
				string psScript = $@"
					$action = New-ScheduledTaskAction -Execute '{certbot}' -Argument 'renew --deploy-hook ""{deployHook}""'
					$trigger = New-ScheduledTaskTrigger -Daily -At '03:00'
					$principal = New-ScheduledTaskPrincipal -UserId 'SYSTEM' -LogonType ServiceAccount -RunLevel Highest
					$settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -StartWhenAvailable
					Unregister-ScheduledTask -TaskName '{taskName}' -Confirm:$false -ErrorAction SilentlyContinue
					Register-ScheduledTask -TaskName '{taskName}' -Action $action -Trigger $trigger -Principal $principal -Settings $settings -Description 'Renew TLS cert for Witness'
				";

				var p = new Process();
				p.StartInfo.FileName = "powershell.exe";
				p.StartInfo.Arguments = $"-NoProfile -Command {psScript}";
				p.StartInfo.UseShellExecute = false;
				p.StartInfo.CreateNoWindow = true;
				p.Start();
				p.WaitForExit();

				return p.ExitCode == 0;
			}
			catch
			{
				return false;
			}
		}

		public static bool Configure(string SettingsRoot, string HostnameAndPort, string CertContact, CertificateMode Mode, string DomainUsername,
			out string CertPath, out string KeyPath)
		{
			CertPath = null;
			KeyPath = null;

			var HostnameSplit = HostnameAndPort.Trim().Split(new char[] { ':' });
			var Hostname = HostnameSplit[0];

			if (HostnameSplit.Length != 2 || Hostname.Length < 1)
			{
				MessageBox.Show("Hostname and port must both be specified and be valid.");
				return false;
			}

			if (!ushort.TryParse(HostnameSplit[1], out ushort Port))
			{
				MessageBox.Show("Port number is not valid.");
				return false;
			}

			switch (Mode)
			{
				case CertificateMode.SelfSigned:
				{
					var result = GenerateSelfSigned(Hostname);
					if (result == null) return false;
					CertPath = result.Value.certPath;
					KeyPath = result.Value.keyPath;
					break;
				}

				case CertificateMode.LetsEncrypt:
				{
					if (string.IsNullOrWhiteSpace(CertContact) || !CertContact.Contains("@") || !CertContact.Contains("."))
					{
						MessageBox.Show("A valid email address is required for Let's Encrypt.");
						return false;
					}

					var result = RunCertbot(Hostname, CertContact);
					if (result == null) return false;
					CertPath = result.Value.certPath;
					KeyPath = result.Value.keyPath;

					// Certbot creates its own renewal task. We add a deploy-hook
					// task to reload the server's TLS context after renewal.
					SetupRenewalTask();
					break;
				}

				case CertificateMode.Manual:
				{
					// Cert/key paths should already be set by the caller
					break;
				}

				case CertificateMode.NoSecurity:
				{
					var confirm = MessageBox.Show(
@"With no security your usernames, passwords and access to
the site will be available to any people or organizations
that may be surveilling you. This might include your
employer, internet service provider, hackers or government.

If you are happy to accept this risk, click Yes to continue.",
						"Are you sure?",
						MessageBoxButton.YesNoCancel, MessageBoxImage.Warning);

					if (confirm != MessageBoxResult.Yes)
						return false;
					break;
				}
			}

			return true;
		}
	}
}
