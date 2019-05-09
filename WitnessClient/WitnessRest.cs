using Newtonsoft.Json;
using System;
using System.Collections.Generic;
using System.Linq;
using System.Net;
using System.Net.Http;
using System.Text;
using System.Threading.Tasks;

namespace WitnessClient
{
	public static class WitnessRest
	{
		private static string CSRFToken = null;
		private static string SessionCookieString = null;
		private static Cookie StoredCookie = null;
		private static CookieContainer CookieStore = null;

		public static void SetCSRF( string CSRF )
		{
			CSRFToken = CSRF;
		}

		public static void SetSessionCookieString(string Cookie)
		{
			SessionCookieString = Cookie;
		}

		public static void SendQuery(bool Post, string QueryString, dynamic Data, bool UseSessionToken, bool UseCSRFToken, Action<Task<HttpResponseMessage>> Callback, Action<IEnumerable<Cookie>> CookieCallback = null )
		{
			string DataJson = Data != null ? JsonConvert.SerializeObject(Data) : null;

			if(string.IsNullOrEmpty(Properties.Settings.Default.SessionToken) )
			{
				if( !string.IsNullOrEmpty(SessionCookieString) )
				{
					Properties.Settings.Default.SessionToken = SessionCookieString;
				}
			}

			if (CookieStore == null)
			{
				CookieStore = new CookieContainer();
			}

			if ( UseSessionToken )
			{
				if (StoredCookie != null)
				{
					CookieStore.Add(StoredCookie);
				}
				else if ( StoredCookie == null && !string.IsNullOrEmpty(Properties.Settings.Default.SessionToken) )
				{
					var NewCookie = JsonConvert.DeserializeObject<Cookie>(Properties.Settings.Default.SessionToken);

					var Cookie = new Cookie(NewCookie.Name, NewCookie.Value);
					Cookie.Domain = NewCookie.Domain;
					Cookie.Expires = DateTime.Now.AddYears(10);
					Cookie.Path = "/";

					CookieStore.Add(Cookie);
					StoredCookie = Cookie;
				}
			}

			if( UseCSRFToken )
			{
				if(CSRFToken != null )
				{
					Data.csrf = CSRFToken;
				}
				else
				{
					//Query token
				}
			}
			
			var Handler = new HttpClientHandler();
			Handler.CookieContainer = CookieStore;
			var Client = new HttpClient(Handler);

			Client.BaseAddress = new Uri(Properties.Settings.Default.Hostname);
			Client.DefaultRequestHeaders.Accept.Clear();
			Client.DefaultRequestHeaders.Accept.Add(new System.Net.Http.Headers.MediaTypeWithQualityHeaderValue("application/json"));

			if (Post)
			{
				var Content = Client.PostAsync(QueryString, new StringContent(DataJson, Encoding.UTF8, "application/json"));
				Content.ContinueWith(new Action<Task<HttpResponseMessage>>( (response) =>
				{
					if( StoredCookie == null )
					{
						var CookieList = CookieStore.GetCookies(Client.BaseAddress).Cast<Cookie>();

						if (CookieList.Count() > 0)
						{
							if( CookieCallback != null )
							{
								CookieCallback(CookieList);
							}
						}
					}

					Callback(response);
				}));
			}
			else
			{
				var Content = Client.GetAsync(QueryString);
				Content.ContinueWith(new Action<Task<HttpResponseMessage>>(response =>
				{
					Callback(response);
				}));
			}
		}
	}
}
