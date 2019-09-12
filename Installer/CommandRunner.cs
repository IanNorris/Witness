using System.Linq;
using System.Management.Automation;

namespace Installer
{
	internal class CommandRunner
	{
		public enum Status
		{
			Failure,
			Success_Done,
			Success_AlreadyDone
		}

		public delegate T DetermineSuccessDelegate<T>(string Input, T CurrentSuccess);

		public static string[] RunCommand(string Command)
		{
			using (PowerShell PS = PowerShell.Create())
			{
				PS.AddScript(Command);

				var Result = PS.Invoke();
				var ConvertedResult = Result.Select(s => s.ToString()).Where(s => !string.IsNullOrWhiteSpace(s)).ToList();

				var ConvertedErrors = PS.Streams.Error.Select(s => s.ToString()).Where(s => !string.IsNullOrWhiteSpace(s)).ToList();

				ConvertedResult.AddRange(ConvertedErrors);

				return ConvertedResult.ToArray();
			}
		}

		public static T RunCommandAndDetermineSuccess<T>(string Command, T Default, DetermineSuccessDelegate<T> IsSuccess)
		{
			string[] ResultLines = RunCommand(Command);

			T ResultValue = Default;
			foreach (var Line in ResultLines)
			{
				ResultValue = IsSuccess(Line, ResultValue);
			}

			return ResultValue;
		}
	}
}
