// ReolinkBaichuanClient — Unofficial Reolink Baichuan protocol client
//
// DISCLAIMER: This is an UNOFFICIAL implementation based on community-researched
// protocol details. It is not endorsed by or affiliated with Reolink Technology Co., Ltd.
// Use at your own risk.
//
// Protocol reference: community reverse-engineering (apocaliss92/nodelink-js)

#include "ReolinkBaichuanClient.h"
#include <Log.h>

#include <lz4frame.h>

#ifdef _WIN32
#define NOMINMAX
#include <WinSock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#define closesocket close
#define INVALID_SOCKET (~(uintptr_t)0)
typedef int SOCKET;
#endif

#include <cstring>
#include <algorithm>
#include <sstream>
#include <iomanip>

// OpenSSL — MD5 + TLS for Baichuan over TLS (newer firmware)
#include <openssl/md5.h>
#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

namespace Witness {
namespace Camera {

// ============================================================================
// Baichuan protocol constants
// ============================================================================

static constexpr uint32_t BC_MAGIC = 0x0ABCDEF0; // Stored as F0 DE BC 0A in LE
static constexpr uint16_t BC_CLASS_LEGACY = 0x6514;
static constexpr uint16_t BC_CLASS_MODERN = 0x6414;

// Encryption negotiation codes
static constexpr uint16_t ENC_REQUEST_BC = 0xDC12;    // BC-XOR encryption supported (reolink_aio: "12dc")
static constexpr uint16_t ENC_RESPONSE_NONE = 0xDD00;
static constexpr uint16_t ENC_RESPONSE_XOR = 0xDD01;
static constexpr uint16_t ENC_RESPONSE_XOR2 = 0xDD12;
static constexpr uint16_t ENC_RESPONSE_AES = 0xDD02;
static constexpr uint16_t ENC_RESPONSE_AES2 = 0xDD03;

// BC-XOR key for XML encryption
static constexpr uint8_t BC_XML_KEY[8] = { 0x1F, 0x2D, 0x3C, 0x4B, 0x5A, 0x69, 0x78, 0xFF };

// AES-128-CFB IV: "0123456789abcdef"
static constexpr uint8_t AES_CFB_IV[16] = {
	0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37,
	0x38, 0x39, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66
};

// BcMedia magic values (little-endian uint32)
static constexpr uint32_t MAGIC_IFRAME_BASE = 0x63643030; // "00dc" as LE
static constexpr uint32_t MAGIC_PFRAME_BASE = 0x63643130; // "01dc" as LE

// LZ4 Frame magic
static constexpr uint8_t LZ4F_MAGIC[4] = { 0x04, 0x22, 0x4D, 0x18 };

// AI frame size TLV pattern
static constexpr uint8_t FRAME_SIZE_TLV[3] = { 0x03, 0x04, 0x00 };

// ============================================================================
// Baichuan header structures
// ============================================================================

#pragma pack(push, 1)
struct BcHeader20
{
	uint32_t Magic;         // 0x0ABCDEF0
	uint32_t CmdId;
	uint32_t BodyLen;
	uint8_t  ChannelId;
	uint8_t  StreamType;
	uint16_t MsgNum;
	uint16_t ResponseCode;
	uint16_t MessageClass;
};

struct BcHeader24
{
	uint32_t Magic;
	uint32_t CmdId;
	uint32_t BodyLen;
	uint8_t  ChannelId;
	uint8_t  StreamType;
	uint16_t MsgNum;
	uint16_t ResponseCode;
	uint16_t MessageClass;
	uint32_t PayloadOffset;
};
#pragma pack(pop)

static_assert(sizeof(BcHeader20) == 20, "BcHeader20 must be 20 bytes");
static_assert(sizeof(BcHeader24) == 24, "BcHeader24 must be 24 bytes");

// ============================================================================
// Utility functions
// ============================================================================

static bool HeaderHasPayloadOffset(uint16_t cls)
{
	return cls == 0x6414 || cls == 0x0000 || cls == 0x6482;
}

static std::string Md5HexUpper(const std::string& input)
{
	unsigned char digest[MD5_DIGEST_LENGTH];
	EVP_MD_CTX* ctx = EVP_MD_CTX_new();
	EVP_DigestInit_ex(ctx, EVP_md5(), nullptr);
	EVP_DigestUpdate(ctx, input.data(), input.size());
	EVP_DigestFinal_ex(ctx, digest, nullptr);
	EVP_MD_CTX_free(ctx);

	std::ostringstream ss;
	for (int i = 0; i < MD5_DIGEST_LENGTH; i++)
		ss << std::uppercase << std::hex << std::setw(2) << std::setfill('0') << (int)digest[i];
	return ss.str();
}

static std::string Md5Modern(const std::string& input)
{
	// Reolink uses 31-char truncated uppercase MD5 (matches reolink_aio md5_str_modern)
	return Md5HexUpper(input).substr(0, 31);
}

static void BcXorEncrypt(const uint8_t* in, uint8_t* out, size_t len, uint8_t offset)
{
	for (size_t i = 0; i < len; i++)
	{
		uint8_t key = BC_XML_KEY[(offset + i) % 8];
		out[i] = in[i] ^ key ^ offset;
	}
}

static void BcXorDecrypt(const uint8_t* in, uint8_t* out, size_t len, uint8_t offset)
{
	BcXorEncrypt(in, out, len, offset); // XOR is symmetric
}

// ============================================================================
// Winsock initialization
// ============================================================================

#ifdef _WIN32
struct WinsockInit
{
	WinsockInit() { WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa); }
	~WinsockInit() { WSACleanup(); }
};
static WinsockInit g_WinsockInit;
#endif

// ============================================================================
// ReolinkBaichuanClient implementation
// ============================================================================

ReolinkBaichuanClient::ReolinkBaichuanClient(const std::string& host, int port,
	const std::string& username, const std::string& password)
	: m_Host(host)
	, m_Port(port > 0 ? port : 9000)
	, m_Username(username)
	, m_Password(password)
{
}

ReolinkBaichuanClient::~ReolinkBaichuanClient()
{
	Stop();
}

void ReolinkBaichuanClient::Start()
{
	if (m_Running.load()) return;
	m_Running = true;
	m_Thread = std::thread(&ReolinkBaichuanClient::ThreadFunc, this);
}

void ReolinkBaichuanClient::Stop()
{
	m_Running = false;
	CleanupTls();
	if (m_Socket != (uintptr_t)INVALID_SOCKET)
	{
#ifdef _WIN32
		shutdown((SOCKET)m_Socket, SD_BOTH);
		closesocket((SOCKET)m_Socket);
#else
		shutdown((int)m_Socket, SHUT_RDWR);
		closesocket((int)m_Socket);
#endif
		m_Socket = (uintptr_t)INVALID_SOCKET;
	}
	if (m_Thread.joinable())
		m_Thread.join();
}

ReolinkDetectionState ReolinkBaichuanClient::GetDetections() const
{
	std::lock_guard<std::mutex> lock(m_DetectionMutex);
	return m_CurrentDetections;
}

std::string ReolinkBaichuanClient::GetLastError() const
{
	std::lock_guard<std::mutex> lock(m_ErrorMutex);
	return m_LastError;
}

void ReolinkBaichuanClient::ThreadFunc()
{
	int backoff = 5000;
	int consecutiveFailures = 0;

	while (m_Running.load())
	{
		if (Connect() && Authenticate() && RequestStream())
		{
			backoff = 5000;
			consecutiveFailures = 0;
			m_Connected = true;
			LOG_INFO("[Baichuan] Camera %s:%d connected%s, receiving detections",
				m_Host.c_str(), m_Port, m_UseTls ? " (TLS)" : "");
			ReadLoop();
			m_Connected = false;
		}
		else
		{
			consecutiveFailures++;

			// If plain TCP fails immediately on first attempt, try TLS
			if (!m_UseTls && consecutiveFailures == 1)
			{
				std::string lastErr;
				{
					std::lock_guard<std::mutex> lock(m_ErrorMutex);
					lastErr = m_LastError;
				}
				if (lastErr.find("Failed to read encryption negotiation") != std::string::npos)
				{
					LOG_INFO("[Baichuan] %s:%d plain TCP rejected, switching to TLS", m_Host.c_str(), m_Port);
					m_UseTls = true;
				}
			}
		}

		CleanupTls();

		if (m_Socket != (uintptr_t)INVALID_SOCKET)
		{
			closesocket((SOCKET)m_Socket);
			m_Socket = (uintptr_t)INVALID_SOCKET;
		}

		if (!m_Running.load()) break;

		std::string lastErr;
		{
			std::lock_guard<std::mutex> lock(m_ErrorMutex);
			lastErr = m_LastError;
		}

		// Only log first few failures and then periodically
		if (consecutiveFailures <= 3 || consecutiveFailures % 10 == 0)
		{
			LOG_WARNING("[Baichuan] Camera %s:%d disconnected (%s), retrying in %dms (attempt %d)",
				m_Host.c_str(), m_Port, lastErr.empty() ? "unknown" : lastErr.c_str(), backoff, consecutiveFailures);
		}

		// Don't wait on TLS upgrade (first retry is immediate)
		if (m_UseTls && consecutiveFailures == 1)
			continue;

		std::this_thread::sleep_for(std::chrono::milliseconds(backoff));
		backoff = std::min(backoff * 2, 60000);
	}
}

bool ReolinkBaichuanClient::Connect()
{
	struct addrinfo hints{}, *result = nullptr;
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;

	std::string portStr = std::to_string(m_Port);
	int ret = getaddrinfo(m_Host.c_str(), portStr.c_str(), &hints, &result);
	if (ret != 0 || !result)
	{
		std::lock_guard<std::mutex> lock(m_ErrorMutex);
		m_LastError = "DNS resolution failed for " + m_Host;
		return false;
	}

	SOCKET sock = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
	if (sock == INVALID_SOCKET)
	{
		freeaddrinfo(result);
		std::lock_guard<std::mutex> lock(m_ErrorMutex);
		m_LastError = "Failed to create socket";
		return false;
	}

	// Set connection timeout (5 seconds)
#ifdef _WIN32
	DWORD timeout = 5000;
	setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
	setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof(timeout));
#else
	struct timeval tv{ 5, 0 };
	setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif

	ret = connect(sock, result->ai_addr, (int)result->ai_addrlen);
	freeaddrinfo(result);

	if (ret != 0)
	{
		closesocket(sock);
		std::lock_guard<std::mutex> lock(m_ErrorMutex);
		m_LastError = "Connection refused to " + m_Host + ":" + std::to_string(m_Port);
		return false;
	}

	// Longer read timeout for streaming
#ifdef _WIN32
	timeout = 30000;
	setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
#else
	tv.tv_sec = 30;
	setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

	m_Socket = (uintptr_t)sock;

	// Establish TLS if required
	if (m_UseTls)
	{
		if (!TryTlsConnect())
		{
			closesocket(sock);
			m_Socket = (uintptr_t)INVALID_SOCKET;
			return false;
		}
	}

	return true;
}

bool ReolinkBaichuanClient::TryTlsConnect()
{
	SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
	if (!ctx)
	{
		std::lock_guard<std::mutex> lock(m_ErrorMutex);
		m_LastError = "Failed to create SSL context";
		return false;
	}

	// Accept self-signed certs (cameras use self-signed)
	SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);

	SSL* ssl = SSL_new(ctx);
	if (!ssl)
	{
		SSL_CTX_free(ctx);
		std::lock_guard<std::mutex> lock(m_ErrorMutex);
		m_LastError = "Failed to create SSL object";
		return false;
	}

	SSL_set_fd(ssl, (int)(uintptr_t)m_Socket);

	int sslRet = SSL_connect(ssl);
	if (sslRet != 1)
	{
		int err = SSL_get_error(ssl, sslRet);
		SSL_free(ssl);
		SSL_CTX_free(ctx);
		std::lock_guard<std::mutex> lock(m_ErrorMutex);
		m_LastError = "TLS handshake failed (SSL error " + std::to_string(err) + ")";
		return false;
	}

	m_SslCtx = ctx;
	m_Ssl = ssl;

	LOG_INFO("[Baichuan] TLS handshake successful with %s:%d", m_Host.c_str(), m_Port);
	return true;
}

