using System.Runtime.InteropServices;
using System.Security;

namespace Installer
{
	class SodiumLibrary
	{
		private const string Name = "libsodium";
		public const int crypto_pwhash_argon2id_ALG_ARGON2ID13 = 2;
		public const long crypto_pwhash_argon2id_OPSLIMIT_MODERARE = 3;
		public const int crypto_pwhash_MEMLIMIT_MODERATE = 268435456;

		static SodiumLibrary()
		{
			sodium_init();
		}

		[DllImport(Name, CallingConvention = CallingConvention.Cdecl)]
		internal static extern void sodium_init();

		[DllImport(Name, CallingConvention = CallingConvention.Cdecl)]
		internal static extern void randombytes_buf(byte[] buffer, int size);

		[SuppressUnmanagedCodeSecurity]
		[DllImport(Name, CallingConvention = CallingConvention.Cdecl)]
		internal static extern int crypto_pwhash_str(byte[] @out, [MarshalAs(UnmanagedType.LPStr)] string passwd, ulong passwdlen, ulong opslimit, uint memlimit);
	}
}
