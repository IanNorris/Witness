// BaichuanTest — Standalone diagnostic tool for Reolink Baichuan protocol
// Usage: BaichuanTest <host> <port> [username] [password]
//
// Attempts multiple connection strategies and reports what works.

#define NOMINMAX
#include <WinSock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/evp.h>

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <sstream>
#include <iomanip>

// ============================================================================
// Baichuan protocol constants
// ============================================================================

static constexpr uint32_t BC_MAGIC = 0x0ABCDEF0;
static constexpr uint16_t BC_CLASS_LEGACY = 0x6514;
static constexpr uint16_t BC_CLASS_MODERN = 0x6414;

static const uint8_t BC_XML_KEY[8] = { 0x1F, 0x2D, 0x3C, 0x4B, 0x5A, 0x69, 0x78, 0xFF };

#pragma pack(push, 1)
struct BcHeader20
{
	uint32_t Magic;
	uint32_t CmdId;
	uint32_t BodyLen;
	uint8_t  ChannelId;
	uint8_t  StreamType;
	uint16_t MsgNum;
	uint16_t ResponseCode;
	uint16_t MessageClass;
};
#pragma pack(pop)

static_assert(sizeof(BcHeader20) == 20, "BcHeader20 must be 20 bytes");

// ============================================================================
// Utilities
// ============================================================================

void HexDump(const uint8_t* data, size_t len, const char* label)
{
	printf("  %s (%zu bytes):\n  ", label, len);
	for (size_t i = 0; i < len && i < 64; i++)
	{
		printf("%02X ", data[i]);
		if ((i + 1) % 16 == 0) printf("\n  ");
	}
	if (len > 64) printf("... (%zu more)", len - 64);
	printf("\n");
}

void BcXorDecrypt(const uint8_t* in, uint8_t* out, size_t len, uint8_t offset)
{
	for (size_t i = 0; i < len; i++)
		out[i] = in[i] ^ BC_XML_KEY[(offset + i) % 8] ^ offset;
}

void BcXorEncrypt(const uint8_t* in, uint8_t* out, size_t len, uint8_t offset)
{
	BcXorDecrypt(in, out, len, offset); // Symmetric
}

std::string Md5Hex(const std::string& input)
{
	unsigned char digest[16];
	EVP_MD_CTX* ctx = EVP_MD_CTX_new();
	EVP_DigestInit_ex(ctx, EVP_md5(), nullptr);
	EVP_DigestUpdate(ctx, input.data(), input.size());
	EVP_DigestFinal_ex(ctx, digest, nullptr);
	EVP_MD_CTX_free(ctx);

	std::ostringstream ss;
	for (int i = 0; i < 16; i++)
		ss << std::uppercase << std::hex << std::setw(2) << std::setfill('0') << (int)digest[i];
	return ss.str();
}

// ============================================================================
// TCP helpers
// ============================================================================

SOCKET ConnectTCP(const char* host, int port)
{
	struct addrinfo hints{}, *result = nullptr;
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;

	char portStr[16];
	snprintf(portStr, sizeof(portStr), "%d", port);

	int ret = getaddrinfo(host, portStr, &hints, &result);
	if (ret != 0 || !result)
	{
		printf("  DNS resolution failed for %s\n", host);
		return INVALID_SOCKET;
	}

	SOCKET sock = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
	if (sock == INVALID_SOCKET)
	{
		freeaddrinfo(result);
		return INVALID_SOCKET;
	}

	DWORD timeout = 3000;
	setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
	setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof(timeout));

	ret = connect(sock, result->ai_addr, (int)result->ai_addrlen);
	freeaddrinfo(result);

	if (ret != 0)
	{
		closesocket(sock);
		return INVALID_SOCKET;
	}

	return sock;
}

bool SendAll(SOCKET sock, const uint8_t* data, size_t len)
{
	size_t sent = 0;
	while (sent < len)
	{
		int r = send(sock, (const char*)(data + sent), (int)(len - sent), 0);
		if (r <= 0) return false;
		sent += r;
	}
	return true;
}

int RecvSome(SOCKET sock, uint8_t* buf, size_t maxLen)
{
	return recv(sock, (char*)buf, (int)maxLen, 0);
}

// ============================================================================
// Test strategies
// ============================================================================

