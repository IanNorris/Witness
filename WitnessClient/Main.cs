using Newtonsoft.Json;
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Data;
using System.Drawing;
using System.Linq;
using System.Net;
using System.Net.Http;
using System.Text;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace WitnessClient
{
	public partial class Main : Form
	{
		public Dictionary<int, MotionToast> RecordingCameras = new Dictionary<int, MotionToast>();

		public Main()
		{
			InitializeComponent();

			LongPollCameaState();
		}

		public void LongPollCameaState()
		{
			WitnessRest.SendQuery(false, "/camera/enum", null, true, false, new Action<Task<HttpResponseMessage>>((response) =>
			{
				try
				{
					response.Wait();
				}
				catch
				{
					System.Threading.Thread.Sleep(250);
					LongPollCameaState();
					return;
				}

				if(response.Status == TaskStatus.Canceled )
				{
					System.Threading.Thread.Sleep(250);
					LongPollCameaState();
				}
				else if (response.Result.StatusCode == HttpStatusCode.OK)
				{
					string JsonContent = response.Result.Content.ReadAsStringAsync().Result;

					var status = JsonConvert.DeserializeObject<List<CameraStatus>>(JsonContent);

					Invoke(new Action(() => {
						foreach ( var cam in status )
						{
							MotionToast MT;
							if(RecordingCameras.TryGetValue(cam.id, out MT))
							{
								if(!cam.recording)
								{
									MT.StopShowing();
									RecordingCameras.Remove(cam.id);
								}
							}
							else
							{
								if( cam.recording )
								{
									var MTNew = new MotionToast(cam.id, cam.name);
									MTNew.Show();
									MTNew.FormClosed += MTNew_FormClosed;

									RecordingCameras.Add(cam.id, MTNew);								
								}
							}
						}
					}));

					System.Threading.Thread.Sleep(250);
					LongPollCameaState();
				}
				else
				{
					MessageBox.Show(response.Result.ReasonPhrase + "\n" + response.Result.Content.ReadAsStringAsync().Result, "Error", MessageBoxButtons.OK, MessageBoxIcon.Error);

					System.Threading.Thread.Sleep(5000);

					LongPollCameaState();
				}

			}));
		}

		private void MTNew_FormClosed(object sender, FormClosedEventArgs e)
		{
			RecordingCameras.Remove(((MotionToast)sender).CamID);
		}

		private void NotifyIcon_MouseClick(object sender, MouseEventArgs e)
		{
			if( e.Button == MouseButtons.Left )
			{
			//	var MT = new MotionToast();
			//	MT.Show();
			}
			else if (e.Button == MouseButtons.Right)
			{

			}
		}

		private void NotifyIcon_MouseDoubleClick(object sender, MouseEventArgs e)
		{
			if (e.Button == MouseButtons.Left)
			{

			}
		}

		private void Main_ResizeBegin(object sender, EventArgs e)
		{
			Hide();
		}

		private void Main_Shown(object sender, EventArgs e)
		{
			NotifyIcon.ShowBalloonTip(2500, "Witness status", "Witness has been started", ToolTipIcon.Info);
			Hide();
		}
	}
}
