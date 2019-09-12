using System.ComponentModel;
using System.Windows;

namespace Installer
{
	/// <summary>
	/// Interaction logic for InstallProgress.xaml
	/// </summary>
	public partial class InstallProgress : Window
	{
		private BackgroundWorker backgroundWorker;

		public delegate void StatusUpdateDelegate(int Progress);
		public delegate void InstallWorkDelegate(StatusUpdateDelegate UpdateDelegate);

		private InstallWorkDelegate WorkDelegate;

		public InstallProgress( InstallWorkDelegate WorkDelegate )
		{
			this.WorkDelegate = WorkDelegate;

			InitializeComponent();

			backgroundWorker = new BackgroundWorker();
			backgroundWorker.WorkerReportsProgress = true;
			backgroundWorker.DoWork += BackgroundWorker_DoWork;
			backgroundWorker.ProgressChanged += BackgroundWorker_ProgressChanged;
			backgroundWorker.RunWorkerCompleted += BackgroundWorker_RunWorkerCompleted;
			backgroundWorker.RunWorkerAsync();
		}

		private void BackgroundWorker_RunWorkerCompleted(object sender, RunWorkerCompletedEventArgs e)
		{
			Status.Value = 100;
			StatusText.Text = "100%";
			Close();
		}

		private void BackgroundWorker_ProgressChanged(object sender, ProgressChangedEventArgs e)
		{
			Status.Value = e.ProgressPercentage;
			StatusText.Text = $"{e.ProgressPercentage}%";
		}

		private void BackgroundWorker_DoWork(object sender, DoWorkEventArgs e)
		{
			StatusUpdateDelegate UpdateStatus = status =>
			{
				(sender as BackgroundWorker).ReportProgress(status);
			};

			WorkDelegate( UpdateStatus );
		}
	}
}
