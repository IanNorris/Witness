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
static constexpr uint16_t ENC_REQUEST_NONE = 0xDC00;
static constexpr uint16_t ENC_RESPONSE_NONE = 0xDD00;
static constexpr uint16_t ENC_RESPONSE_XOR = 0xDD01;

// BC-XOR key for XML encryption
static constexpr uint8_t BC_XML_KEY[8] = { 0x1F, 0x2D, 0x3C, 0x4B, 0x5A, 0x69, 0x78, 0xFF };

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
	return Md5HexUpper(input).substr(0, 32);
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
	negHeader.ChannelId = 0;
	negHeader.StreamType = 0;
	negHeader.MsgNum = 0; // First message is always 0
	negHeader.ResponseCode = ENC_REQUEST_NONE; // Request no encryption
	negHeader.MessageClass = BC_CLASS_LEGACY;

	LOG_INFO("[Baichuan] %s:%d sending negotiation: magic=0x%08X cmd=%u class=0x%04X resp=0x%04X",
		m_Host.c_str(), m_Port, negHeader.Magic, negHeader.CmdId, negHeader.MessageClass, negHeader.ResponseCode);

	if (!SendRaw(reinterpret_cast<uint8_t*>(&negHeader), sizeof(negHeader)))
	{
		std::lock_guard<std::mutex> lock(m_ErrorMutex);
		m_LastError = "Failed to send encryption negotiation";
		return false;
	}

	// Read negotiation response — could be 20 or 24 byte header
	// Read the first 20 bytes to determine header type
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

	LOG_INFO("[Baichuan] %s:%d negotiation response: cmd=%u, class=0x%04X, bodyLen=%u, respCode=%u",
		m_Host.c_str(), m_Port, negResp.CmdId, negResp.MessageClass, negResp.BodyLen, negResp.ResponseCode);

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
		LOG_INFO("[Baichuan] %s:%d negotiation XML: %s", m_Host.c_str(), m_Port, xml.c_str());

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

	// BC-XOR encrypt the login body
	std::vector<uint8_t> encPayload(loginPayload.size());
	BcXorEncrypt(loginPayload.data(), encPayload.data(), loginPayload.size(), 0);

	// Send with modern 24-byte header
	BcHeader24 loginHeader{};
	loginHeader.Magic = BC_MAGIC;
	loginHeader.CmdId = 1;
	loginHeader.BodyLen = (uint32_t)encPayload.size();
	loginHeader.ChannelId = 0;
	loginHeader.StreamType = 0;
	loginHeader.MsgNum = ++m_MessageCounter;
	loginHeader.ResponseCode = 0;
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

	LOG_INFO("[Baichuan] %s:%d login response: cmd=%u, class=0x%04X, bodyLen=%u, respCode=%u",
		m_Host.c_str(), m_Port, loginRespBase.CmdId, loginRespBase.MessageClass,
		loginRespBase.BodyLen, loginRespBase.ResponseCode);

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

		// Decrypt and log for diagnostics
		std::vector<uint8_t> decResp(loginRespBase.BodyLen);
		BcXorDecrypt(respBody.data(), decResp.data(), loginRespBase.BodyLen, loginRespBase.ChannelId);
		std::string respXml(decResp.begin(), decResp.end());
		LOG_INFO("[Baichuan] %s:%d login response body: %s", m_Host.c_str(), m_Port, respXml.c_str());
	}

	if (loginRespBase.ResponseCode != 200)
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
	// Request video stream (cmd_id=3) with extension + payload XML
	std::string extXml = "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\n"
		"<Extension version=\"1.1\">\n"
		"<channelId>0</channelId>\n"
		"</Extension>";
	std::string payXml = "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\n"
		"<body>\n"
		"<Preview version=\"1.0\">\n"
		"<handle>0</handle>\n"
		"<streamType>mainStream</streamType>\n"
		"</Preview>\n"
		"</body>";

	std::vector<uint8_t> extData(extXml.begin(), extXml.end());
	std::vector<uint8_t> payData(payXml.begin(), payXml.end());

	// BC-XOR encrypt both parts
	std::vector<uint8_t> encExt(extData.size());
	std::vector<uint8_t> encPay(payData.size());
	BcXorEncrypt(extData.data(), encExt.data(), extData.size(), 0);
	BcXorEncrypt(payData.data(), encPay.data(), payData.size(), 0);

	// Build message: header + extension + payload
	uint32_t totalBody = (uint32_t)(encExt.size() + encPay.size());

	BcHeader24 streamHeader{};
	streamHeader.Magic = BC_MAGIC;
	streamHeader.CmdId = 3;
	streamHeader.BodyLen = totalBody;
	streamHeader.ChannelId = 0;
	streamHeader.StreamType = 0;
	streamHeader.MsgNum = ++m_MessageCounter;
	streamHeader.ResponseCode = 0;
	streamHeader.MessageClass = BC_CLASS_MODERN;
	streamHeader.PayloadOffset = (uint32_t)encExt.size();

	if (!SendRaw(reinterpret_cast<uint8_t*>(&streamHeader), sizeof(streamHeader)))
		return false;
	if (!SendRaw(encExt.data(), encExt.size()))
		return false;
	if (!SendRaw(encPay.data(), encPay.size()))
		return false;

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
		if (header.BodyLen > 20 * 1024 * 1024)
		{
			LOG_WARNING("[Baichuan] Body too large (%u), reconnecting", header.BodyLen);
			break;
		}

		std::vector<uint8_t> body(header.BodyLen);
		if (!ReadExact(body.data(), header.BodyLen))
			break;

		// If this is a video stream message (cmd_id=3), parse BcMedia frames
		if (header.CmdId == 3)
		{
			ParseBcMediaFrames(body.data(), body.size());
		}
	}
}

