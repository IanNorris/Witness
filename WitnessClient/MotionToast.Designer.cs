namespace WitnessClient
{
	partial class MotionToast
	{
		/// <summary>
		/// Required designer variable.
		/// </summary>
		private System.ComponentModel.IContainer components = null;

		/// <summary>
		/// Clean up any resources being used.
		/// </summary>
		/// <param name="disposing">true if managed resources should be disposed; otherwise, false.</param>
		protected override void Dispose(bool disposing)
		{
			if (disposing && (components != null))
			{
				components.Dispose();
			}
			base.Dispose(disposing);
		}

		#region Windows Form Designer generated code

		/// <summary>
		/// Required method for Designer support - do not modify
		/// the contents of this method with the code editor.
		/// </summary>
		private void InitializeComponent()
		{
			this.components = new System.ComponentModel.Container();
			this.FadeTimer = new System.Windows.Forms.Timer(this.components);
			this.StreamImage = new System.Windows.Forms.PictureBox();
			((System.ComponentModel.ISupportInitialize)(this.StreamImage)).BeginInit();
			this.SuspendLayout();
			// 
			// FadeTimer
			// 
			this.FadeTimer.Enabled = true;
			this.FadeTimer.Interval = 16;
			this.FadeTimer.Tick += new System.EventHandler(this.Timer_Tick);
			// 
			// StreamImage
			// 
			this.StreamImage.Dock = System.Windows.Forms.DockStyle.Fill;
			this.StreamImage.Location = new System.Drawing.Point(0, 0);
			this.StreamImage.Name = "StreamImage";
			this.StreamImage.Size = new System.Drawing.Size(373, 338);
			this.StreamImage.SizeMode = System.Windows.Forms.PictureBoxSizeMode.StretchImage;
			this.StreamImage.TabIndex = 0;
			this.StreamImage.TabStop = false;
			this.StreamImage.Click += new System.EventHandler(this.StreamImage_Click);
			// 
			// MotionToast
			// 
			this.AutoScaleDimensions = new System.Drawing.SizeF(6F, 13F);
			this.AutoScaleMode = System.Windows.Forms.AutoScaleMode.Font;
			this.ClientSize = new System.Drawing.Size(373, 338);
			this.ControlBox = false;
			this.Controls.Add(this.StreamImage);
			this.FormBorderStyle = System.Windows.Forms.FormBorderStyle.None;
			this.Name = "MotionToast";
			this.Opacity = 0.3D;
			this.ShowIcon = false;
			this.ShowInTaskbar = false;
			this.SizeGripStyle = System.Windows.Forms.SizeGripStyle.Hide;
			this.TopMost = true;
			this.Shown += new System.EventHandler(this.MotionToast_Shown);
			((System.ComponentModel.ISupportInitialize)(this.StreamImage)).EndInit();
			this.ResumeLayout(false);

		}

		#endregion

		private System.Windows.Forms.PictureBox StreamImage;
		private System.Windows.Forms.Timer FadeTimer;
	}
}