void TestRawProbe(const char* host, int port)
{
	printf("\n[1] Raw probe: connect and wait for camera to send first...\n");
	SOCKET sock = ConnectTCP(host, port);
	if (sock == INVALID_SOCKET)
	{
		printf("  FAILED: Connection refused\n");
		return;
	}
	printf("  TCP connected. Waiting 3s for camera greeting...\n");

	uint8_t buf[256];
	int r = RecvSome(sock, buf, sizeof(buf));
	if (r > 0)
	{
		printf("  Camera sent %d bytes first!\n", r);
		HexDump(buf, r, "Greeting");
	}
	else if (r == 0)
	{
		printf("  Camera closed connection immediately (no greeting)\n");
	}
	else
	{
		int err = WSAGetLastError();
		if (err == WSAETIMEDOUT)
			printf("  Timeout — camera waiting for client to send first\n");
		else
			printf("  recv error: %d\n", err);
	}
	closesocket(sock);
}

void TestBaichuanPlain(const char* host, int port, uint16_t respCode, uint16_t msgClass, const char* label)
{
	printf("\n[2] Baichuan plain (%s): respCode=0x%04X class=0x%04X...\n", label, respCode, msgClass);
	SOCKET sock = ConnectTCP(host, port);
	if (sock == INVALID_SOCKET)
	{
		printf("  FAILED: Connection refused\n");
		return;
	}

	BcHeader20 neg{};
	neg.Magic = BC_MAGIC;
	neg.CmdId = 1;
	neg.BodyLen = 0;
	neg.ChannelId = 0;
	neg.StreamType = 0;
	neg.MsgNum = 0;
	neg.ResponseCode = respCode;
	neg.MessageClass = msgClass;

	HexDump(reinterpret_cast<uint8_t*>(&neg), 20, "Sending");

	if (!SendAll(sock, reinterpret_cast<uint8_t*>(&neg), 20))
	{
		printf("  FAILED: send error\n");
		closesocket(sock);
		return;
	}

	uint8_t resp[256];
	int r = RecvSome(sock, resp, sizeof(resp));
	if (r > 0)
	{
		printf("  SUCCESS: Got %d bytes!\n", r);
		HexDump(resp, r, "Response");

		if (r >= 20)
		{
			BcHeader20* h = reinterpret_cast<BcHeader20*>(resp);
			printf("  Header: magic=0x%08X cmd=%u bodyLen=%u ch=%u resp=0x%04X class=0x%04X\n",
				h->Magic, h->CmdId, h->BodyLen, h->ChannelId, h->ResponseCode, h->MessageClass);

			if (h->BodyLen > 0 && r > 20)
			{
				size_t bodyStart = 20;
				// Check if modern (24-byte header)
				if (h->MessageClass == 0x6414 || h->MessageClass == 0x0000 || h->MessageClass == 0x6482)
					bodyStart = 24;

				if ((size_t)r > bodyStart)
				{
					size_t bodyLen = r - bodyStart;
					std::vector<uint8_t> dec(bodyLen);
					BcXorDecrypt(resp + bodyStart, dec.data(), bodyLen, h->ChannelId);
					printf("  Body (XOR decrypted): %.*s\n", (int)bodyLen, (char*)dec.data());
				}
			}
		}
	}
	else if (r == 0)
	{
		printf("  REJECTED: Connection closed by peer\n");
	}
	else
	{
		int err = WSAGetLastError();
		if (err == WSAETIMEDOUT)
			printf("  TIMEOUT: No response in 3s\n");
		else
			printf("  ERROR: recv error %d\n", err);
	}
	closesocket(sock);
}