void ReolinkBaichuanClient::ParseBcMediaFrames(const uint8_t* data, size_t length)
{
	size_t offset = 0;

	while (offset + 24 <= length)
	{
		uint32_t magic = *(const uint32_t*)(data + offset);

		// Check if this is an I-frame or P-frame
		bool isIFrame = (magic >= 0x63643030 && magic <= 0x63643039);
		bool isPFrame = (magic >= 0x63643130 && magic <= 0x63643139);

		if (!isIFrame && !isPFrame)
		{
			// Could be InfoV1/V2, audio, or other — skip
			// InfoV1/V2 are 32 bytes with known magic
			uint32_t infoMagic = magic;
			if (infoMagic == 0x31303031 || infoMagic == 0x32303031)
			{
				// Info packet — 32 bytes
				offset += 32;
				continue;
			}
			// Audio (AAC or ADPCM)
			if (magic == 0x62773530 || magic == 0x62773130)
			{
				if (offset + 8 > length) break;
				uint16_t audioLen = *(const uint16_t*)(data + offset + 4);
				size_t padLen = (audioLen % 8 == 0) ? 0 : (8 - audioLen % 8);
				offset += 8 + audioLen + padLen;
				continue;
			}
			// Unknown — try advancing
			offset += 4;
			continue;
		}

		// Parse video frame header (24 bytes)
		// Layout: magic(4) + encoding(4) + payloadSize(4) + additionalHeaderSize(4) + micros(4) + unknown(4)
		if (offset + 24 > length) break;

		uint32_t payloadSize = *(const uint32_t*)(data + offset + 8);
		uint32_t additionalHeaderSize = *(const uint32_t*)(data + offset + 12);
		// uint32_t microseconds = *(const uint32_t*)(data + offset + 16);

		size_t frameHeaderEnd = offset + 24;

		// Additional header comes FIRST (after the 24-byte frame header)
		if (additionalHeaderSize > 0 && frameHeaderEnd + additionalHeaderSize <= length)
		{
			ParseDetectionData(data + frameHeaderEnd, additionalHeaderSize, isIFrame);
		}

		// Skip past additional header + payload + padding
		size_t payloadStart = frameHeaderEnd + additionalHeaderSize;
		size_t padSize = (payloadSize % 8 == 0) ? 0 : (8 - payloadSize % 8);
		offset = payloadStart + payloadSize + padSize;

		if (offset > length)
			break;
	}
}