void ReolinkBaichuanClient::CleanupTls()
{
	if (m_Ssl)
	{
		SSL_shutdown((SSL*)m_Ssl);
		SSL_free((SSL*)m_Ssl);
		m_Ssl = nullptr;
	}
	if (m_SslCtx)
	{
		SSL_CTX_free((SSL_CTX*)m_SslCtx);
		m_SslCtx = nullptr;
	}
}

bool ReolinkBaichuanClient::Authenticate()
{
	// Phase 1: Encryption negotiation (Legacy header, empty body)
	BcHeader20 negHeader{};
	negHeader.Magic = BC_MAGIC;
	negHeader.CmdId = 1;
	negHeader.BodyLen = 0;
	negHeader.ChannelId = 250; // host channel per reolink_aio
	negHeader.StreamType = (uint8_t)(++m_MessageCounter & 0xFF); // Low byte of 3-byte mess_id
	negHeader.MsgNum = (uint16_t)((m_MessageCounter >> 8) & 0xFFFF); // High 2 bytes of mess_id
	negHeader.ResponseCode = ENC_REQUEST_BC; // BC-XOR encryption
	negHeader.MessageClass = BC_CLASS_LEGACY;

	if (!SendRaw(reinterpret_cast<uint8_t*>(&negHeader), sizeof(negHeader)))
	{
		std::lock_guard<std::mutex> lock(m_ErrorMutex);
		m_LastError = "Failed to send encryption negotiation";
		return false;
	}

	// Read negotiation response
	BcHeader20 negResp{};
	if (!ReadExact(reinterpret_cast<uint8_t*>(&negResp), sizeof(negResp)))
	{
		std::lock_guard<std::mutex> lock(m_ErrorMutex);
		m_LastError = "Failed to read encryption negotiation response";
		return false;
	}

	if (negResp.Magic != BC_MAGIC)
	{
		std::lock_guard<std::mutex> lock(m_ErrorMutex);
		m_LastError = "Invalid magic in negotiation response: 0x" +
			([&]{ std::ostringstream s; s << std::hex << negResp.Magic; return s.str(); })();
		return false;
	}

	// If modern header, read the extra 4 bytes (PayloadOffset)
	uint32_t payloadOffset = 0;
	if (HeaderHasPayloadOffset(negResp.MessageClass))
	{
		if (!ReadExact(reinterpret_cast<uint8_t*>(&payloadOffset), 4))
		{
			std::lock_guard<std::mutex> lock(m_ErrorMutex);
			m_LastError = "Failed to read negotiation header extension";
			return false;
		}
	}

	// Read nonce from response body (BC-XOR encrypted XML)
	std::string nonce;
	if (negResp.BodyLen > 0 && negResp.BodyLen < 4096)
	{
		std::vector<uint8_t> encBody(negResp.BodyLen);
		if (!ReadExact(encBody.data(), negResp.BodyLen))
		{
			std::lock_guard<std::mutex> lock(m_ErrorMutex);
			m_LastError = "Failed to read negotiation body";
			return false;
		}

		// Decrypt with BC-XOR
		std::vector<uint8_t> decBody(negResp.BodyLen);
		BcXorDecrypt(encBody.data(), decBody.data(), negResp.BodyLen, negResp.ChannelId);

		std::string xml(decBody.begin(), decBody.end());

		// Extract nonce from XML: <nonce>...</nonce>
		auto nonceStart = xml.find("<nonce>");
		auto nonceEnd = xml.find("</nonce>");
		if (nonceStart != std::string::npos && nonceEnd != std::string::npos)
		{
			nonceStart += 7; // strlen("<nonce>")
			nonce = xml.substr(nonceStart, nonceEnd - nonceStart);
		}
	}

	if (nonce.empty())
	{
		LOG_WARNING("[Baichuan] %s:%d no nonce received, using direct credentials", m_Host.c_str(), m_Port);
	}
	else
	{
		// Derive AES key for push event decryption: MD5(nonce + "-" + password)[0:16] as ASCII bytes
		std::string aesKeyStr = Md5Modern(nonce + "-" + m_Password).substr(0, 16);
		m_AesKey.assign(aesKeyStr.begin(), aesKeyStr.end());
	}

	// Phase 2: Login with MD5-hashed credentials (Modern header)
	std::string userHash = nonce.empty() ? m_Username : Md5Modern(m_Username + nonce);
	std::string passHash = nonce.empty() ? m_Password : Md5Modern(m_Password + nonce);

	std::string loginXml = "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\n<body>\n"
		"<LoginUser version=\"1.1\">\n"
		"<userName>" + userHash + "</userName>\n"
		"<password>" + passHash + "</password>\n"
		"<userVer>1</userVer>\n"
		"</LoginUser>\n"
		"<LoginNet version=\"1.1\">\n"
		"<type>LAN</type>\n"
		"<udpPort>0</udpPort>\n"
		"</LoginNet>\n"
		"</body>";

	std::vector<uint8_t> loginPayload(loginXml.begin(), loginXml.end());

	// BC-XOR encrypt the login body (offset = ChannelId used in header)
	static constexpr uint8_t LOGIN_CHANNEL_ID = 250; // "host" channel per reolink_aio
	std::vector<uint8_t> encPayload(loginPayload.size());
	BcXorEncrypt(loginPayload.data(), encPayload.data(), loginPayload.size(), LOGIN_CHANNEL_ID);

	// Send with modern 24-byte header (reolink_aio login uses default message_class="1464", status="0000")
	BcHeader24 loginHeader{};
	loginHeader.Magic = BC_MAGIC;
	loginHeader.CmdId = 1;
	loginHeader.BodyLen = (uint32_t)encPayload.size();
	loginHeader.ChannelId = LOGIN_CHANNEL_ID;
	uint32_t msgId = ++m_MessageCounter;
	loginHeader.StreamType = (uint8_t)(msgId & 0xFF);
	loginHeader.MsgNum = (uint16_t)((msgId >> 8) & 0xFFFF);
	loginHeader.ResponseCode = 0x0000;
	loginHeader.MessageClass = BC_CLASS_MODERN;
	loginHeader.PayloadOffset = 0;

	if (!SendRaw(reinterpret_cast<uint8_t*>(&loginHeader), sizeof(loginHeader)))
	{
		std::lock_guard<std::mutex> lock(m_ErrorMutex);
		m_LastError = "Failed to send login header";
		return false;
	}
	if (!SendRaw(encPayload.data(), encPayload.size()))
	{
		std::lock_guard<std::mutex> lock(m_ErrorMutex);
		m_LastError = "Failed to send login body";
		return false;
	}

	// Read login response — first 20 bytes, then check if modern
	BcHeader20 loginRespBase{};
	if (!ReadExact(reinterpret_cast<uint8_t*>(&loginRespBase), sizeof(loginRespBase)))
	{
		std::lock_guard<std::mutex> lock(m_ErrorMutex);
		m_LastError = "Failed to read login response";
		return false;
	}

	if (loginRespBase.Magic != BC_MAGIC)
	{
		std::lock_guard<std::mutex> lock(m_ErrorMutex);
		m_LastError = "Invalid magic in login response: 0x" +
			([&]{ std::ostringstream s; s << std::hex << loginRespBase.Magic; return s.str(); })();
		return false;
	}

	// If modern header, consume extra 4 bytes
	if (HeaderHasPayloadOffset(loginRespBase.MessageClass))
	{
		uint32_t off = 0;
		if (!ReadExact(reinterpret_cast<uint8_t*>(&off), 4))
		{
			std::lock_guard<std::mutex> lock(m_ErrorMutex);
			m_LastError = "Failed to read login response header extension";
			return false;
		}
	}

	LOG_INFO("[Baichuan] %s:%d login response: respCode=%u",
		m_Host.c_str(), m_Port, loginRespBase.ResponseCode);

	// Skip response body
	if (loginRespBase.BodyLen > 0 && loginRespBase.BodyLen < 65536)
	{
		std::vector<uint8_t> respBody(loginRespBase.BodyLen);
		if (!ReadExact(respBody.data(), loginRespBase.BodyLen))
		{
			std::lock_guard<std::mutex> lock(m_ErrorMutex);
			m_LastError = "Failed to read login response body";
			return false;
		}
	}

	if (loginRespBase.ResponseCode != 200 && loginRespBase.ResponseCode != 201 && loginRespBase.ResponseCode != 300)
	{
		std::lock_guard<std::mutex> lock(m_ErrorMutex);
		m_LastError = "Login failed, code=" + std::to_string(loginRespBase.ResponseCode);
		LOG_ERROR("[Baichuan] Login failed for %s:%d (code %u)", m_Host.c_str(), m_Port, loginRespBase.ResponseCode);
		return false;
	}

	LOG_INFO("[Baichuan] Authenticated to %s:%d", m_Host.c_str(), m_Port);
	return true;
}

