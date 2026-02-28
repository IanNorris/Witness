using System.Linq;
using System.IO;
using System.Security.AccessControl;

namespace Installer
{
	internal static class Permissions
	{
		public static string GetCurrentUser()
		{
			return System.Security.Principal.WindowsIdentity.GetCurrent().Name;
		}

		public static void GrantPermissions(string Folder, string Account, FileSystemRights Rights )
		{
			var AccessRule = new FileSystemAccessRule(Account, Rights, AccessControlType.Allow);

			var Files = Directory.EnumerateFileSystemEntries(Folder).ToList();
			Files.Add(Folder);
			foreach( var FileEntry in Files )
			{
				if (Directory.Exists(FileEntry))
				{
					var dirInfo = new DirectoryInfo(FileEntry);
					var dirSecurity = dirInfo.GetAccessControl();
					dirSecurity.AddAccessRule(AccessRule);
					dirInfo.SetAccessControl(dirSecurity);
				}
				else
				{
					var fileInfo = new FileInfo(FileEntry);
					var fileSecurity = fileInfo.GetAccessControl();
					fileSecurity.AddAccessRule(AccessRule);
					fileInfo.SetAccessControl(fileSecurity);
				}
			}
		}
	}
}
