using Newtonsoft.Json;
using System;
using System.Collections.Generic;
using System.Dynamic;
using System.Linq;
using System.Net.Http;
using System.Threading;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace WitnessClient
{
	static class Program
	{
		/// <summary>
		/// The main entry point for the application.
		/// </summary>
		[STAThread]
		static void Main()
		{
			Application.EnableVisualStyles();
			Application.SetCompatibleTextRenderingDefault(false);

			bool OK = false;
			if ( !string.IsNullOrEmpty(Properties.Settings.Default.SessionToken) )
			{
				AutoResetEvent WaitHandle = new AutoResetEvent(false);

				
				dynamic DummyData = new ExpandoObject();
				WitnessRest.SendQuery(true, "/auth/profile", DummyData, true, false, new Action<Task<HttpResponseMessage>>((response) =>
				{
					try
					{
						var profileResult = response.Result.Content.ReadAsStringAsync();
						profileResult.Wait();

						var Profile = JsonConvert.DeserializeObject<Profile>(profileResult.Result);

						WitnessRest.SetCSRF(Profile.csrf);

						OK = true;
					}
					catch
					{
						
					}

					WaitHandle.Set();
				}));

				WaitHandle.WaitOne();
			}
			
			if( !OK )
			{
				var login = new Login();
				if (login.ShowDialog() != DialogResult.OK)
				{
					return;
				}
			}

			Application.Run(new Main());
		}
	}
}
