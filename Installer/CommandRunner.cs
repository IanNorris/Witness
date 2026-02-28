using System;
using System.Linq;
using System.Management.Automation;
using System.Windows;

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

				try
				{
					var Result = PS.Invoke();

					if (Result != null)
					{
						var ConvertedResult = Result.Select(s => s?.ToString()).Where(s => !string.IsNullOrWhiteSpace(s)).ToList();

						if (PS.Streams.Error != null)
						{
							var ConvertedErrors = PS.Streams.Error.Select(s => s?.ToString()).Where(s => !string.IsNullOrWhiteSpace(s)).ToList();

							ConvertedResult.AddRange(ConvertedErrors);
						}

						return ConvertedResult.ToArray();
					}
				}
				catch(Exception)
				{
					// Caller handles errors
				}

				return new string[0];
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