bool ReolinkBaichuanClient::RequestStream()
{
	// Subscribe to push events (cmd_id=31) — this gives us motion/AI detection events
	// without needing to parse BcMedia video frames.
	// reolink_aio sends: cmd_id=31, ch_id=251 (push channel), no body
	static constexpr uint8_t PUSH_CHANNEL_ID = 251;

	BcHeader24 subHeader{};
	subHeader.Magic = BC_MAGIC;
	subHeader.CmdId = 31;
	subHeader.BodyLen = 0;
	subHeader.ChannelId = PUSH_CHANNEL_ID;
	uint32_t subMsgId = ++m_MessageCounter;
	subHeader.StreamType = (uint8_t)(subMsgId & 0xFF);
	subHeader.MsgNum = (uint16_t)((subMsgId >> 8) & 0xFFFF);
	subHeader.ResponseCode = 0x0000;
	subHeader.MessageClass = BC_CLASS_MODERN;
	subHeader.PayloadOffset = 0;

	LOG_INFO("[Baichuan] %s:%d subscribing to push events (cmd_id=31)", m_Host.c_str(), m_Port);

	if (!SendRaw(reinterpret_cast<uint8_t*>(&subHeader), sizeof(subHeader)))
	{
		LOG_ERROR("[Baichuan] %s:%d failed to send event subscription", m_Host.c_str(), m_Port);
		return false;
	}

	return true;
}

