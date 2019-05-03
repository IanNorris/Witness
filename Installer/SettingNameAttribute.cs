using System;

namespace Installer
{
	[AttributeUsage(AttributeTargets.Property)]
	public class SettingName : Attribute
	{
		public string Name { get; set; }

		public SettingName( string InName )
		{
			Name = InName;
		}
	}
}