void ReolinkBaichuanClient::ParseDetectionData(const uint8_t* data, size_t length, bool isIFrame)
{
	// Check for AI frame size TLV anywhere in the data
	for (size_t i = 0; i + 7 <= length; i++)
	{
		if (data[i] == FRAME_SIZE_TLV[0] && data[i + 1] == FRAME_SIZE_TLV[1] && data[i + 2] == FRAME_SIZE_TLV[2])
		{
			uint16_t w = *(const uint16_t*)(data + i + 3);
			uint16_t h = *(const uint16_t*)(data + i + 5);
			if (w >= 64 && w <= 8192 && h >= 64 && h <= 8192)
			{
				m_AiWidth = w;
				m_AiHeight = h;
			}
		}
	}

	// For I-frames, the standard marker is at offset 8 (skip 8-byte prefix)
	// For P-frames, it's at offset 0
	size_t markerOffset = isIFrame ? 8 : 0;

	if (length <= markerOffset + 8)
	{
		// Too short for any detection data
		std::lock_guard<std::mutex> lock(m_DetectionMutex);
		m_CurrentDetections.Detections.clear();
		m_CurrentDetections.Timestamp = std::chrono::steady_clock::now();
		m_CurrentDetections.HasData = true;
		return;
	}

	// Check for standard marker: FF xx 00 01 0B 00 01 08
	bool hasMarker = (data[markerOffset] == 0xFF &&
		data[markerOffset + 2] == 0x00 &&
		data[markerOffset + 3] == 0x01 &&
		data[markerOffset + 4] == 0x0B &&
		data[markerOffset + 5] == 0x00 &&
		data[markerOffset + 6] == 0x01 &&
		data[markerOffset + 7] == 0x08);

	if (!hasMarker)
	{
		// No standard marker — no detection data
		std::lock_guard<std::mutex> lock(m_DetectionMutex);
		m_CurrentDetections.Detections.clear();
		m_CurrentDetections.Timestamp = std::chrono::steady_clock::now();
		m_CurrentDetections.HasData = true;
		return;
	}

	// Baseline header (128 bytes for P-frame, 136 for I-frame) = no overlay
	size_t baselineSize = isIFrame ? 136 : 128;
	if (length <= baselineSize)
	{
		std::lock_guard<std::mutex> lock(m_DetectionMutex);
		m_CurrentDetections.Detections.clear();
		m_CurrentDetections.Timestamp = std::chrono::steady_clock::now();
		m_CurrentDetections.HasData = true;
		return;
	}

	// Parse outer TLV structure starting after the marker
	// The structure is: marker(8) + counter(4) + TLV stream
	size_t tlvStart = markerOffset + 12; // 8-byte marker + 4-byte counter

	ParseOuterTLV(data + tlvStart, length - tlvStart);
}

void ReolinkBaichuanClient::ParseOuterTLV(const uint8_t* data, size_t length)
{
	// Walk outer TLV looking for the LZ4 compressed block
	// Structure: type(1) + length(2) + value(length)
	// We're looking for the pattern: type=0xFF wrapping type=2 wrapping type=2 with LZ4F magic

	size_t offset = 0;
	while (offset + 3 < length)
	{
		uint8_t type = data[offset];
		if (type == 0) break; // End marker

		uint16_t tlvLen = *(const uint16_t*)(data + offset + 1);
		size_t valueStart = offset + 3;

		if (valueStart + tlvLen > length) break;

		if (type == 0xFF)
		{
			// Recurse into the 0xFF wrapper
			ParseOuterTLV(data + valueStart, tlvLen);
			return; // Only one top-level 0xFF expected
		}
		else if (type == 0x02)
		{
			// This might contain the LZ4 data or another nested TLV
			const uint8_t* value = data + valueStart;
			if (tlvLen >= 4 && memcmp(value, LZ4F_MAGIC, 4) == 0)
			{
				// Direct LZ4 compressed data
				DecompressAndParseTLV(value, tlvLen);
				return;
			}
			else
			{
				// Nested TLV — look for type=2 with LZ4 inside
				size_t innerOff = 0;
				while (innerOff + 3 < tlvLen)
				{
					uint8_t innerType = value[innerOff];
					if (innerType == 0) break;
					uint16_t innerLen = *(const uint16_t*)(value + innerOff + 1);
					size_t innerVal = innerOff + 3;
					if (innerVal + innerLen > tlvLen) break;

					if (innerType == 0x02 && innerLen >= 4 &&
						memcmp(value + innerVal, LZ4F_MAGIC, 4) == 0)
					{
						DecompressAndParseTLV(value + innerVal, innerLen);
						return;
					}
					innerOff = innerVal + innerLen;
				}
			}
		}

		offset = valueStart + tlvLen;
	}

	// No LZ4 data found — clear detections
	std::lock_guard<std::mutex> lock(m_DetectionMutex);
	m_CurrentDetections.Detections.clear();
	m_CurrentDetections.Timestamp = std::chrono::steady_clock::now();
	m_CurrentDetections.HasData = true;
}