void ReolinkBaichuanClient::ReadLoop()
{
	while (m_Running.load())
	{
		// Read minimum header (20 bytes)
		BcHeader20 header{};
		if (!ReadExact(reinterpret_cast<uint8_t*>(&header), sizeof(header)))
			break;

		if (header.Magic != BC_MAGIC)
		{
			LOG_WARNING("[Baichuan] Unexpected magic 0x%08X, reconnecting", header.Magic);
			break;
		}

		// Determine if 24-byte header
		uint32_t payloadOffset = 0;
		if (HeaderHasPayloadOffset(header.MessageClass))
		{
			if (!ReadExact(reinterpret_cast<uint8_t*>(&payloadOffset), 4))
				break;
		}

		// Read body
		if (header.BodyLen == 0) continue;
		if (header.BodyLen > 1 * 1024 * 1024) // 1MB max for event XML
		{
			LOG_WARNING("[Baichuan] Body too large (%u), reconnecting", header.BodyLen);
			break;
		}

		std::vector<uint8_t> body(header.BodyLen);
		if (!ReadExact(body.data(), header.BodyLen))
			break;

		// Decrypt body based on header type and encryption indicator
		std::string xml;
		bool isModernHeader = HeaderHasPayloadOffset(header.MessageClass);

		if (isModernHeader)
		{
			// Modern 24-byte header: body is AES encrypted
			if (!AesDecrypt(body.data(), header.BodyLen, xml))
			{
				// Fallback: try BC-XOR
				std::vector<uint8_t> decBody(header.BodyLen);
				BcXorDecrypt(body.data(), decBody.data(), header.BodyLen, header.ChannelId);
				xml.assign(decBody.begin(), decBody.end());
			}
		}
		else
		{
			// Legacy 20-byte header: check ResponseCode for encryption type
			if (header.ResponseCode == ENC_RESPONSE_NONE)
			{
				xml.assign(body.begin(), body.end());
			}
			else if (header.ResponseCode == ENC_RESPONSE_AES || header.ResponseCode == ENC_RESPONSE_AES2)
			{
				if (!AesDecrypt(body.data(), header.BodyLen, xml))
				{
					std::vector<uint8_t> decBody(header.BodyLen);
					BcXorDecrypt(body.data(), decBody.data(), header.BodyLen, header.ChannelId);
					xml.assign(decBody.begin(), decBody.end());
				}
			}
			else
			{
				// BC-XOR (0xDD01, 0xDD12, or other)
				std::vector<uint8_t> decBody(header.BodyLen);
				BcXorDecrypt(body.data(), decBody.data(), header.BodyLen, header.ChannelId);
				xml.assign(decBody.begin(), decBody.end());
			}
		}

		// Validate decryption — if not XML, try alternate method
		if (!xml.empty() && xml.substr(0, 5) != "<?xml")
		{
			// AES didn't produce XML, try BC-XOR
			std::vector<uint8_t> decBody(header.BodyLen);
			BcXorDecrypt(body.data(), decBody.data(), header.BodyLen, header.ChannelId);
			std::string altXml(decBody.begin(), decBody.end());
			if (altXml.substr(0, 5) == "<?xml")
				xml = std::move(altXml);
		}

		// Handle push events
		if (header.CmdId == 33)
		{
			// Motion/AI detection event — parse AlarmEvent XML
			ParseAlarmEvent(xml);
		}
		else if (header.CmdId == 78)
		{
			// Channel status push — ignore for now
		}
		else if (header.CmdId == 31)
		{
			// Subscribe response — log success
			LOG_INFO("[Baichuan] %s:%d event subscription confirmed (resp=%u)",
				m_Host.c_str(), m_Port, header.ResponseCode);
		}
		else if (header.CmdId == 79 || header.CmdId == 291)
		{
			// Channel status (79) and floodlight status (291) — ignore silently
		}
		else
		{
			// Other push events — log for debugging
			LOG_INFO("[Baichuan] %s:%d received cmd_id=%u body=%u bytes",
				m_Host.c_str(), m_Port, header.CmdId, header.BodyLen);
		}
	}
}