void TestTLS(const char* host, int port)
{
	printf("\n[3] TLS handshake on port %d...\n", port);
	SOCKET sock = ConnectTCP(host, port);
	if (sock == INVALID_SOCKET)
	{
		printf("  FAILED: Connection refused\n");
		return;
	}

	SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
	SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);
	SSL* ssl = SSL_new(ctx);
	SSL_set_fd(ssl, (int)sock);

	int ret = SSL_connect(ssl);
	if (ret == 1)
	{
		printf("  TLS SUCCESS! Cipher: %s, Protocol: %s\n", SSL_get_cipher(ssl), SSL_get_version(ssl));

		// Try Baichuan over TLS
		BcHeader20 neg{};
		neg.Magic = BC_MAGIC;
		neg.CmdId = 1;
		neg.BodyLen = 0;
		neg.MsgNum = 0;
		neg.ResponseCode = 0xDC00;
		neg.MessageClass = BC_CLASS_LEGACY;

		printf("  Sending Baichuan negotiation over TLS...\n");
		int w = SSL_write(ssl, &neg, 20);
		if (w == 20)
		{
			uint8_t resp[256];
			int r = SSL_read(ssl, resp, sizeof(resp));
			if (r > 0)
			{
				printf("  Got %d bytes over TLS!\n", r);
				HexDump(resp, r, "TLS Response");
				if (r >= 20)
				{
					BcHeader20* h = reinterpret_cast<BcHeader20*>(resp);
					printf("  Header: magic=0x%08X cmd=%u bodyLen=%u resp=0x%04X class=0x%04X\n",
						h->Magic, h->CmdId, h->BodyLen, h->ResponseCode, h->MessageClass);
				}
			}
			else
			{
				int err = SSL_get_error(ssl, r);
				printf("  SSL_read failed (error %d)\n", err);
			}
		}
	}
	else
	{
		int err = SSL_get_error(ssl, ret);
		unsigned long sslErr = ERR_get_error();
		char errBuf[256];
		ERR_error_string_n(sslErr, errBuf, sizeof(errBuf));
		printf("  TLS FAILED (error %d): %s\n", err, errBuf);
	}

	SSL_shutdown(ssl);
	SSL_free(ssl);
	SSL_CTX_free(ctx);
	closesocket(sock);
}

void TestFullLogin(const char* host, int port, const char* user, const char* pass)
{
	printf("\n[5] Full login flow (no encryption)...\n");

	auto doNegotiation = [&](SOCKET sock) -> bool
	{
		BcHeader20 neg{};
		neg.Magic = BC_MAGIC;
		neg.CmdId = 1;
		neg.BodyLen = 0;
		neg.MsgNum = 0;
		neg.ResponseCode = 0x0000;
		neg.MessageClass = BC_CLASS_LEGACY;

		if (!SendAll(sock, reinterpret_cast<uint8_t*>(&neg), 20))
			return false;

		uint8_t resp[256];
		int r = RecvSome(sock, resp, sizeof(resp));
		if (r >= 20)
		{
			BcHeader20* h = reinterpret_cast<BcHeader20*>(resp);
			printf("  Neg OK: class=0x%04X resp=0x%04X\n", h->MessageClass, h->ResponseCode);
			return true;
		}
		return false;
	};

	auto tryLogin = [&](int cmdId, const std::string& xml, bool encrypt, const char* label)
	{
		printf("\n  --- %s (cmdId=%d, encrypt=%s) ---\n", label, cmdId, encrypt ? "yes" : "no");

		SOCKET sock = ConnectTCP(host, port);
		if (sock == INVALID_SOCKET) { printf("  Connect failed\n"); return; }

		if (!doNegotiation(sock)) { printf("  Negotiation failed\n"); closesocket(sock); return; }

		std::vector<uint8_t> body(xml.size());
		if (encrypt)
			BcXorEncrypt(reinterpret_cast<const uint8_t*>(xml.data()), body.data(), xml.size(), 0);
		else
			memcpy(body.data(), xml.data(), xml.size());

		BcHeader20 login{};
		login.Magic = BC_MAGIC;
		login.CmdId = cmdId;
		login.BodyLen = (uint32_t)xml.size();
		login.MsgNum = 1;
		login.ResponseCode = 0x0000;
		login.MessageClass = BC_CLASS_LEGACY;

		printf("  XML: %.80s...\n", xml.c_str());
		HexDump(reinterpret_cast<uint8_t*>(&login), 20, "Header");

		if (!SendAll(sock, reinterpret_cast<uint8_t*>(&login), 20) ||
			!SendAll(sock, body.data(), body.size()))
		{
			printf("  Send failed\n");
			closesocket(sock);
			return;
		}

		uint8_t resp[512];
		int r = RecvSome(sock, resp, sizeof(resp));
		if (r > 0)
		{
			printf("  RESPONSE: %d bytes\n", r);
			HexDump(resp, std::min(r, 64), "Data");
			if (r >= 20)
			{
				BcHeader20* lr = reinterpret_cast<BcHeader20*>(resp);
				printf("  magic=0x%08X cmd=%u body=%u resp=0x%04X class=0x%04X\n",
					lr->Magic, lr->CmdId, lr->BodyLen, lr->ResponseCode, lr->MessageClass);
				if (r > 20 && lr->BodyLen > 0)
				{
					size_t blen = std::min((size_t)(r - 20), (size_t)lr->BodyLen);
					// Try both decrypted and raw
					std::vector<uint8_t> dec(blen);
					BcXorDecrypt(resp + 20, dec.data(), blen, lr->ChannelId);
					printf("  Body (XOR): %.*s\n", (int)blen, (char*)dec.data());
					printf("  Body (raw): %.*s\n", (int)blen, (char*)(resp + 20));
				}
			}
		}
		else if (r == 0)
			printf("  Connection closed\n");
		else
		{
			int err = WSAGetLastError();
			printf("  %s (error %d)\n", err == WSAETIMEDOUT ? "TIMEOUT" : "ERROR", err);
		}
		closesocket(sock);
	};

	std::string passHash = Md5Hex(std::string(pass));

	// Format 1: Standard LoginUser XML with MD5 password
	std::string xml1 = "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\n"
		"<body>\n<LoginUser version=\"1.1\">\n"
		"<userName>" + std::string(user) + "</userName>\n"
		"<password>" + passHash + "</password>\n"
		"<userVer>1</userVer>\n"
		"</LoginUser>\n</body>\n";

	// Format 2: Same but with plain password
	std::string xml2 = "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\n"
		"<body>\n<LoginUser version=\"1.1\">\n"
		"<userName>" + std::string(user) + "</userName>\n"
		"<password>" + std::string(pass) + "</password>\n"
		"<userVer>1</userVer>\n"
		"</LoginUser>\n</body>\n";

	// Format 3: Minimal
	std::string xml3 = "<LoginUser><userName>" + std::string(user) +
		"</userName><password>" + passHash + "</password></LoginUser>";

	// Try combinations
	tryLogin(1, xml1, true, "MD5+XOR cmdId=1");
	tryLogin(1, xml1, false, "MD5+plain cmdId=1");
	tryLogin(2, xml1, true, "MD5+XOR cmdId=2");
	tryLogin(2, xml1, false, "MD5+plain cmdId=2");
	tryLogin(1, xml2, false, "PlainPass+plain cmdId=1");
	tryLogin(1, xml3, true, "Minimal+XOR cmdId=1");
	tryLogin(3, xml1, true, "MD5+XOR cmdId=3");
}