void ReolinkBaichuanClient::DecompressAndParseTLV(const uint8_t* data, size_t length)
{
	// LZ4 Frame decompression
	LZ4F_dctx* dctx = nullptr;
	LZ4F_errorCode_t err = LZ4F_createDecompressionContext(&dctx, LZ4F_VERSION);
	if (LZ4F_isError(err))
	{
		std::lock_guard<std::mutex> lock(m_DetectionMutex);
		m_CurrentDetections.Detections.clear();
		m_CurrentDetections.Timestamp = std::chrono::steady_clock::now();
		m_CurrentDetections.HasData = true;
		return;
	}

	std::vector<uint8_t> decompressed;
	decompressed.reserve(4096);

	size_t srcOffset = 0;
	uint8_t chunk[4096];

	while (srcOffset < length)
	{
		size_t srcSize = length - srcOffset;
		size_t dstSize = sizeof(chunk);
		size_t consumed = LZ4F_decompress(dctx, chunk, &dstSize, data + srcOffset, &srcSize, nullptr);

		if (LZ4F_isError(consumed))
			break;

		if (dstSize > 0)
			decompressed.insert(decompressed.end(), chunk, chunk + dstSize);

		srcOffset += srcSize;
		if (consumed == 0) break;
	}

	LZ4F_freeDecompressionContext(dctx);

	if (decompressed.empty())
	{
		std::lock_guard<std::mutex> lock(m_DetectionMutex);
		m_CurrentDetections.Detections.clear();
		m_CurrentDetections.Timestamp = std::chrono::steady_clock::now();
		m_CurrentDetections.HasData = true;
		return;
	}

	// Parse decompressed TLV for detection boxes
	ParseInnerDetectionTLV(decompressed.data(), decompressed.size());
}

void ReolinkBaichuanClient::ParseInnerDetectionTLV(const uint8_t* data, size_t length)
{
	// The inner TLV uses nested context: type1 (class), type2 (view), type3 (leaf)
	// type1: 1=people, 2=vehicle, 3=animal
	// Leaf boxes: type=4 with length 10/13/14, or type=2 with length=10

	std::vector<ReolinkDetection> detections;
	ParseTLVRecursive(data, length, 0, 0, detections);

	// Update state
	std::lock_guard<std::mutex> lock(m_DetectionMutex);
	m_CurrentDetections.Detections = std::move(detections);
	m_CurrentDetections.Timestamp = std::chrono::steady_clock::now();
	m_CurrentDetections.HasData = true;
}

void ReolinkBaichuanClient::ParseTLVRecursive(const uint8_t* data, size_t length,
	uint8_t contextType1, uint8_t contextType2, std::vector<ReolinkDetection>& detections)
{
	size_t offset = 0;

	while (offset + 3 <= length)
	{
		uint8_t type = data[offset];
		if (type == 0) break;

		uint16_t tlvLen = *(const uint16_t*)(data + offset + 1);
		size_t valueStart = offset + 3;

		if (valueStart + tlvLen > length) break;

		// Check if this is a box record
		bool isBox = false;
		if (contextType1 != 0 && contextType2 != 0)
		{
			if ((type == 4 && (tlvLen == 10 || tlvLen == 13 || tlvLen == 14)) ||
				(type == 2 && tlvLen == 10))
			{
				isBox = true;
			}
		}

		if (isBox)
		{
			// Parse box: x1(2) + y1(2) + x2(2) + y2(2) + conf(2)
			const uint8_t* box = data + valueStart;
			uint16_t x1 = *(const uint16_t*)(box + 0);
			uint16_t y1 = *(const uint16_t*)(box + 2);
			uint16_t x2 = *(const uint16_t*)(box + 4);
			uint16_t y2 = *(const uint16_t*)(box + 6);
			uint16_t conf = *(const uint16_t*)(box + 8);

			if (x2 > x1 && y2 > y1 && conf > 0 && conf <= 100)
			{
				ReolinkDetection det;
				det.X1 = (float)x1 / (float)m_AiWidth;
				det.Y1 = (float)y1 / (float)m_AiHeight;
				det.X2 = (float)x2 / (float)m_AiWidth;
				det.Y2 = (float)y2 / (float)m_AiHeight;
				det.Confidence = (float)conf / 100.0f;

				switch (contextType1)
				{
				case 1: det.DetectionClass = ReolinkDetection::People; break;
				case 2: det.DetectionClass = ReolinkDetection::Vehicle; break;
				case 3: det.DetectionClass = ReolinkDetection::Animal; break;
				default: det.DetectionClass = ReolinkDetection::People; break;
				}

				detections.push_back(det);
			}
		}
		else
		{
			// Nested TLV — update context and recurse
			uint8_t newType1 = contextType1;
			uint8_t newType2 = contextType2;

			if (contextType1 == 0)
				newType1 = type;
			else if (contextType2 == 0)
				newType2 = type;

			ParseTLVRecursive(data + valueStart, tlvLen, newType1, newType2, detections);
		}

		offset = valueStart + tlvLen;
	}
}

bool ReolinkBaichuanClient::ParseBcMediaFrame(const std::vector<uint8_t>& frame)
{
	// Handled by ParseBcMediaFrames
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

}}