void ReolinkBaichuanClient::ParseAlarmEvent(const std::string& xml)
{
	// Parse cmd_id=33 AlarmEvent XML for motion and AI detection states
	// Format:
	// <body><AlarmEventList><AlarmEvent>
	//   <channelId>0</channelId>
	//   <status>MD</status>  (or "none")
	//   <AItype>people,vehicle</AItype>  (comma-separated, or "none")
	//   <smartAiTypeList>...</smartAiTypeList>  (optional, with bounding info)
	// </AlarmEvent></AlarmEventList></body>

	std::vector<ReolinkDetection> detections;

	// Simple XML parsing for status and AItype
	auto findTag = [&](const std::string& tag) -> std::string
	{
		std::string openTag = "<" + tag + ">";
		std::string closeTag = "</" + tag + ">";
		auto start = xml.find(openTag);
		if (start == std::string::npos) return "";
		start += openTag.size();
		auto end = xml.find(closeTag, start);
		if (end == std::string::npos) return "";
		return xml.substr(start, end - start);
	};

	std::string status = findTag("status");
	std::string aiTypes = findTag("AItype");

	bool hasMotion = (status.find("MD") != std::string::npos);
	bool hasPeople = (aiTypes.find("people") != std::string::npos);
	bool hasVehicle = (aiTypes.find("vehicle") != std::string::npos);
	bool hasAnimal = (aiTypes.find("dog_cat") != std::string::npos);

	// Create detection entries for each detected AI type
	if (hasPeople)
	{
		ReolinkDetection det;
		det.DetectionClass = ReolinkDetection::People;
		det.Confidence = 1.0f;
		det.X1 = 0; det.Y1 = 0; det.X2 = 1; det.Y2 = 1; // No bbox from events
		detections.push_back(det);
	}
	if (hasVehicle)
	{
		ReolinkDetection det;
		det.DetectionClass = ReolinkDetection::Vehicle;
		det.Confidence = 1.0f;
		det.X1 = 0; det.Y1 = 0; det.X2 = 1; det.Y2 = 1;
		detections.push_back(det);
	}
	if (hasAnimal)
	{
		ReolinkDetection det;
		det.DetectionClass = ReolinkDetection::Animal;
		det.Confidence = 1.0f;
		det.X1 = 0; det.Y1 = 0; det.X2 = 1; det.Y2 = 1;
		detections.push_back(det);
	}

	// Update detection state
	{
		std::lock_guard<std::mutex> lock(m_DetectionMutex);
		m_CurrentDetections.Detections = std::move(detections);
		m_CurrentDetections.Timestamp = std::chrono::steady_clock::now();
		m_CurrentDetections.HasData = true;
		m_CurrentDetections.HasMotion = hasMotion;
	}
}