void TestPortScan(const char* host)
{
	printf("\n[4] Port scan...\n");
	int ports[] = { 9000, 8000, 34567, 34568, 80, 443, 554, 8080, 37777 };
	for (int p : ports)
	{
		SOCKET sock = ConnectTCP(host, p);
		if (sock != INVALID_SOCKET)
		{
			printf("  Port %5d: OPEN\n", p);
			closesocket(sock);
		}
		else
		{
			printf("  Port %5d: closed\n", p);
		}
	}
}

// ============================================================================

int main(int argc, char* argv[])
{
	if (argc < 3)
	{
		printf("Usage: BaichuanTest <host> <port> [username] [password]\n");
		printf("  Probes a Reolink camera's Baichuan protocol support.\n");
		return 1;
	}

	const char* host = argv[1];
	int port = atoi(argv[2]);
	const char* user = argc > 3 ? argv[3] : "admin";
	const char* pass = argc > 4 ? argv[4] : "";

	WSADATA wsa;
	WSAStartup(MAKEWORD(2, 2), &wsa);
	SSL_library_init();
	SSL_load_error_strings();

	printf("=== Baichuan Protocol Diagnostic ===\n");
	printf("Target: %s:%d (user=%s, pass=%s)\n", host, port, user, strlen(pass) > 0 ? "<present>" : "<empty>");

	TestRawProbe(host, port);
	TestBaichuanPlain(host, port, 0xDC00, BC_CLASS_LEGACY, "standard DC00");
	TestBaichuanPlain(host, port, 0xDC12, BC_CLASS_LEGACY, "DC12 (reolink_aio)");
	TestBaichuanPlain(host, port, 0x0000, BC_CLASS_LEGACY, "respCode=0");
	TestBaichuanPlain(host, port, 0xDC00, BC_CLASS_MODERN, "modern class");
	TestTLS(host, port);
	TestPortScan(host);
	TestFullLogin(host, port, user, pass);

	WSACleanup();
	printf("\n=== Done ===\n");
	return 0;
}
