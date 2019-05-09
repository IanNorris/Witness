using Newtonsoft.Json;
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Data;
using System.Drawing;
using System.Dynamic;
using System.Linq;
using System.Net;
using System.Net.Http;
using System.Net.Http.Headers;
using System.Text;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace WitnessClient
{
	public class Profile
	{
		public string csrf { get; set; }
	}

	public partial class Login : Form
	{
		public string Hostname;
		public string Username;

		public Login()
		{
			InitializeComponent();

			Hostname = HostnameTextBox.Text = Properties.Settings.Default.Hostname;
			Username = UsernameTextBox.Text = Properties.Settings.Default.Username;


		}

		private void OK_Click(object sender, EventArgs e)
		{
			DialogResult = DialogResult.OK;

			Properties.Settings.Default.Hostname = Hostname = HostnameTextBox.Text;
			Properties.Settings.Default.Username = Username = UsernameTextBox.Text;
			Properties.Settings.Default.Save();

			dynamic LoginData = new ExpandoObject();
			LoginData.username = Username;
			LoginData.password = PasswordTextBox.Text;

			WitnessRest.SendQuery(true, "/auth/login", LoginData, false, false, new Action<Task<HttpResponseMessage>>( (response) =>
			{
				try
				{
					if (response.Result.StatusCode == HttpStatusCode.OK)
					{
						dynamic DummyData = new ExpandoObject();
						WitnessRest.SendQuery(true, "/auth/profile", DummyData, true, false, new Action<Task<HttpResponseMessage>>((responseProfile) =>
						{
							try
							{
								var profileResult = response.Result.Content.ReadAsStringAsync();
								profileResult.Wait();

								var Profile = JsonConvert.DeserializeObject<Profile>(profileResult.Result);

								Invoke(new Action(() =>
								{
									WitnessRest.SetCSRF(Profile.csrf);

									OK.Enabled = true;

									Close();
								}));
							}
							catch
							{

							}
						}));
					}
					else
					{
						Invoke(new Action(() =>
						{
							MessageBox.Show(response.Result.ReasonPhrase + "\n" + response.Result.Content.ReadAsStringAsync().Result, "Error", MessageBoxButtons.OK, MessageBoxIcon.Error);
						}));
					}
				}
				catch
				{
					Invoke(new Action(() =>
					{
						MessageBox.Show(response.Exception.ToString(), "Error", MessageBoxButtons.OK, MessageBoxIcon.Error);
					}));
				}
			}),
			new Action<IEnumerable<Cookie>>(cookies =>
			{
				var CookieString = JsonConvert.SerializeObject(cookies.First());

				WitnessRest.SetSessionCookieString(CookieString);

				Properties.Settings.Default.SessionToken = CookieString;
				Properties.Settings.Default.Save();
			}));
		}
	}
}