// ============================================================================
// PTZ commands
// ============================================================================

bool ReolinkBaichuanClient::PtzControl(const std::string& command, int speed, int channel)
{
	if (!m_Connected.load())
		return false;

	std::string xml = "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\n<body>\n"
		"<PtzControl version=\"1.1\">\n"
		"<channelId>" + std::to_string(channel) + "</channelId>\n"
		"<command>" + command + "</command>\n"
		"<speed>" + std::to_string(speed) + "</speed>\n"
		"</PtzControl>\n"
		"</body>";

	std::lock_guard<std::mutex> lock(m_SendMutex);
	uint32_t msgId = ++m_MessageCounter;

	// AES-encrypt the body for modern header commands
	std::vector<uint8_t> body(xml.begin(), xml.end());
	std::vector<uint8_t> encBody;
	if (!m_AesKey.empty())
	{
		// AES-128-CFB encrypt
		EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
		if (ctx)
		{
			encBody.resize(body.size() + 16);
			int outLen = 0;
			EVP_EncryptInit_ex(ctx, EVP_aes_128_cfb128(), nullptr, m_AesKey.data(), AES_CFB_IV);
			EVP_CIPHER_CTX_set_padding(ctx, 0);
			EVP_EncryptUpdate(ctx, encBody.data(), &outLen, body.data(), (int)body.size());
			int finalLen = 0;
			EVP_EncryptFinal_ex(ctx, encBody.data() + outLen, &finalLen);
			EVP_CIPHER_CTX_free(ctx);
			encBody.resize(outLen + finalLen);
		}
		else
		{
			encBody = body; // fallback to unencrypted
		}
	}
	else
	{
		encBody = body;
	}

	// Modern 24-byte header, cmd_id=18 (PtzControl), ch_id = channel+1
	BcHeader24 header{};
	header.Magic = BC_MAGIC;
	header.CmdId = 18;
	header.BodyLen = (uint32_t)encBody.size();
	header.ChannelId = (uint8_t)(channel + 1);
	header.StreamType = (uint8_t)(msgId & 0xFF);
	header.MsgNum = (uint16_t)((msgId >> 8) & 0xFFFF);
	header.ResponseCode = 0x0000;
	header.MessageClass = BC_CLASS_MODERN;
	header.PayloadOffset = 0;

	if (!SendRaw(reinterpret_cast<uint8_t*>(&header), sizeof(header)))
		return false;
	if (!SendRaw(encBody.data(), encBody.size()))
		return false;

	return true;
}

bool ReolinkBaichuanClient::PtzStop(int channel)
{
	return PtzControl("Stop", 0, channel);
}

bool ReolinkBaichuanClient::PtzGoToPreset(int presetId, int channel)
{
	if (!m_Connected.load())
		return false;

	std::string xml = "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\n<body>\n"
		"<PtzPreset version=\"1.1\">\n"
		"<channelId>" + std::to_string(channel) + "</channelId>\n"
		"<presetList>\n<preset>\n"
		"<id>" + std::to_string(presetId) + "</id>\n"
		"<command>toPos</command>\n"
		"</preset>\n</presetList>\n"
		"</PtzPreset>\n"
		"</body>";

	std::lock_guard<std::mutex> lock(m_SendMutex);
	uint32_t msgId = ++m_MessageCounter;

	std::vector<uint8_t> body(xml.begin(), xml.end());
	std::vector<uint8_t> encBody;
	if (!m_AesKey.empty())
	{
		EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
		if (ctx)
		{
			encBody.resize(body.size() + 16);
			int outLen = 0;
			EVP_EncryptInit_ex(ctx, EVP_aes_128_cfb128(), nullptr, m_AesKey.data(), AES_CFB_IV);
			EVP_CIPHER_CTX_set_padding(ctx, 0);
			EVP_EncryptUpdate(ctx, encBody.data(), &outLen, body.data(), (int)body.size());
			int finalLen = 0;
			EVP_EncryptFinal_ex(ctx, encBody.data() + outLen, &finalLen);
			EVP_CIPHER_CTX_free(ctx);
			encBody.resize(outLen + finalLen);
		}
		else
		{
			encBody = body;
		}
	}
	else
	{
		encBody = body;
	}

	// cmd_id=19 (PtzPreset), ch_id = channel+1
	BcHeader24 header{};
	header.Magic = BC_MAGIC;
	header.CmdId = 19;
	header.BodyLen = (uint32_t)encBody.size();
	header.ChannelId = (uint8_t)(channel + 1);
	header.StreamType = (uint8_t)(msgId & 0xFF);
	header.MsgNum = (uint16_t)((msgId >> 8) & 0xFFFF);
	header.ResponseCode = 0x0000;
	header.MessageClass = BC_CLASS_MODERN;
	header.PayloadOffset = 0;

	if (!SendRaw(reinterpret_cast<uint8_t*>(&header), sizeof(header)))
		return false;
	if (!SendRaw(encBody.data(), encBody.size()))
		return false;

	return true;
}

