using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Data;
using System.Drawing;
using System.Linq;
using System.Net.Http;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Forms;

namespace WitnessClient
{
	public partial class MotionToast : Form
	{
		private int Stage = 0;
		private bool IsClosing = false;
		private string CameraName;
		public int CamID { get; private set; } = 0;
		
		public MotionToast( int CamID, string Title )
		{
			this.CamID = CamID;

			Enabled = false;

			InitializeComponent();
			Opacity = 0;
			CameraName = Title;
		}

		protected override bool ShowWithoutActivation
		{
			get { return true; }
		}

		protected override CreateParams CreateParams
		{
			get
			{
				CreateParams BaseParams = base.CreateParams;

				const int WS_EX_NOACTIVATE = 0x08000000;
				const int WS_EX_TOPMOST = 0x00000008;
				BaseParams.ExStyle |= (int)(WS_EX_NOACTIVATE | WS_EX_TOPMOST);

				return BaseParams;
			}
		}

		public void StopShowing()
		{
			if(Stage == 0)
			{
				Stage = 2;
			}
			else if( Stage == 1)
			{
				Stage = 2;
			}
		}

		private void MotionToast_Shown(object sender, EventArgs e)
		{
			Width = (int)(SystemParameters.PrimaryScreenWidth * 0.2);
			Height = (int)((float)Width * 9.0 / 16.0);

			Location = new Point((int)SystemParameters.PrimaryScreenWidth - (Width + 50), (int)SystemParameters.PrimaryScreenHeight - (Height + 50));
		}

		private void Timer_Tick(object sender, EventArgs e)
		{
			WitnessRest.SendQuery(false, $"/camera/previewLarge/{CamID}", null, true, false, new Action<Task<HttpResponseMessage>>((response) =>
			{
				if( response != null && response.Status != TaskStatus.Faulted )
				{
					try
					{
						response.Wait();
						if ( !response.Result.IsSuccessStatusCode )
						{
							return;
						}
					}
					catch(Exception)
					{
						System.Threading.Thread.Sleep(1000);
						return;
					}

					bool Success = true;
					var image = response.Result.Content.ReadAsStreamAsync();
					try
					{
						image.Wait();
						if( image.Result == null )
						{
							Success = false;
						}
					}
					catch
					{
						Success = false;
					}

					if (!IsClosing && Success)
					{
						if (!IsDisposed)
						{
							try
							{
								Invoke(new Action(() =>
								{
									try
									{
										if (!IsClosing && Success)
										{
											if (!IsDisposed)
											{
												StreamImage.Image = Bitmap.FromStream(image.Result);
												var Graph = Graphics.FromImage(StreamImage.Image);
												Graph.SmoothingMode = System.Drawing.Drawing2D.SmoothingMode.AntiAlias;
												Graph.InterpolationMode = System.Drawing.Drawing2D.InterpolationMode.Bicubic;
												Graph.PixelOffsetMode = System.Drawing.Drawing2D.PixelOffsetMode.HighQuality;
												Graph.DrawString(CameraName, new Font("Arial", StreamImage.Image.Height / 20.0f), Brushes.GreenYellow, new PointF(StreamImage.Image.Height / 20.0f, StreamImage.Image.Height / 20.0f));
												Graph.Flush();
											}
										}
									}
									catch (ObjectDisposedException)
									{

									}
								}));
							}
							catch( ObjectDisposedException )
							{

							}
						}
					}
				}
			}));

			if (Stage == 0)
			{
				Opacity = Math.Min( 1.0, Opacity + 0.05 );
				if(Opacity >= 1.0)
				{
					Stage++;
				}
			}
			else if(Stage == 1)
			{
				//Stay here until we say otherwise
			}
			else if(Stage == 2)
			{
				Opacity = Math.Max(0, Opacity - 0.05);
				if(Opacity <= 0 )
				{
					IsClosing = true;
					Close();
				}
			}
		}

		private void StreamImage_Click(object sender, EventArgs e)
		{
			StopShowing();
		}

		private void MotionToast_FormClosing(object sender, FormClosingEventArgs e)
		{
			FadeTimer.Stop();
		}
	}
}
