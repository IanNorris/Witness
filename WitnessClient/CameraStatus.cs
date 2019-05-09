using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;

namespace WitnessClient
{
	public class CameraStatus
	{
		public string description { get; set; }
		public string name { get; set; }
		public string status { get; set; }
		public int enabled { get; set; }
		public bool recording { get; set; }
		public List<int> groups { get; set; }
		public int id { get; set; }
		public long lastTimestamp { get; set; }
	}
}
