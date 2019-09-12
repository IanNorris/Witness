using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Linq;
using System.Security;

namespace Installer
{
	public enum CertificateMode
	{
		[Description("Let's Encrypt with port 80 forwarded to this device (Recommended)")]
		LetsEncryptAuto,
		[Description("Let's Encrypt with manual setup")]
		LetsEncryptManual,
		[Description("Manual certificate management")]
		Manual,
		[Description("No security")]
		NoSecurity
	}

	public enum StartupMode
	{
		[Description("Run as a Windows service (Recommended)")]
		Service,
		[Description("Run as the current user")]
		Task,
		[Description("Don't run at startup")]
		Manual
	}

	public class Setup
	{
		public string Username { get; set; }
		public string Password { get; set; }
		public string PasswordConfirm { get; set; }
		public bool PasswordsMatch { get; set; } = false;
		public bool DeleteAllAdminAccounts { get; set; } = false;

		[SettingName("server_startup_mode")]
		public StartupMode StartupMode { get; set; } = StartupMode.Service;

		[SettingName("server_restart_on_failure")]
		public bool? RestartOnFailure { get; set; } = true;

		[SettingName("server_hostname")]
		public string Hostname { get; set; } = null;

		[SettingName("server_tls_mode")]
		public CertificateMode TlsMode { get; set; } = CertificateMode.LetsEncryptAuto;

		[SettingName("server_tls_contact")]
		public string TlsContact { get; set; } = null;

		[SettingName("server_root")]
		public string WebRoot { get; set; } = null;

		[SettingName("server_cache")]
		public string Cache { get; set; } = "C:\\WitnessCache";

		[SettingName("processing_threads")]
		public uint ThreadCount { get; set; } = 0;

		[SettingName("clip_lead_in")]
		public float ClipLeadIn { get; set; } = 8.0f;

		public List<Setting> GetSettings()
		{
			var Settings = new List<Setting>();

			var Properties = this.GetType().GetProperties().Where(prop => Attribute.IsDefined(prop, typeof(SettingName)));
			foreach( var Prop in Properties )
			{
				var PropAttribute = Prop.GetCustomAttributes(typeof(SettingName), false)[0] as SettingName;

				var NewSettingName = PropAttribute.Name;
				var StringValue = "";
				var Value = GetType().GetProperty(Prop.Name).GetValue(this);
				if(Value != null)
				{
					StringValue = Value.ToString();
				}

				Settings.Add(new Setting { Name = NewSettingName, Value = StringValue });
			}

			return Settings;
		}

		public void SetSettings(List<Setting> Settings)
		{
			var Properties = this.GetType().GetProperties().Where(prop => Attribute.IsDefined(prop, typeof(SettingName)));
			foreach (var Prop in Properties)
			{
				var PropAttribute = Prop.GetCustomAttributes(typeof(SettingName), false)[0] as SettingName;

				foreach( var setting in Settings )
				{
					if( string.Compare( setting.Name, PropAttribute.Name, true ) == 0 )
					{
						object Value = null;

						if( Prop.PropertyType == typeof(string) )
						{
							Value = setting.Value;
						}
						else if (Prop.PropertyType == typeof(bool))
						{
							Value = bool.Parse(setting.Value);
						}
						else if (Prop.PropertyType == typeof(bool?))
						{
							Value = bool.Parse(setting.Value);
						}
						else if (Prop.PropertyType == typeof(ushort))
						{
							Value = ushort.Parse(setting.Value);
						}
						else if (Prop.PropertyType == typeof(uint))
						{
							Value = uint.Parse(setting.Value);
						}
						else if (Prop.PropertyType == typeof(float))
						{
							Value = float.Parse(setting.Value);
						}
						else if (Prop.PropertyType.IsEnum)
						{
							Value = Enum.Parse(Prop.PropertyType,setting.Value);
						}
						else
						{
							throw new InvalidCastException($"{Prop.PropertyType.Name} can not be converted");
						}

						GetType().GetProperty(Prop.Name).SetValue(this, Value);
					}
				}

				
			}
		}
	}
}
