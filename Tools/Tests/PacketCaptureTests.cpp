#include "PacketCapture.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <stdexcept>
#include <thread>

static void Require(bool condition)
{
	if (!condition) throw std::runtime_error("Packet capture test failed");
}

int main()
{
	const auto directory = std::filesystem::temp_directory_path() /
		("witness-packet-capture-test-" + std::to_string(
			std::chrono::steady_clock::now().time_since_epoch().count()));
	{
		Witness::Camera::PacketCapture capture(directory, 1);
		Require(capture.Ready());
		const uint8_t input[] = { 1, 2, 3, 4 };
		const uint8_t output[] = { 5, 6, 7 };
		capture.AddMetadata("{\"type\":\"capture\"");
		capture.AddInput("{\"type\":\"input\"", input, sizeof(input));
		capture.AddOutput("{\"type\":\"partial\"", output, sizeof(output));
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (!capture.Complete() && std::chrono::steady_clock::now() < deadline)
			std::this_thread::sleep_for(std::chrono::milliseconds(25));
		Require(capture.Complete());
	}
	const auto read = [](const auto& path)
	{
		std::ifstream file(path, std::ios::binary);
		return std::string(std::istreambuf_iterator<char>(file), {});
	};
	const auto manifest = read(directory / "events.jsonl");
	Require(manifest.find("\"type\":\"capture\"}") != std::string::npos);
	Require(manifest.find("\"type\":\"input\",\"offset\":0,\"bytes\":4}") != std::string::npos);
	Require(manifest.find("\"type\":\"partial\",\"offset\":0,\"bytes\":3}") != std::string::npos);
	Require(manifest.find("\"queueRejectedRecords\":0") != std::string::npos);
	Require(read(directory / "input.bin") == std::string("\1\2\3\4", 4));
	Require(read(directory / "output.bin") == std::string("\5\6\7", 3));
	std::filesystem::remove_all(directory);
}