// ============================================================================
// Low-level I/O
// ============================================================================

bool ReolinkBaichuanClient::ReadExact(uint8_t* buffer, size_t length)
{
	size_t totalRead = 0;
	while (totalRead < length && m_Running.load())
	{
		int bytesRead;
		if (m_Ssl)
		{
			bytesRead = SSL_read((SSL*)m_Ssl, reinterpret_cast<char*>(buffer + totalRead),
				(int)(length - totalRead));
		}
		else
		{
			bytesRead = recv((SOCKET)m_Socket, reinterpret_cast<char*>(buffer + totalRead),
				(int)(length - totalRead), 0);
		}

		if (bytesRead <= 0)
		{
			if (bytesRead == 0)
			{
				LOG_WARNING("[Baichuan] Connection closed by peer (read %zu/%zu bytes)", totalRead, length);
			}
			else
			{
				if (m_Ssl)
				{
					int sslErr = SSL_get_error((SSL*)m_Ssl, bytesRead);
					LOG_WARNING("[Baichuan] SSL read error %d (read %zu/%zu bytes)", sslErr, totalRead, length);
				}
				else
				{
#ifdef _WIN32
					int err = WSAGetLastError();
					LOG_WARNING("[Baichuan] recv error %d (read %zu/%zu bytes)", err, totalRead, length);
#else
					LOG_WARNING("[Baichuan] recv error %d (read %zu/%zu bytes)", errno, totalRead, length);
#endif
				}
			}
			return false;
		}
		totalRead += bytesRead;
	}
	return totalRead == length;
}

bool ReolinkBaichuanClient::SendRaw(const uint8_t* data, size_t length)
{
	size_t totalSent = 0;
	while (totalSent < length)
	{
		int sent;
		if (m_Ssl)
		{
			sent = SSL_write((SSL*)m_Ssl, reinterpret_cast<const char*>(data + totalSent),
				(int)(length - totalSent));
		}
		else
		{
			sent = send((SOCKET)m_Socket, reinterpret_cast<const char*>(data + totalSent),
				(int)(length - totalSent), 0);
		}

		if (sent <= 0)
			return false;
		totalSent += sent;
	}
	return true;
}

bool ReolinkBaichuanClient::SendBcMessage(uint32_t cmdId, uint32_t msgId, const std::vector<uint8_t>& payload)
{
	// Convenience wrapper — uses modern header
	BcHeader24 header{};
	header.Magic = BC_MAGIC;
	header.CmdId = cmdId;
	header.BodyLen = (uint32_t)payload.size();
	header.ChannelId = 0;
	header.StreamType = 0;
	header.MsgNum = (uint16_t)msgId;
	header.ResponseCode = 0;
	header.MessageClass = BC_CLASS_MODERN;
	header.PayloadOffset = 0;

	if (!SendRaw(reinterpret_cast<uint8_t*>(&header), sizeof(header)))
		return false;
	if (!payload.empty())
	{
		if (!SendRaw(payload.data(), payload.size()))
			return false;
	}
	return true;
}

bool ReolinkBaichuanClient::AesDecrypt(const uint8_t* in, size_t len, std::string& out)
{
	if (m_AesKey.empty() || m_AesKey.size() != 16)
		return false;

	// AES-128-CFB decryption using OpenSSL EVP
	EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
	if (!ctx) return false;

	if (EVP_DecryptInit_ex(ctx, EVP_aes_128_cfb128(), nullptr, m_AesKey.data(), AES_CFB_IV) != 1)
	{
		EVP_CIPHER_CTX_free(ctx);
		return false;
	}

	// CFB doesn't need padding
	EVP_CIPHER_CTX_set_padding(ctx, 0);

	std::vector<uint8_t> plaintext(len + 16);
	int outLen = 0;
	if (EVP_DecryptUpdate(ctx, plaintext.data(), &outLen, in, (int)len) != 1)
	{
		EVP_CIPHER_CTX_free(ctx);
		return false;
	}

	int finalLen = 0;
	EVP_DecryptFinal_ex(ctx, plaintext.data() + outLen, &finalLen);
	EVP_CIPHER_CTX_free(ctx);

	out.assign(reinterpret_cast<char*>(plaintext.data()), outLen + finalLen);
	return true;
}

}}
